/**
 * @file afe_pipeline.c
 * @brief Unified AFE pipeline: AEC + WakeNet + VAD
 */

#include "afe_pipeline.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "runtime_config.h"
#include "sdkconfig.h"
#include "voice_client.h"

static const char *TAG = "AFE_PIPE";

// Configuration
#define AFE_SAMPLE_RATE 16000
#define AFE_MIC_GAIN_SHIFT 10
#define REF_BUFFER_SIZE (16000 * 2)
#define AFE_TASK_STACK 12288
#define AFE_FEED_PRIO 6
#define AFE_FETCH_PRIO 7
#define WAKE_GUARD_MS 1500
#define WAKE_SENS_LEVEL_MAX 10U
#define WAKE_SENS_THRESHOLD_STRICT 0.90f
#define WAKE_SENS_THRESHOLD_SENSITIVE 0.40f

// 10ms frame for AEC synchronization
#define AEC_FRAME_SAMPLES 160                                 // 10ms @ 16kHz
#define AEC_FRAME_BYTES (AEC_FRAME_SAMPLES * sizeof(int16_t)) // 320 bytes

extern i2s_chan_handle_t s_i2s_rx_chan; // Microphone

typedef struct {
  const esp_afe_sr_iface_t *iface;
  esp_afe_sr_data_t *data;
  afe_config_t *config;

  int feed_chunk;
  int fetch_chunk;
  int feed_channels;
  int sample_rate;

  int32_t *i2s_raw;
  int16_t *mic_pcm;
  int16_t *ref_pcm;
  int16_t *feed_buffer;

  int16_t *ref_buffer;
  size_t ref_head;
  size_t ref_tail;
  size_t ref_count;
  size_t ref_capacity;
  size_t ref_delay_samples;
  size_t ref_delay_remaining;

  TaskHandle_t task;
  TaskHandle_t feed_task;
  TaskHandle_t fetch_task;
  bool initialized;
  bool session_active;
  bool session_start_inflight;
  bool processing_session;
  volatile bool reset_requested;
  int64_t wake_guard_until_us;
  bool wake_guard_logged;

  // TX buffer to split AFE data into 10ms frames
  int16_t *tx_buffer;     // Buffer for accumulating AFE data
  size_t tx_buffer_size;  // Size in samples
  size_t tx_buffer_count; // Current number of samples in buffer
} afe_state_t;

static afe_state_t s_afe = {0};

static afe_wake_word_detected_cb_t s_on_wake_cb = NULL;
static afe_session_end_cb_t s_on_end_cb = NULL;

static SemaphoreHandle_t s_ref_mutex = NULL;

static float wake_sensitivity_level_to_threshold(uint32_t level) {
  if (level > WAKE_SENS_LEVEL_MAX) {
    level = WAKE_SENS_LEVEL_MAX;
  }

  // Human scale:
  // 0 = strictest detection (highest threshold), 10 = most sensitive.
  const float step =
      (WAKE_SENS_THRESHOLD_STRICT - WAKE_SENS_THRESHOLD_SENSITIVE) /
      (float)WAKE_SENS_LEVEL_MAX;
  return WAKE_SENS_THRESHOLD_STRICT - ((float)level * step);
}

static void wake_guard_arm(void) {
  s_afe.wake_guard_until_us =
      esp_timer_get_time() + (int64_t)WAKE_GUARD_MS * 1000;
  s_afe.wake_guard_logged = false;
}

static void ref_buffer_reset(void) {
  if (!s_ref_mutex ||
      xSemaphoreTake(s_ref_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }

  s_afe.ref_head = 0;
  s_afe.ref_tail = 0;
  s_afe.ref_count = 0;
  s_afe.ref_delay_remaining = s_afe.ref_delay_samples;

  xSemaphoreGive(s_ref_mutex);
}

static void afe_set_processing_mode(bool session_active) {
  if (!s_afe.iface || !s_afe.data) {
    return;
  }

  if (session_active) {
    ref_buffer_reset();
#if CONFIG_AEC_ENABLED
    if (s_afe.iface->enable_aec) {
      s_afe.iface->enable_aec(s_afe.data);
    }
#endif
    if (s_afe.iface->enable_ns) {
      s_afe.iface->enable_ns(s_afe.data);
    }
    if (s_afe.iface->enable_vad) {
      s_afe.iface->enable_vad(s_afe.data);
    }
    if (s_afe.iface->enable_wakenet) {
      s_afe.iface->enable_wakenet(s_afe.data);
    }
    s_afe.processing_session = true;
#if CONFIG_AEC_ENABLED
    ESP_LOGI(TAG, "AFE session mode: AEC/NS/VAD enabled");
#else
    ESP_LOGI(TAG, "AFE session mode: NS/VAD enabled (AEC disabled)");
#endif
  } else {
#if CONFIG_AEC_ENABLED
    if (s_afe.iface->disable_aec) {
      s_afe.iface->disable_aec(s_afe.data);
    }
#endif
    // Keep NS enabled in idle mode to reduce false wake-word triggers on
    // background microphone noise.
    if (s_afe.iface->enable_ns) {
      s_afe.iface->enable_ns(s_afe.data);
    }
    if (s_afe.iface->enable_vad) {
      s_afe.iface->enable_vad(s_afe.data);
    }
    if (s_afe.iface->enable_wakenet) {
      s_afe.iface->enable_wakenet(s_afe.data);
    }
    s_afe.processing_session = false;
#if CONFIG_AEC_ENABLED
    ESP_LOGI(TAG, "AFE idle mode: AEC disabled, NS/VAD/WakeNet enabled");
#else
    ESP_LOGI(TAG, "AFE idle mode: NS/VAD/WakeNet enabled (AEC disabled)");
#endif
  }
}

// Simple ring buffer write implementation
static void ref_buffer_write(const int16_t *data, size_t count) {
  if (!s_afe.ref_buffer || s_afe.ref_capacity == 0 || !s_ref_mutex) {
    return;
  }

  if (xSemaphoreTake(s_ref_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return; // Drop reference data if we can't get lock quickly
  }

  for (size_t i = 0; i < count; i++) {
    s_afe.ref_buffer[s_afe.ref_head] = data[i];
    s_afe.ref_head = (s_afe.ref_head + 1) % s_afe.ref_capacity;

    if (s_afe.ref_count < s_afe.ref_capacity) {
      s_afe.ref_count++;
    } else {
      s_afe.ref_tail = (s_afe.ref_tail + 1) % s_afe.ref_capacity;
    }
  }

  xSemaphoreGive(s_ref_mutex);
}

// Read from ring buffer
static void ref_buffer_read(int16_t *data, size_t count) {
  if (!s_afe.processing_session) {
    memset(data, 0, count * sizeof(int16_t));
    return;
  }

  size_t written = 0;

  if (s_afe.ref_delay_remaining > 0) {
    size_t delay = s_afe.ref_delay_remaining;
    if (delay > count) {
      delay = count;
    }
    memset(data, 0, delay * sizeof(int16_t));
    s_afe.ref_delay_remaining -= delay;
    written += delay;
  }

  if (written == count) {
    return;
  }

  if (!s_afe.ref_buffer || s_afe.ref_capacity == 0) {
    memset(data + written, 0, (count - written) * sizeof(int16_t));
    return;
  }

  if (!s_ref_mutex ||
      xSemaphoreTake(s_ref_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    memset(data + written, 0, (count - written) * sizeof(int16_t));
    return;
  }

  size_t remaining = count - written;
  if (s_afe.ref_count < remaining) {
    size_t available = s_afe.ref_count;
    for (size_t i = 0; i < available; i++) {
      data[written + i] = s_afe.ref_buffer[s_afe.ref_tail];
      s_afe.ref_tail = (s_afe.ref_tail + 1) % s_afe.ref_capacity;
    }
    memset(data + written + available, 0,
           (remaining - available) * sizeof(int16_t));
    s_afe.ref_count = 0;
  } else {
    for (size_t i = 0; i < remaining; i++) {
      data[written + i] = s_afe.ref_buffer[s_afe.ref_tail];
      s_afe.ref_tail = (s_afe.ref_tail + 1) % s_afe.ref_capacity;
    }
    s_afe.ref_count -= remaining;
  }

  xSemaphoreGive(s_ref_mutex);
}

static inline int16_t i2s_sample32_to_pcm16(int32_t sample) {
  sample >>= AFE_MIC_GAIN_SHIFT;
  if (sample > INT16_MAX) {
    sample = INT16_MAX;
  } else if (sample < INT16_MIN) {
    sample = INT16_MIN;
  }
  return (int16_t)sample;
}

static void start_voice_session_task(void *arg) {
  (void)arg;

  esp_err_t err = voice_client_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start voice session: %s",
             esp_err_to_name(err));
    s_afe.session_active = false;
    s_afe.session_start_inflight = false;
    afe_set_processing_mode(false);

    // Call callback to indicate error (LED + beep)
    if (s_on_end_cb) {
      s_on_end_cb();
    }
  }
  vTaskDelete(NULL);
}

static void handle_fetch_result(afe_fetch_result_t *res) {
  if (!res || !res->data) {
    return;
  }

  // Wake-word detection (only when session is NOT active)
  if (res->wakeup_state == WAKENET_DETECTED && !s_afe.session_active &&
      !s_afe.session_start_inflight) {
    int64_t now = esp_timer_get_time();
    if (now < s_afe.wake_guard_until_us) {
      if (!s_afe.wake_guard_logged) {
        ESP_LOGW(TAG, "WakeNet ignored during warmup (%d ms)", WAKE_GUARD_MS);
        s_afe.wake_guard_logged = true;
      }
    } else {
      ESP_LOGI(TAG, ">>> WAKE WORD DETECTED <<<");
      bool should_start = true;
      if (s_on_wake_cb) {
        should_start = s_on_wake_cb(s_afe.config->wakenet_model_name, 0);
      }
      if (should_start) {
        s_afe.session_start_inflight = true;
        xTaskCreatePinnedToCore(start_voice_session_task, "voice_start", 4096,
                                NULL, 5, NULL, 0);
      }
    }
  }
  // Audio is NOT sent through AFE; direct I2S capture is used
}

static void afe_feed_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "AFE feed task started");

  int accumulated_samples = 0;
  int mic_slot = -1; // 0 = left, 1 = right
  bool mic_slot_logged = false;

  while (s_afe.initialized) {
    if (s_afe.reset_requested) {
      ref_buffer_reset();
      if (s_afe.iface && s_afe.iface->reset_buffer) {
        s_afe.iface->reset_buffer(s_afe.data);
      }
      accumulated_samples = 0;
      mic_slot = -1;
      mic_slot_logged = false;
      s_afe.reset_requested = false;
    }

    // 1. I2S READ (Stereo)
    // During an active session, mic_capture_task reads I2S directly
    if (s_afe.session_active) {
      static bool s_session_active_logged = false;
      if (!s_session_active_logged) {
        ESP_LOGI(TAG, "AFE feed suspended (session active)");
        s_session_active_logged = true;
      }
      vTaskDelay(pdMS_TO_TICKS(50)); // Sleep while session is active
      continue;
    } else {
      static bool s_session_inactive_logged =
          true; // Initial state - active
      if (s_session_inactive_logged) {
        ESP_LOGI(TAG, "AFE feed resuming (session inactive)");
        s_session_inactive_logged = false;
      }
    }

    int samples_needed = s_afe.feed_chunk - accumulated_samples;
    size_t bytes_to_read = samples_needed * 2 * sizeof(int32_t);
    size_t bytes_read = 0;

    // portMAX_DELAY replaced with short timeout to check session_active
    esp_err_t err =
        i2s_channel_read(s_i2s_rx_chan, s_afe.i2s_raw, bytes_to_read,
                         &bytes_read, pdMS_TO_TICKS(20));

    if (err == ESP_OK && bytes_read > 0) {
      size_t frames_read = bytes_read / (2 * sizeof(int32_t));
      int64_t energy_left = 0;
      int64_t energy_right = 0;

      for (size_t i = 0; i < frames_read; i++) {
        int32_t left = s_afe.i2s_raw[i * 2];
        int32_t right = s_afe.i2s_raw[i * 2 + 1];
        energy_left += llabs((long long)left);
        energy_right += llabs((long long)right);
      }

      if (mic_slot < 0 && (energy_left > 0 || energy_right > 0)) {
        mic_slot = (energy_left >= energy_right) ? 0 : 1;
        ESP_LOGI(TAG, "I2S mic slot selected: %s",
                 mic_slot == 0 ? "LEFT" : "RIGHT");
        mic_slot_logged = true;
      }

      int selected_slot = (mic_slot >= 0) ? mic_slot : 0;

      for (size_t i = 0; i < frames_read; i++) {
        if (accumulated_samples < s_afe.feed_chunk) {
          int32_t sample = s_afe.i2s_raw[i * 2 + selected_slot];
          s_afe.mic_pcm[accumulated_samples++] = i2s_sample32_to_pcm16(sample);
        }
      }
    } else {
      // If no data is read, yield to RTOS to avoid stalling the core
      vTaskDelay(1);
    }

    // 2. FEED AFE
    if (accumulated_samples >= s_afe.feed_chunk) {
      if (s_afe.feed_channels > 1 && s_afe.ref_pcm) {
        ref_buffer_read(s_afe.ref_pcm, s_afe.feed_chunk);
      }

      for (int i = 0; i < s_afe.feed_chunk; i++) {
        s_afe.feed_buffer[i * s_afe.feed_channels + 0] = s_afe.mic_pcm[i];
        if (s_afe.feed_channels > 1) {
          s_afe.feed_buffer[i * s_afe.feed_channels + 1] = s_afe.ref_pcm[i];
        }
      }

      int feed_ret = s_afe.iface->feed(s_afe.data, s_afe.feed_buffer);
      accumulated_samples = 0;
      if (feed_ret <= 0) {
        // Give fetch task a chance to release buffers
        vTaskDelay(1);
      }
    }
  }

  ESP_LOGI(TAG, "AFE feed task stopped");
  s_afe.feed_task = NULL;
  vTaskDelete(NULL);
}

static void afe_fetch_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "AFE fetch task started");

  while (s_afe.initialized) {
    afe_fetch_result_t *res = s_afe.iface->fetch(s_afe.data);
    handle_fetch_result(res);
  }

  ESP_LOGI(TAG, "AFE fetch task stopped");
  s_afe.fetch_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t afe_pipeline_init(srmodel_list_t *models) {
  if (s_afe.initialized) {
    return ESP_OK;
  }

  if (!models || models->num == 0) {
    ESP_LOGE(TAG, "No models available for AFE");
    return ESP_ERR_INVALID_STATE;
  }

  s_afe.config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
  if (!s_afe.config) {
    ESP_LOGE(TAG, "Failed to create AFE config");
    return ESP_FAIL;
  }

  s_afe.config->pcm_config.sample_rate = AFE_SAMPLE_RATE;
  s_afe.config->pcm_config.total_ch_num = 2;
  s_afe.config->pcm_config.mic_num = 1;
  s_afe.config->pcm_config.ref_num = 1;

#if CONFIG_AEC_ENABLED
  s_afe.config->aec_init = true;
  s_afe.config->aec_mode = AEC_MODE_SR_HIGH_PERF;
  s_afe.config->aec_filter_length =
      5; // Reverted to safe value due to WDT crash
#else
  s_afe.config->aec_init = false;
#endif

  // Speech Enhancement (AGC) DISABLED - server receives raw PCM unprocessed
  // AFE is used only for WakeNet detection
  s_afe.config->se_init = false;
  s_afe.config->ns_init = true; // NS is needed only for WakeNet
  s_afe.config->vad_init = true;
  s_afe.config->wakenet_init = true;

  const runtime_config_t *runtime_cfg = runtime_config_get();
  uint32_t wake_level = 6;
  int wakenet_mode = DET_MODE_95;
  if (runtime_cfg) {
    wake_level = runtime_cfg->wake_sensitivity_level;
    if (strcmp(runtime_cfg->wake_detection_mode, "normal") == 0) {
      wakenet_mode = DET_MODE_90;
    }
  }
  s_afe.config->wakenet_mode = wakenet_mode;
  s_afe.config->fixed_first_channel = true;
  s_afe.config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  s_afe.config->afe_perferred_core = 1;
  s_afe.config->afe_perferred_priority =
      7; // Highest priority for internal processing

  const char *desired_model = NULL;
#ifdef CONFIG_WAKE_WORD_MODEL
  desired_model = CONFIG_WAKE_WORD_MODEL;
#endif

  char *wakenet_name = esp_srmodel_filter(models, ESP_WN_PREFIX, desired_model);
  if (!wakenet_name) {
    wakenet_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
  }
  s_afe.config->wakenet_model_name = wakenet_name;

  s_afe.config = afe_config_check(s_afe.config);

  s_afe.iface = esp_afe_handle_from_config(s_afe.config);
  if (!s_afe.iface) {
    ESP_LOGE(TAG, "Failed to create AFE interface");
    return ESP_FAIL;
  }

  s_afe.data = s_afe.iface->create_from_config(s_afe.config);
  if (!s_afe.data) {
    ESP_LOGE(TAG, "Failed to create AFE data");
    return ESP_FAIL;
  }

  float wake_threshold = wake_sensitivity_level_to_threshold(wake_level);

  if (s_afe.iface->set_wakenet_threshold) {
    int set_ret = s_afe.iface->set_wakenet_threshold(s_afe.data, 1, wake_threshold);
    if (set_ret < 0) {
      ESP_LOGW(TAG, "Failed to apply wake threshold %.3f (level=%u)",
               (double)wake_threshold, (unsigned)wake_level);
    } else {
      ESP_LOGI(TAG, "Wake sensitivity level=%u -> threshold=%.3f",
               (unsigned)wake_level, (double)wake_threshold);
    }
  }
  ESP_LOGI(TAG, "Wake detection mode=%s (%s)",
           (wakenet_mode == DET_MODE_90) ? "normal" : "strict",
           (wakenet_mode == DET_MODE_90) ? "DET_MODE_90" : "DET_MODE_95");

  s_afe.feed_chunk = s_afe.iface->get_feed_chunksize(s_afe.data);
  s_afe.fetch_chunk = s_afe.iface->get_fetch_chunksize(s_afe.data);
  if (s_afe.iface->get_feed_channel_num) {
    s_afe.feed_channels = s_afe.iface->get_feed_channel_num(s_afe.data);
  } else {
    s_afe.feed_channels = 2;
  }
  s_afe.sample_rate = s_afe.iface->get_samp_rate
                          ? s_afe.iface->get_samp_rate(s_afe.data)
                          : AFE_SAMPLE_RATE;

  ESP_LOGI(TAG, "AFE feed_chunk=%d fetch_chunk=%d channels=%d rate=%d",
           s_afe.feed_chunk, s_afe.fetch_chunk, s_afe.feed_channels,
           s_afe.sample_rate);

  if (s_afe.sample_rate > 0) {
    ESP_LOGI(TAG, "AEC Filter Length: %d frames (approx %d ms)",
             s_afe.config->aec_filter_length,
             s_afe.config->aec_filter_length * s_afe.feed_chunk /
                 (s_afe.sample_rate / 1000));
  }

#ifdef CONFIG_AEC_REF_DELAY_MS
  if (s_afe.sample_rate > 0) {
    s_afe.ref_delay_samples =
        (size_t)((s_afe.sample_rate * CONFIG_AEC_REF_DELAY_MS) / 1000);
  } else {
    s_afe.ref_delay_samples = 0;
  }
#else
  s_afe.ref_delay_samples = 0;
#endif
  s_afe.ref_delay_remaining = s_afe.ref_delay_samples;
  ESP_LOGI(TAG, "AEC Ref Delay: %u samples (~%u ms)",
           (unsigned)s_afe.ref_delay_samples,
           (unsigned)((s_afe.sample_rate > 0)
                          ? (s_afe.ref_delay_samples * 1000 /
                             (unsigned)s_afe.sample_rate)
                          : 0));

  s_afe.i2s_raw =
      heap_caps_malloc(s_afe.feed_chunk * 2 * sizeof(int32_t), MALLOC_CAP_DMA);
  s_afe.mic_pcm =
      heap_caps_malloc(s_afe.feed_chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  s_afe.ref_pcm =
      heap_caps_malloc(s_afe.feed_chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  s_afe.feed_buffer =
      heap_caps_malloc(s_afe.feed_chunk * s_afe.feed_channels * sizeof(int16_t),
                       MALLOC_CAP_SPIRAM);

  s_afe.ref_capacity = REF_BUFFER_SIZE / sizeof(int16_t);
  s_afe.ref_buffer = heap_caps_malloc(REF_BUFFER_SIZE, MALLOC_CAP_SPIRAM);

  // TX buffer to split AFE data into 10ms frames
  s_afe.tx_buffer_size = AEC_FRAME_SAMPLES * 2; // Reserve for 2 frames
  s_afe.tx_buffer = heap_caps_malloc(s_afe.tx_buffer_size * sizeof(int16_t),
                                     MALLOC_CAP_INTERNAL);
  s_afe.tx_buffer_count = 0;

  if (!s_afe.i2s_raw || !s_afe.mic_pcm || !s_afe.ref_pcm ||
      !s_afe.feed_buffer || !s_afe.ref_buffer || !s_afe.tx_buffer) {
    ESP_LOGE(TAG, "Failed to allocate AFE buffers");
    return ESP_ERR_NO_MEM;
  }

  s_afe.ref_head = 0;
  s_afe.ref_tail = 0;
  s_afe.ref_count = 0;

  if (!s_ref_mutex) {
    s_ref_mutex = xSemaphoreCreateMutex();
    if (!s_ref_mutex) {
      ESP_LOGE(TAG, "Failed to create ref mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  afe_set_processing_mode(false);
  s_afe.initialized = true;
  return ESP_OK;
}

esp_err_t afe_pipeline_start(void) {
  if (!s_afe.initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (s_afe.feed_task || s_afe.fetch_task) {
    return ESP_OK;
  }
  BaseType_t core = 1;
#if portNUM_PROCESSORS > 1
  core = 1; // Move afe_task to Core 1 to avoid fighting with WiFi (Core 0)
#endif
  wake_guard_arm();
  BaseType_t feed_ret =
      xTaskCreatePinnedToCore(afe_feed_task, "afe_feed_task", AFE_TASK_STACK,
                              NULL, AFE_FEED_PRIO, &s_afe.feed_task, core);
  if (feed_ret != pdPASS) {
    return ESP_FAIL;
  }

  BaseType_t fetch_ret =
      xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch_task", AFE_TASK_STACK,
                              NULL, AFE_FETCH_PRIO, &s_afe.fetch_task, core);
  if (fetch_ret != pdPASS) {
    vTaskDelete(s_afe.feed_task);
    s_afe.feed_task = NULL;
    return ESP_FAIL;
  }

  return ESP_OK;
}

void afe_pipeline_feed_reference(const int16_t *data, size_t len) {
  if (!s_afe.initialized || !data || len == 0) {
    return;
  }
  size_t count = len / sizeof(int16_t);
  ref_buffer_write(data, count);
}

void afe_pipeline_reset(void) {
  if (!s_afe.initialized) {
    return;
  }
  wake_guard_arm();
  s_afe.reset_requested = true;
  s_afe.tx_buffer_count = 0; // Reset TX buffer on interruption
}

void afe_pipeline_register_callbacks(afe_wake_word_detected_cb_t on_wake,
                                     afe_session_end_cb_t on_end) {
  s_on_wake_cb = on_wake;
  s_on_end_cb = on_end;
}

void afe_pipeline_notify_session_start(void) {
  s_afe.session_active = true;
  s_afe.session_start_inflight = false;
  afe_set_processing_mode(true);
}

void afe_pipeline_notify_session_end(void) {
  s_afe.session_active = false;
  s_afe.session_start_inflight = false;
  afe_set_processing_mode(false);
  afe_pipeline_reset();

  if (s_on_end_cb) {
    s_on_end_cb();
  }
}

void afe_pipeline_manual_wakeup(void) {
  if (s_afe.session_active || s_afe.session_start_inflight) {
    ESP_LOGW(TAG, "Manual wakeup ignored: session already active");
    return;
  }

  // Check Wake Guard (to avoid triggering right after session end)
  int64_t now = esp_timer_get_time();
  if (now < s_afe.wake_guard_until_us) {
    ESP_LOGW(TAG, "Manual wakeup ignored: wake guard active");
    return;
  }

  ESP_LOGI(TAG, ">>> MANUAL WAKEUP <<<");
  bool should_start = true;

  // Call callback (sound/LED)
  if (s_on_wake_cb) {
    should_start = s_on_wake_cb("BUTTON", 0);
  }

  if (should_start) {
    s_afe.session_start_inflight = true;
    xTaskCreatePinnedToCore(start_voice_session_task, "voice_start", 4096, NULL,
                            5, NULL, 0);
  }
}
