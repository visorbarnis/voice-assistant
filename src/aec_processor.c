/**
 * @file aec_processor.c
 * @brief AEC processor implementation based on ESP-AFE
 *
 * Uses ESP-AFE in Voice Communication mode (AFE_TYPE_VC) for
 * acoustic echo cancellation without speech recognition.
 */

#include "aec_processor.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static const char *TAG = "AEC_PROC";

// ============================================================================
// Configuration
// ============================================================================

// Reference buffer size (playback-to-capture delay ~100-300ms)
// 16000 samples/sec * 2 bytes * 0.5 sec = 16000 bytes
#define REF_BUFFER_SIZE (16000 * 2)

// ============================================================================
// Internal state
// ============================================================================

typedef struct {
  const esp_afe_sr_iface_t *afe_handle;
  esp_afe_sr_data_t *afe_data;
  afe_config_t *afe_config;

  // Ring buffer for reference signal
  int16_t *ref_buffer;
  size_t ref_head;
  size_t ref_tail;
  size_t ref_count;
  size_t ref_capacity;

  // Temporary buffer for interleaving (Feed data)
  int16_t *feed_buffer;
  size_t feed_buffer_size;
  int feed_chunk_size;
  int fetch_chunk_size;
  int feed_channels;
  int sample_rate;

  bool is_initialized;
} aec_state_t;

static aec_state_t s_aec = {0};

// ============================================================================
// Internal functions
// ============================================================================

// Simple ring buffer write implementation
static void ref_buffer_write(const int16_t *data, size_t count) {
  for (size_t i = 0; i < count; i++) {
    s_aec.ref_buffer[s_aec.ref_head] = data[i];
    s_aec.ref_head = (s_aec.ref_head + 1) % s_aec.ref_capacity;

    if (s_aec.ref_count < s_aec.ref_capacity) {
      s_aec.ref_count++;
    } else {
      // Overflow: shift tail (drop old data)
      s_aec.ref_tail = (s_aec.ref_tail + 1) % s_aec.ref_capacity;
    }
  }
}

// Read from ring buffer
static void ref_buffer_read(int16_t *data, size_t count) {
  if (s_aec.ref_count < count) {
    // If data is insufficient, pad with zeros (silence)
    size_t available = s_aec.ref_count;
    for (size_t i = 0; i < available; i++) {
      data[i] = s_aec.ref_buffer[s_aec.ref_tail];
      s_aec.ref_tail = (s_aec.ref_tail + 1) % s_aec.ref_capacity;
    }
    // Fill the rest with zeros
    memset(data + available, 0, (count - available) * sizeof(int16_t));
    s_aec.ref_count = 0;
  } else {
    for (size_t i = 0; i < count; i++) {
      data[i] = s_aec.ref_buffer[s_aec.ref_tail];
      s_aec.ref_tail = (s_aec.ref_tail + 1) % s_aec.ref_capacity;
    }
    s_aec.ref_count -= count;
  }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t aec_processor_init(int sample_rate) {
  if (s_aec.is_initialized) {
    ESP_LOGW(TAG, "AEC is already initialized");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Initializing AEC Processor (sample_rate=%d)...", sample_rate);

  // 1. AFE configuration
  // "MR" = Mic (1ch) + Reference (1ch)
  // AFE_TYPE_VC = Voice Communication (for AEC without WakeNet)
  // AFE_MODE_HIGH_PERF = high quality
  s_aec.afe_config =
      afe_config_init("MR", NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
  if (!s_aec.afe_config) {
    ESP_LOGE(TAG, "Failed to create AFE configuration");
    return ESP_FAIL;
  }

  // Configure parameters
  s_aec.afe_config->pcm_config.sample_rate = sample_rate;
  s_aec.afe_config->pcm_config.total_ch_num = 2; // Mic + Ref
  s_aec.afe_config->pcm_config.mic_num = 1;
  s_aec.afe_config->pcm_config.ref_num = 1;

  s_aec.afe_config->aec_init = true;      // AEC enabled
  s_aec.afe_config->se_init = true;       // Noise Suppression enabled
  s_aec.afe_config->vad_init = true;      // VAD enabled
  s_aec.afe_config->wakenet_init = false; // WakeNet not needed for AEC

  s_aec.afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  s_aec.afe_config->afe_perferred_core = 1; // Core 1
  s_aec.afe_config->afe_perferred_priority = 5;

  // 2. Create AFE handle
  s_aec.afe_handle = esp_afe_handle_from_config(s_aec.afe_config);
  if (!s_aec.afe_handle) {
    ESP_LOGE(TAG, "Failed to create AFE handle");
    return ESP_FAIL;
  }

  // 3. Create AFE data
  s_aec.afe_data = s_aec.afe_handle->create_from_config(s_aec.afe_config);
  if (!s_aec.afe_data) {
    ESP_LOGE(TAG, "Failed to create AFE data");
    return ESP_FAIL;
  }

  // 4. Get chunk size
  s_aec.feed_chunk_size = s_aec.afe_handle->get_feed_chunksize(s_aec.afe_data);
  if (s_aec.afe_handle->get_fetch_chunksize) {
    s_aec.fetch_chunk_size =
        s_aec.afe_handle->get_fetch_chunksize(s_aec.afe_data);
  } else {
    s_aec.fetch_chunk_size = s_aec.feed_chunk_size;
  }

  if (s_aec.afe_handle->get_feed_channel_num) {
    s_aec.feed_channels =
        s_aec.afe_handle->get_feed_channel_num(s_aec.afe_data);
  } else if (s_aec.afe_handle->get_channel_num) {
    s_aec.feed_channels = s_aec.afe_handle->get_channel_num(s_aec.afe_data);
  } else {
    s_aec.feed_channels = 2;
  }

  s_aec.sample_rate = sample_rate;

  ESP_LOGI(TAG, "AFE feed_chunk=%d fetch_chunk=%d channels=%d",
           s_aec.feed_chunk_size, s_aec.fetch_chunk_size, s_aec.feed_channels);

  // 5. Allocate memory for reference buffer
  s_aec.ref_capacity = REF_BUFFER_SIZE / sizeof(int16_t);
  s_aec.ref_buffer = heap_caps_malloc(REF_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
  if (!s_aec.ref_buffer) {
    ESP_LOGE(TAG, "Failed to allocate reference buffer");
    aec_processor_deinit();
    return ESP_ERR_NO_MEM;
  }
  s_aec.ref_head = 0;
  s_aec.ref_tail = 0;
  s_aec.ref_count = 0;

  // 6. Feed buffer (Interleaved: Mic + Ref)
  s_aec.feed_buffer_size =
      s_aec.feed_chunk_size * s_aec.feed_channels * sizeof(int16_t);
  s_aec.feed_buffer =
      heap_caps_malloc(s_aec.feed_buffer_size, MALLOC_CAP_SPIRAM);

  if (!s_aec.feed_buffer) {
    ESP_LOGE(TAG, "Failed to allocate feed buffer");
    aec_processor_deinit();
    return ESP_ERR_NO_MEM;
  }

  s_aec.is_initialized = true;
  ESP_LOGI(TAG, "AEC Processor initialized");

  return ESP_OK;
}

int aec_processor_get_chunk_size(void) {
  if (!s_aec.is_initialized)
    return 0;
  return s_aec.feed_chunk_size;
}

int aec_processor_get_fetch_chunk_size(void) {
  if (!s_aec.is_initialized)
    return 0;
  return s_aec.fetch_chunk_size;
}

void aec_processor_deinit(void) {
  if (!s_aec.is_initialized)
    return;

  if (s_aec.afe_handle && s_aec.afe_data) {
    s_aec.afe_handle->destroy(s_aec.afe_data);
    s_aec.afe_data = NULL;
  }

  if (s_aec.ref_buffer) {
    heap_caps_free(s_aec.ref_buffer);
    s_aec.ref_buffer = NULL;
  }

  if (s_aec.feed_buffer) {
    heap_caps_free(s_aec.feed_buffer);
    s_aec.feed_buffer = NULL;
  }

  s_aec.afe_handle = NULL;
  s_aec.afe_config = NULL;
  s_aec.is_initialized = false;
  ESP_LOGI(TAG, "AEC Processor stopped");
}

void aec_processor_feed_reference(const int16_t *data, size_t len) {
  if (!s_aec.is_initialized || !data || len == 0)
    return;

  // len is in bytes, convert to samples
  size_t count = len / sizeof(int16_t);
  ref_buffer_write(data, count);
}

esp_err_t aec_processor_process(const int16_t *mic_data, size_t len,
                                int16_t *out_data, size_t *out_len) {
  if (!s_aec.is_initialized)
    return ESP_ERR_INVALID_STATE;
  if (!mic_data || !out_data || !out_len)
    return ESP_ERR_INVALID_ARG;

  size_t samples_in = len / sizeof(int16_t);

  // Validate chunk size
  if (samples_in != s_aec.feed_chunk_size) {
    ESP_LOGW(TAG, "Chunk size (%d) != expected (%d)", (int)samples_in,
             s_aec.feed_chunk_size);
  }

  // Prepare interleaved data: [Mic0, Ref0, Mic1, Ref1, ...]
  size_t samples_to_process = samples_in;
  if (samples_to_process > (size_t)s_aec.feed_chunk_size) {
    samples_to_process = s_aec.feed_chunk_size;
  }

  for (size_t i = 0; i < samples_to_process; i++) {
    int16_t ref_sample = 0;
    if (s_aec.feed_channels > 1) {
      ref_buffer_read(&ref_sample, 1);
    }

    for (int ch = 0; ch < s_aec.feed_channels; ch++) {
      int idx = (int)(i * s_aec.feed_channels + ch);
      if (ch == 0) {
        s_aec.feed_buffer[idx] = mic_data[i];
      } else if (ch == s_aec.feed_channels - 1) {
        s_aec.feed_buffer[idx] = ref_sample;
      } else {
        s_aec.feed_buffer[idx] = 0;
      }
    }
  }

  // If data is smaller than AFE needs, pad with zeros to avoid feeding garbage
  if (samples_to_process < (size_t)s_aec.feed_chunk_size) {
    size_t missing = (size_t)s_aec.feed_chunk_size - samples_to_process;
    memset(&s_aec.feed_buffer[samples_to_process * s_aec.feed_channels], 0,
           missing * s_aec.feed_channels * sizeof(int16_t));
  }

  // Feed into AFE
  int feed_res = s_aec.afe_handle->feed(s_aec.afe_data, s_aec.feed_buffer);
  if (feed_res <= 0) {
    static uint32_t s_feed_warn_counter = 0;
    if ((s_feed_warn_counter++ % 125) == 0) {
      int expected = s_aec.feed_chunk_size * 2;
      ESP_LOGW(TAG, "AFE feed result=%d (expected ~%d samples)", feed_res,
               expected);
    }
  }

  // Fetch from AFE without blocking (polling)
  // We must pull data as quickly as possible.
  // If AFE returns data, great. If not, continue.
  afe_fetch_result_t *res = NULL;

  // Measure time only when active
  int64_t start = esp_timer_get_time();

  if (s_aec.afe_handle->fetch_with_delay) {
    int wait_ms = 20;
    if (s_aec.fetch_chunk_size > 0 && s_aec.sample_rate > 0) {
      wait_ms = (s_aec.fetch_chunk_size * 1000) / s_aec.sample_rate + 5;
      if (wait_ms < 5) {
        wait_ms = 5;
      } else if (wait_ms > 100) {
        wait_ms = 100;
      }
    }
    res = s_aec.afe_handle->fetch_with_delay(s_aec.afe_data,
                                             pdMS_TO_TICKS(wait_ms));
  } else {
    res = s_aec.afe_handle->fetch(s_aec.afe_data);
  }

  int64_t end = esp_timer_get_time();
  if (end - start > 5000) {
    // If fetch took more than 5 ms, something is slowing down
    ESP_LOGW(TAG, "WARN: AFE Processing took %lld us", end - start);
  }

  if (res) {
    static uint32_t s_log_counter = 0;
    if ((s_log_counter++ % 125) == 0) {
      ESP_LOGI(TAG,
               "AFE fetch: ret=%d vad=%d data_size=%d ringbuff_free=%.2f",
               res->ret_value, (int)res->vad_state, res->data_size,
               (double)res->ringbuff_free_pct);
    }
  }

  if (res && res->data && res->data_size > 0) {
    memcpy(out_data, res->data, res->data_size);
    *out_len = res->data_size;
    return ESP_OK;
  }

  // If no data is available
  *out_len = 0;
  return ESP_OK;
}

void aec_processor_reset(void) {
  if (s_aec.is_initialized) {
    s_aec.ref_head = 0;
    s_aec.ref_tail = 0;
    s_aec.ref_count = 0;
  }
}
