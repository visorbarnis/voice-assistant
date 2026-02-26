/**
 * @file main.c
 * @brief Wake-word recognition project for ESP32-S3 DevKitC-1
 *
 * Uses ESP-SR (Espressif Speech Recognition) framework to detect
 * a wake word. INMP441 microphone is connected via I2S, optionally
 * with MAX98357A for audio feedback.
 *
 * Wake-word model: wn9_mycroft_tts
 *
 * @author Generated with ESP-SR documentation
 * @date 2026
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// ESP-IDF drivers
#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

// ESP-SR (Speech Recognition) library
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"

#include "afe_pipeline.h"
#include "audio_volume_service.h"
#include "led_control.h"
#include "ntp_sync.h"
#include "runtime_config.h"
#include "session_state_bridge.h"
#include "voice_client.h"
#include "wifi_manager.h"

// ============================================================================
// Project configuration
// ============================================================================

static const char *TAG = "WAKE_WORD";

// Wake-word model configuration
// Available models: wn9_hilexin, wn9_alexa, wn9_nihaoxiaozhi, wn9_mycroft_tts
#ifndef CONFIG_WAKE_WORD_MODEL
#define CONFIG_WAKE_WORD_MODEL "wn9_mycroft_tts"
#endif

// ----------------------------------------------------------------------------
// I2S configuration (Full-duplex: Mic + Speaker on one port)
// ----------------------------------------------------------------------------
#define AUDIO_I2S_PORT I2S_NUM_0
#define MIC_SAMPLE_RATE 16000  // 16 kHz - required for ESP-SR
#define MIC_BITS_PER_SAMPLE 16 // 16-bit PCM

// Shared synchronization lines (BCLK/WS)
#define AUDIO_WS_GPIO GPIO_NUM_1
#define AUDIO_BCLK_GPIO GPIO_NUM_2

// Data lines
#define MIC_SD_GPIO GPIO_NUM_16 // Mic data (DOUT)

// DMA buffer
#define I2S_DMA_BUF_LEN                                                        \
  160 // 10ms frame (160 samples @ 16kHz) for AEC synchronization
#define I2S_DMA_BUF_COUNT 3 // Number of DMA buffers (low latency for AEC)

// ----------------------------------------------------------------------------
// I2S configuration for MAX98357A amplifier (Full-duplex)
// ----------------------------------------------------------------------------
#define SPK_DIN_GPIO GPIO_NUM_42 // Data In

// ----------------------------------------------------------------------------
// LED indicator configuration
// ----------------------------------------------------------------------------
// ESP32-S3 DevKitC-1 has built-in RGB LED on GPIO48
#define STATUS_LED_GPIO GPIO_NUM_48

// Wake button
#define WAKE_BUTTON_GPIO GPIO_NUM_11

// ----------------------------------------------------------------------------
// Recognition sensitivity settings
// ----------------------------------------------------------------------------
// Detection modes: DET_MODE_90, DET_MODE_95 (stricter)
// DET_MODE_90 - better for noisy environments, more false positives
// DET_MODE_95 - stricter, fewer false positives
#define WAKENET_DETECTION_MODE DET_MODE_95

// Bit shift for converting 32-bit I2S -> 16-bit PCM (mic gain)
// Lower value = higher gain (8=max, 16=min)
#define MIC_GAIN_SHIFT 14

// ============================================================================
// Global variables
// ============================================================================

// I2S channels (available from voice_client.c)
i2s_chan_handle_t s_i2s_rx_chan = NULL; // Microphone I2S channel
i2s_chan_handle_t s_i2s_tx_chan = NULL; // Speaker I2S channel

static volatile bool s_wake_detected = false; // Wake-word detected flag
static volatile bool s_voice_session_active =
    false; // Active voice session flag

static QueueHandle_t s_button_evt_queue = NULL;

// ============================================================================
// Helper functions
// ============================================================================

/**
 * @brief Convert 32-bit I2S sample to 16-bit PCM
 *
 * INMP441 outputs data in 32-bit format (24-bit left-justified),
 * while ESP-SR requires 16-bit PCM.
 *
 * @param sample 32-bit sample from I2S
 * @return int16_t 16-bit PCM sample
 */
static inline int16_t i2s_sample32_to_pcm16(int32_t sample) {
  // Shift and overflow protection
  int32_t val = sample >> MIC_GAIN_SHIFT;
  if (val > 32767)
    return 32767;
  if (val < -32768)
    return -32768;
  return (int16_t)val;
}

// ============================================================================
// NVS initialization
// ============================================================================

/**
 * @brief Initialize Non-Volatile Storage
 *
 * NVS is required for many ESP-IDF components.
 */
static void ensure_nvs_init(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "Erasing NVS flash...");
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_LOGI(TAG, "NVS initialized");
}

// LED functions moved to led_control.c

// ============================================================================
// Button handling
// ============================================================================

static void IRAM_ATTR button_isr_handler(void *arg) {
  uint32_t gpio_num = (uint32_t)arg;
  xQueueSendFromISR(s_button_evt_queue, &gpio_num, NULL);
}

static void button_task(void *arg) {
  uint32_t io_num;
  while (1) {
    if (xQueueReceive(s_button_evt_queue, &io_num, portMAX_DELAY)) {
      // Simple software debounce
      vTaskDelay(pdMS_TO_TICKS(50));

      // Check level (assume Active Low - pressed to ground)
      if (gpio_get_level(io_num) == 0) {
        ESP_LOGI(TAG, "Button pressed on GPIO %d", io_num);
        afe_pipeline_manual_wakeup();

        // Wait for release or timeout to avoid spamming
        vTaskDelay(pdMS_TO_TICKS(500));
      }
    }
  }
}

static void init_button(void) {
  gpio_config_t io_conf = {
      .intr_type = GPIO_INTR_NEGEDGE, // Interrupt on button press (falling edge)
      .mode = GPIO_MODE_INPUT,
      .pin_bit_mask = (1ULL << WAKE_BUTTON_GPIO),
      .pull_down_en = 0,
      .pull_up_en = 1, // Pull-up enabled (button shorts to ground)
  };
  gpio_config(&io_conf);

  s_button_evt_queue = xQueueCreate(10, sizeof(uint32_t));
  xTaskCreate(button_task, "button_task", 4096, NULL, 10, NULL);

  gpio_install_isr_service(0);
  gpio_isr_handler_add(WAKE_BUTTON_GPIO, button_isr_handler,
                       (void *)WAKE_BUTTON_GPIO);

  ESP_LOGI(TAG, "Button initialized on GPIO %d", WAKE_BUTTON_GPIO);
}

// ============================================================================
// Full-duplex I2S initialization (Mic + Speaker on one port)
// ============================================================================

/**
 * @brief Initialize I2S interface for full-duplex
 *
 * RX (INMP441) and TX (MAX98357A) run on the same I2S port with shared
 * BCLK/WS. This is critical for stable AEC.
 *
 * @return esp_err_t ESP_OK on success
 */
static esp_err_t init_i2s_full_duplex(void) {
  ESP_LOGI(TAG, "Initializing I2S: Force 32-bit Slot Width for Sync");

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  chan_cfg.auto_clear = true;
  chan_cfg.dma_desc_num = 16;   // 16 buffers to absorb delay (~160ms)
  chan_cfg.dma_frame_num = 160; // 10ms frame for AEC synchronization

  ESP_RETURN_ON_ERROR(
      i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan), TAG,
      "Failed to create I2S channels");

  // 1. RX (MIC): 32-bit data in 32-bit slot
  i2s_std_config_t rx_std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
      // Use STEREO to capture everything (safer when channel routing varies)
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = GPIO_NUM_NC,
              .bclk = AUDIO_BCLK_GPIO,
              .ws = AUDIO_WS_GPIO,
              .dout = GPIO_NUM_NC,
              .din = MIC_SD_GPIO,
          },
  };
  // Explicitly set 32-bit slot width
  rx_std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  // WS must match slot width in full-duplex mode
  rx_std_cfg.slot_cfg.ws_width = I2S_SLOT_BIT_WIDTH_32BIT;

  // 2. TX (SPK): 16-bit data in 32-bit slot (KEY POINT)
  // We send 16-bit samples in 32-bit containers so BCLK matches
  // the microphone side.
  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = GPIO_NUM_NC,
              .bclk = AUDIO_BCLK_GPIO,
              .ws = AUDIO_WS_GPIO,
              .dout = SPK_DIN_GPIO,
              .din = GPIO_NUM_NC,
          },
  };
  // FORCE 32-bit slot width so BCLK matches RX frequency
  tx_std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  // Data remains 16-bit; driver pads with zeros.
  // WS must match slot width in full-duplex mode
  tx_std_cfg.slot_cfg.ws_width = I2S_SLOT_BIT_WIDTH_32BIT;

  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx_chan, &rx_std_cfg),
                      TAG, "RX init failed");
  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx_chan, &tx_std_cfg),
                      TAG, "TX init failed");

  // =========================================================================
  // MAIN FIX: Enable pull-down on microphone input
  // This removes noise in empty Hi-Z slot by forcing clean zero.
  // =========================================================================
  gpio_set_pull_mode(MIC_SD_GPIO, GPIO_PULLDOWN_ONLY);
  ESP_LOGI(TAG, "GPIO %d Pull-Down Enabled (Silence Fix)", MIC_SD_GPIO);

  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx_chan), TAG,
                      "RX enable failed");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx_chan), TAG,
                      "TX enable failed");

  return ESP_OK;
}

/**
 * @brief Reconfigure speaker sample rate
 *
 * In full-duplex mode, rate must match microphone (16 kHz),
 * otherwise AEC and WakeNet work incorrectly.
 *
 * @param sample_rate New sample rate
 * @return esp_err_t ESP_OK on success
 */
esp_err_t reconfigure_speaker_sample_rate(uint32_t sample_rate) {
  if (!s_i2s_tx_chan) {
    ESP_LOGE(TAG, "I2S TX channel is not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (sample_rate != MIC_SAMPLE_RATE) {
    ESP_LOGW(TAG,
             "Full-duplex: fixed sample rate %d Hz, request %lu ignored",
             MIC_SAMPLE_RATE, (unsigned long)sample_rate);
    return ESP_ERR_INVALID_ARG;
  }

  return ESP_OK;
}

/**
 * @brief Play beep sound - blocking version
 *
 * Generates a simple sine tone.
 * @param freq_hz Frequency in Hz
 * @param duration_ms Duration in ms
 */
static void play_beep(int freq_hz, int duration_ms) {
  if (!s_i2s_tx_chan) {
    ESP_LOGW(TAG, "I2S TX channel is not initialized");
    return;
  }

  const int sample_count = MIC_SAMPLE_RATE * duration_ms / 1000;
  const float amplitude = 0.5f; // 50% of maximum

  int16_t *tone_buffer =
      heap_caps_malloc(sample_count * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
  if (!tone_buffer) {
    ESP_LOGW(TAG, "Failed to allocate memory for tone");
    return;
  }

  // Fill buffer with sine wave
  for (int i = 0; i < sample_count; i++) {
    float t = (float)i / MIC_SAMPLE_RATE;
    float sample = amplitude * sinf(2.0f * M_PI * freq_hz * t);
    int16_t sample16 = (int16_t)(sample * 32767.0f);
    tone_buffer[i * 2] = sample16;
    tone_buffer[i * 2 + 1] = sample16;
  }

  audio_volume_service_apply_pcm16(tone_buffer, (size_t)sample_count * 2);

  // Play tone
  size_t bytes_written = 0;
  i2s_channel_write(s_i2s_tx_chan, tone_buffer,
                    sample_count * 2 * sizeof(int16_t), &bytes_written,
                    portMAX_DELAY);

  heap_caps_free(tone_buffer);
}

/**
 * @brief Parameters for asynchronous beep
 */
typedef struct {
  int freq_hz;
  int duration_ms;
} beep_params_t;

/**
 * @brief Beep playback task (non-blocking)
 */
static void beep_task(void *arg) {
  beep_params_t *p = (beep_params_t *)arg;
  play_beep(p->freq_hz, p->duration_ms);
  free(p);
  vTaskDelete(NULL);
}

/**
 * @brief Asynchronous beep playback (runs in separate task)
 */
static void play_beep_async(int freq_hz, int duration_ms) {
  beep_params_t *p = malloc(sizeof(beep_params_t));
  if (p) {
    p->freq_hz = freq_hz;
    p->duration_ms = duration_ms;
    xTaskCreate(beep_task, "beep", 2048, p, 3, NULL);
  }
}

/**
 * @brief Startup beep on boot
 */
static void play_startup_beep(void) {
  ESP_LOGI(TAG, "Playing startup tone...");
  // Two short beeps: high + low
  play_beep(880, 100); // A5 - 100ms
  vTaskDelay(pdMS_TO_TICKS(50));
  play_beep(440, 100); // A4 - 100ms
  ESP_LOGI(TAG, "Startup tone finished");
}

// ============================================================================
// Callback functions for AFE (UI + Session Control)
// ============================================================================

/**
 * @brief Callback when voice session ends
 */
static void on_voice_session_ended(void) {
  bool was_active = s_voice_session_active;

  // CRITICAL: reset flag to allow next wake word
  s_voice_session_active = false;

  if (!was_active) {
    return;
  }

  ESP_LOGI(TAG, "==========================================");
  ESP_LOGI(TAG, "🔌 Voice session ended");
  ESP_LOGI(TAG, "🎤 Returning to wake-word detection mode");
  ESP_LOGI(TAG, "==========================================");

  // Outside active session, LED should be off
  led_control_set_state(LED_STATE_OFF);

  // Non-blocking beep to indicate return to wake-word mode
  play_beep_async(440, 100); // A4 - 100ms
}

void session_state_mark_started_external(void) {
  // Wake-word path already set the flag/UI. Do nothing in that case.
  if (s_voice_session_active) {
    return;
  }

  s_voice_session_active = true;

  ESP_LOGI(TAG, "Session started by server command");
  led_control_set_state(LED_STATE_LISTENING);
  play_beep_async(1000, 150);
}

/**
 * @brief Callback on wake-word detection
 *
 * @return true to start session, false to cancel
 */
static bool on_wake_word_detected_cb(const char *model_name, float volume_db) {
  // Check whether voice session is already active
  if (s_voice_session_active) {
    return false;
  }

  // Check WiFi FIRST (before setting active flag)
  if (!wifi_manager_is_connected()) {
    ESP_LOGW(TAG, "⚠️ WiFi is not connected, voice session is not possible");
    play_beep_async(200, 100); // Error signal
    led_control_set_state(LED_STATE_OFF);
    return false;
  }

  // Wake-word is allowed only with active WSS connection
  if (!voice_client_is_connected()) {
    ESP_LOGW(TAG, "⚠️ WSS is not connected, wake-word ignored");
    play_beep_async(200, 100); // Error signal
    led_control_set_state(LED_STATE_OFF);
    return false;
  }

  s_voice_session_active = true;

  ESP_LOGI(TAG, "==========================================");
  ESP_LOGI(TAG, "🎤 WAKE WORD DETECTED!");
  ESP_LOGI(TAG, "   Model: %s", model_name ? model_name : "unknown");
  ESP_LOGI(TAG, "   Volume: %.1f dBFS", volume_db);
  ESP_LOGI(TAG, "==========================================");

  // Non-blocking calls - do not stall AFE task
  led_control_set_state(LED_STATE_LISTENING);
  play_beep_async(1000, 150); // 1000 Hz, 150 ms

  return true;
}

// ============================================================================
// Entry point
// ============================================================================

/**
 * @brief Main application function
 *
 * Initializes all components and starts AFE pipeline.
 *
 */
void app_main(void) {
  const runtime_config_t *runtime_cfg = NULL;

  ESP_LOGI(TAG, "================================================");
  ESP_LOGI(TAG, "ESP32-S3 Wake-Word + Voice Assistant");
  ESP_LOGI(TAG, "Model: %s", CONFIG_WAKE_WORD_MODEL);
  ESP_LOGI(TAG, "================================================");

  // Suppress frequent AFE warnings (Ringbuffer empty), which are normal
  // during startup/shutdown
  esp_log_level_set("AFE", ESP_LOG_ERROR);

  // Initialize NVS
  ensure_nvs_init();

  esp_err_t cfg_err = runtime_config_init();
  if (cfg_err != ESP_OK) {
    ESP_LOGW(TAG, "runtime_config_init failed: %s (using defaults)",
             esp_err_to_name(cfg_err));
  }
  runtime_cfg = runtime_config_get();
  ESP_LOGI(TAG, "Server: wss://%s:%u%s", runtime_cfg->server_host,
           (unsigned)runtime_cfg->server_port, runtime_cfg->voice_ws_path);

  esp_err_t vol_err =
      audio_volume_service_init(runtime_cfg->audio_playback_volume_percent);
  if (vol_err != ESP_OK) {
    ESP_LOGW(TAG, "audio_volume_service_init failed: %s",
             esp_err_to_name(vol_err));
  }

  // Initialize LED (WS2812 RMT, low priority)
  ESP_ERROR_CHECK(led_control_init(STATUS_LED_GPIO));
  led_control_set_state(LED_STATE_OFF);

  // Initialize full-duplex I2S (shared BCLK/WS for Mic and Speaker)
  ESP_ERROR_CHECK(init_i2s_full_duplex());

  // Startup tone to verify speaker
  play_startup_beep();

  // Initialize WiFi
  ESP_LOGI(TAG, "Initializing WiFi...");
  ESP_ERROR_CHECK(wifi_manager_init());

  // Connect to WiFi (blocking call)
  esp_err_t wifi_err = wifi_manager_connect();
  if (wifi_err == ESP_OK) {
    ESP_LOGI(TAG, "✅ WiFi connected successfully");

    // NTP sync is required to validate TLS certificate dates (WSS)
    esp_err_t ntp_err = ntp_sync_initialize();
    if (ntp_err != ESP_OK) {
      ESP_LOGW(TAG, "⚠️ NTP synchronization failed: %s",
               esp_err_to_name(ntp_err));
    }

    // Signal successful WiFi connection
    play_beep(880, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    play_beep(1760, 100);
    // LED off outside active session
    led_control_set_state(LED_STATE_OFF);
  } else {
    ESP_LOGW(TAG, "⚠️ WiFi is not connected, voice session unavailable");
    // WiFi error signal
    play_beep(200, 200);
    vTaskDelay(pdMS_TO_TICKS(100));
    play_beep(200, 200);
    led_control_set_state(LED_STATE_OFF);
  }

  // Initialize voice client
  ESP_LOGI(TAG, "Initializing voice client...");
  // Register session-end callback in client
  // (client triggers it on WS disconnect; we forward to AFE)
  ESP_ERROR_CHECK(voice_client_init(afe_pipeline_notify_session_end));

  // Initialize AFE (WakeNet + AEC)
  srmodel_list_t *models = esp_srmodel_init("model");
  if (!models || models->num == 0) {
    ESP_LOGE(TAG, "ERROR: Models not found in 'model' partition");
    return;
  }

  // Initialize pipeline
  if (afe_pipeline_init(models) != ESP_OK) {
    ESP_LOGE(TAG, "ERROR: Failed to initialize AFE pipeline");
    return;
  }

  // Register UI callbacks
  afe_pipeline_register_callbacks(on_wake_word_detected_cb,
                                  on_voice_session_ended);

  // Start AFE task
  if (afe_pipeline_start() != ESP_OK) {
    ESP_LOGE(TAG, "ERROR: Failed to start AFE pipeline");
    return;
  }

  // Initialize button
  init_button();

  ESP_LOGI(TAG, "System ready. Say the wake word or press the button...");
}
