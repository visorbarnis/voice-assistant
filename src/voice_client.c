/**
 * @file voice_client.c
 * @brief WebSocket voice client implementation
 *
 * Manages the full voice session lifecycle:
 * 1. Connect to WebSocket server
 * 2. Start audio capture and playback tasks
 * 3. Process incoming messages (audio and commands)
 * 4. Graceful shutdown on disconnect
 */

#include "voice_client.h"
#include "audio_volume_service.h"
#include "gpio_control.h"
#include "led_control.h"
#include "runtime_config.h"
#include "session_state_bridge.h"
#include "wifi_manager.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "afe_pipeline.h"
#include "cJSON.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "VOICE_CLI";

// ----------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------

// Microphone chunk size (samples)
#define MIC_CHUNK_SAMPLES 160 // 10ms frame for AEC synchronization
#define MIC_SAMPLE_RATE 16000
#define MIC_BYTES_PER_SAMPLE 2

// Playback chunk size (samples)
#define PLAYBACK_CHUNK_SAMPLES 160 // 10ms frame for AEC synchronization
#define PLAYBACK_ACTIVE_HOLD_MS 200

// Task stack sizes
#define AUDIO_TASK_STACK_SIZE 8192

// Persistent connection maintenance intervals
#define RECONNECT_INTERVAL_MS 3000
#define HEARTBEAT_INTERVAL_MS 5000
#define MAX_TEXT_PAYLOAD_BYTES 16384
#define CONTROL_MSG_QUEUE_LEN 8
#define CONTROL_MSG_TASK_STACK 4096
#define CONTROL_MSG_TASK_PRIO 2
#define VOLUME_PERSIST_TASK_STACK 3072
#define VOLUME_PERSIST_TASK_PRIO 2

// JSON interrupt command
#define INTERRUPTED_MSG "{\"type\":\"interrupted\"}"

// INMP441: 24-bit data in 32-bit slot, left-aligned
// Shift by 10 keeps 22 bits of precision -> 16 bits with gain
#define MIC_GAIN_SHIFT 11

// ID ring buffer for DMA delay compensation
#define ID_RING_SIZE 32     // >= DMA descriptors count (16)
#define DMA_DELAY_FRAMES 16 // Number of delayed DMA frames

// ----------------------------------------------------------------------------
// External I2S channels (defined in main.c)
// ----------------------------------------------------------------------------
extern i2s_chan_handle_t s_i2s_rx_chan; // Microphone
extern i2s_chan_handle_t s_i2s_tx_chan; // Speaker

// Speaker reconfiguration function (defined in main.c)
extern esp_err_t reconfigure_speaker_sample_rate(uint32_t sample_rate);

// ----------------------------------------------------------------------------
// Internal state
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// Internal state
// ----------------------------------------------------------------------------

typedef struct {
  esp_websocket_client_handle_t ws_client;
  audio_buffer_t *playback_buffer;

  TaskHandle_t connection_task;
  TaskHandle_t heartbeat_task;
  TaskHandle_t playback_task;
  TaskHandle_t tx_task;
  TaskHandle_t mic_capture_task; // Raw PCM capture task
  QueueHandle_t tx_queue;

  voice_client_session_end_cb_t on_session_end;
  volatile bool ws_connected;
  volatile bool session_active;
  volatile bool client_running;
  volatile bool tasks_should_run;
  volatile bool playback_active;
  int64_t last_playback_rx_us;
  int64_t last_connect_attempt_us;
  SemaphoreHandle_t state_mutex;

  char uri[192];
  char headers[512];
} voice_client_state_t;

static voice_client_state_t s_state = {0};
typedef enum {
  DUPLEX_MODE_FULL = 0,
  DUPLEX_MODE_HALF = 1,
} duplex_mode_t;

static bool s_half_duplex_mode = false;
static duplex_mode_t s_effective_duplex_mode = DUPLEX_MODE_FULL;
static const runtime_config_t *s_cfg = NULL;

// Packet descriptor for sending
typedef struct {
  uint8_t *data;
  size_t len;
} tx_item_t;

typedef struct {
  char *payload;
  size_t payload_len;
} control_msg_t;

// ID ring buffer for DMA delay compensation
typedef struct {
  int32_t ids[ID_RING_SIZE];
  size_t head;
  size_t tail;
  size_t count;
} id_ring_t;

static id_ring_t s_id_ring = {0};
static volatile int32_t s_current_played_id =
    -1; // Frame ID currently being played

// Last known server ID (received from server, used as fallback during
// buffering) Last received server ID (used as fallback during buffering)
static volatile int32_t s_last_received_server_id = -1;

// Handle for Task Notification synchronization
static TaskHandle_t s_mic_task_handle = NULL;
static char *s_text_payload_buffer = NULL;
static size_t s_text_payload_capacity = 0;
static QueueHandle_t s_control_msg_queue = NULL;
static TaskHandle_t s_control_msg_task = NULL;
static QueueHandle_t s_volume_persist_queue = NULL;
static TaskHandle_t s_volume_persist_task = NULL;

// ID ring buffer helpers
static void id_ring_push(id_ring_t *ring, int32_t id) {
  ring->ids[ring->head] = id;
  ring->head = (ring->head + 1) % ID_RING_SIZE;
  if (ring->count < ID_RING_SIZE) {
    ring->count++;
  } else {
    ring->tail = (ring->tail + 1) % ID_RING_SIZE;
  }
}

/**
 * @brief Extract ID with DMA delay compensation
 *
 * Returns frame ID that is ACTUALLY being played by the speaker
 * (accounting for DMA buffer delay).
 *
 * IMPORTANT for AEC: If there are not enough frames, return OLDEST
 * available ID instead of -1. This lets server use the "closest"
 * reference instead of fully skipping AEC.
 */
static int32_t id_ring_pop_delayed(id_ring_t *ring, size_t delay) {
  if (ring->count == 0) {
    return -1; // Buffer is empty - no data
  }

  if (ring->count <= delay) {
    // Startup phase: not enough frames for full compensation
    // Return oldest ID without pop (closest to what is playing)
    // Server gets approximate reference - better than nothing
    return ring->ids[ring->tail];
  }

  // Normal operation: full DMA delay compensation
  int32_t id = ring->ids[ring->tail];
  ring->tail = (ring->tail + 1) % ID_RING_SIZE;
  ring->count--;
  return id;
}

static void id_ring_reset(id_ring_t *ring) {
  ring->head = 0;
  ring->tail = 0;
  ring->count = 0;
}

static const char *duplex_mode_to_string(duplex_mode_t mode) {
  return (mode == DUPLEX_MODE_HALF) ? "half-duplex" : "full-duplex";
}

static bool parse_duplex_mode(const char *mode, duplex_mode_t *out_mode) {
  if (!mode || !out_mode) {
    return false;
  }

  if (strcmp(mode, "half-duplex") == 0 || strcmp(mode, "hal-duplex") == 0) {
    *out_mode = DUPLEX_MODE_HALF;
    return true;
  }

  if (strcmp(mode, "full-duplex") == 0 || strcmp(mode, "full-diplex") == 0) {
    *out_mode = DUPLEX_MODE_FULL;
    return true;
  }

  return false;
}

static void resolve_speak_mode_config(void) {
  const char *speak_mode = s_cfg ? s_cfg->speak_mode : CONFIG_SPEAK_MODE;
  const char *client_mode =
      s_cfg ? s_cfg->voice_client_mode : CONFIG_VOICE_CLIENT_MODE;
  duplex_mode_t parsed_speak_mode = DUPLEX_MODE_FULL;
  duplex_mode_t parsed_client_mode = DUPLEX_MODE_FULL;
  bool speak_mode_valid = parse_duplex_mode(speak_mode, &parsed_speak_mode);
  bool client_mode_valid = parse_duplex_mode(client_mode, &parsed_client_mode);

  if (!speak_mode_valid) {
    ESP_LOGW(TAG, "Unknown speak_mode='%s', using full-duplex", speak_mode);
    parsed_speak_mode = DUPLEX_MODE_FULL;
  }

  if (!client_mode_valid) {
    ESP_LOGW(TAG,
             "Unknown client_mode='%s'; advertising effective controller mode '%s'",
             client_mode, duplex_mode_to_string(parsed_speak_mode));
  } else if (parsed_client_mode != parsed_speak_mode) {
    ESP_LOGW(TAG,
             "Config mismatch: speak_mode='%s', client_mode='%s'; "
             "advertising effective controller mode '%s'",
             speak_mode, client_mode, duplex_mode_to_string(parsed_speak_mode));
  }

  s_effective_duplex_mode = parsed_speak_mode;
  s_half_duplex_mode = (s_effective_duplex_mode == DUPLEX_MODE_HALF);

  ESP_LOGI(TAG, "Speak mode: %s", duplex_mode_to_string(s_effective_duplex_mode));
  ESP_LOGI(TAG, "Effective client mode: %s",
           duplex_mode_to_string(s_effective_duplex_mode));
}

// ----------------------------------------------------------------------------
// Prototypes
// ----------------------------------------------------------------------------

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data);
static void connection_maintainer_task(void *arg);
static void heartbeat_task(void *arg);
static void audio_playback_task(void *arg);
static void mic_capture_task(void *arg); // Raw PCM capture from I2S
static void network_tx_task(void *arg);
static void stop_audio_tasks(void);
static void update_playback_activity(bool buffering);
static void async_session_cleanup_task(void *arg);
static void control_message_task(void *arg);
static bool enqueue_control_message(char *payload, size_t payload_len);
static bool is_set_volume_type_message(const char *payload);
static bool is_gpio_command_type_message(const char *payload);
static void volume_persist_task(void *arg);
static void schedule_volume_persist(uint32_t volume_percent);
static bool handle_set_volume_command(const char *payload, size_t payload_len);
static void reset_text_payload_buffer(void);
static bool assemble_text_payload(const esp_websocket_event_data_t *data,
                                  char **payload_out, size_t *payload_len_out);
static esp_err_t ensure_ws_client_locked(void);
static void reset_ws_client_locked(const char *reason);
static bool is_ws_connected(void);
static esp_err_t request_session_start(void);
static esp_err_t request_session_stop(void);
static esp_err_t start_local_audio_session(void);
static bool stop_local_audio_session(bool notify_session_end);
static void resolve_speak_mode_config(void);

/**
 * @brief Convert 32-bit I2S sample (INMP441) to 16-bit PCM
 *
 * INMP441 outputs 24-bit signed data, left-aligned in a 32-bit slot.
 * Shift by 10 bits (not 16) to preserve precision and gain,
 * then clip to prevent overflow.
 */
static inline int16_t i2s_sample32_to_pcm16(int32_t sample) {
  sample >>= MIC_GAIN_SHIFT;
  if (sample > INT16_MAX) {
    return INT16_MAX;
  }
  if (sample < INT16_MIN) {
    return INT16_MIN;
  }
  return (int16_t)sample;
}

// ----------------------------------------------------------------------------
// Asynchronous session cleanup on errors
// (do not call stop_audio_tasks from event handler - deadlock!)
// ----------------------------------------------------------------------------

static void async_session_cleanup_task(void *arg) {
  ESP_LOGW(TAG, "Asynchronous logical session cleanup after error");
  stop_local_audio_session(true);

  vTaskDelete(NULL);
}

static void reset_text_payload_buffer(void) {
  if (s_text_payload_buffer) {
    free(s_text_payload_buffer);
    s_text_payload_buffer = NULL;
  }
  s_text_payload_capacity = 0;
}

static bool assemble_text_payload(const esp_websocket_event_data_t *data,
                                  char **payload_out,
                                  size_t *payload_len_out) {
  if (!data || !payload_out || !payload_len_out || !data->data_ptr ||
      data->data_len <= 0) {
    return false;
  }

  size_t chunk_len = (size_t)data->data_len;
  size_t chunk_offset = data->payload_offset > 0 ? (size_t)data->payload_offset : 0;
  size_t total_len = data->payload_len > 0 ? (size_t)data->payload_len : chunk_len;

  if (total_len > MAX_TEXT_PAYLOAD_BYTES) {
    ESP_LOGW(TAG, "Dropping oversized text payload (%u bytes)",
             (unsigned)total_len);
    reset_text_payload_buffer();
    return false;
  }

  if (chunk_offset == 0) {
    reset_text_payload_buffer();
    s_text_payload_buffer = malloc(total_len + 1);
    if (!s_text_payload_buffer) {
      ESP_LOGE(TAG, "Failed to allocate text payload buffer (%u bytes)",
               (unsigned)total_len);
      return false;
    }
    s_text_payload_capacity = total_len;
  }

  if (!s_text_payload_buffer || s_text_payload_capacity != total_len) {
    ESP_LOGW(TAG, "Text payload assembly state mismatch, dropping frame");
    reset_text_payload_buffer();
    return false;
  }

  if (chunk_offset + chunk_len > s_text_payload_capacity) {
    ESP_LOGW(TAG,
             "Text payload chunk overflow: offset=%u chunk=%u capacity=%u",
             (unsigned)chunk_offset, (unsigned)chunk_len,
             (unsigned)s_text_payload_capacity);
    reset_text_payload_buffer();
    return false;
  }

  memcpy(s_text_payload_buffer + chunk_offset, data->data_ptr, chunk_len);
  size_t assembled_len = chunk_offset + chunk_len;
  if (assembled_len < s_text_payload_capacity) {
    return false;
  }

  s_text_payload_buffer[s_text_payload_capacity] = '\0';
  *payload_out = s_text_payload_buffer;
  *payload_len_out = s_text_payload_capacity;
  s_text_payload_buffer = NULL;
  s_text_payload_capacity = 0;
  return true;
}

static void volume_persist_task(void *arg) {
  uint32_t volume_percent = 0;

  while (s_state.client_running ||
         (s_volume_persist_queue &&
          uxQueueMessagesWaiting(s_volume_persist_queue) > 0)) {
    if (!s_volume_persist_queue) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (xQueueReceive(s_volume_persist_queue, &volume_percent,
                      pdMS_TO_TICKS(200)) != pdTRUE) {
      continue;
    }

    // Keep only the latest requested value.
    uint32_t latest = volume_percent;
    while (xQueueReceive(s_volume_persist_queue, &volume_percent, 0) == pdTRUE) {
      latest = volume_percent;
    }

    esp_err_t err = runtime_config_set_audio_playback_volume_percent(latest);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to persist volume %lu%%: %s",
               (unsigned long)latest, esp_err_to_name(err));
    } else {
      ESP_LOGI(TAG, "Persisted volume: %lu%%", (unsigned long)latest);
    }
  }

  s_volume_persist_task = NULL;
  vTaskDelete(NULL);
}

static void schedule_volume_persist(uint32_t volume_percent) {
  if (!s_volume_persist_queue) {
    return;
  }

  xQueueOverwrite(s_volume_persist_queue, &volume_percent);
}

static bool enqueue_control_message(char *payload, size_t payload_len) {
  if (!payload || payload_len == 0 || !s_control_msg_queue) {
    return false;
  }

  control_msg_t msg = {
      .payload = payload,
      .payload_len = payload_len,
  };

  if (xQueueSend(s_control_msg_queue, &msg, 0) != pdTRUE) {
    return false;
  }

  return true;
}

static bool is_set_volume_type_message(const char *payload) {
  if (!payload) {
    return false;
  }

  return strstr(payload, "\"type\":\"set_volume\"") != NULL ||
         strstr(payload, "\"type\": \"set_volume\"") != NULL;
}

static bool is_gpio_command_type_message(const char *payload) {
  if (!payload) {
    return false;
  }

  return strstr(payload, "\"type\":\"gpio_command\"") != NULL ||
         strstr(payload, "\"type\": \"gpio_command\"") != NULL;
}

static bool parse_volume_percent(const cJSON *item, const char *field_name,
                                 int *out_volume) {
  if (!item || !field_name || !out_volume) {
    return false;
  }

  double raw = 0.0;
  if (cJSON_IsNumber(item)) {
    raw = item->valuedouble;
  } else if (cJSON_IsString(item) && item->valuestring &&
             item->valuestring[0] != '\0') {
    char *endptr = NULL;
    raw = strtod(item->valuestring, &endptr);
    if (!endptr || endptr == item->valuestring || *endptr != '\0') {
      return false;
    }
  } else {
    return false;
  }

  if (!isfinite(raw)) {
    return false;
  }

  // Accept both formats:
  // - volume/value: 0..1 (normalized) or 0..100 (percent)
  // - volume_pct: 0..100 (percent only)
  double percent = raw;
  if (strcmp(field_name, "volume_pct") != 0 && raw >= 0.0 && raw <= 1.0) {
    percent = raw * 100.0;
  }

  if (percent < 0.0 || percent > 100.0) {
    return false;
  }

  int rounded = (int)(percent + 0.5);
  if (rounded > 100) {
    rounded = 100;
  }

  *out_volume = rounded;
  return true;
}

static bool handle_set_volume_command(const char *payload, size_t payload_len) {
  if (!payload || payload_len == 0) {
    return false;
  }

  if (strstr(payload, "set_volume") == NULL) {
    return false;
  }

  cJSON *root = cJSON_ParseWithLength(payload, payload_len);
  if (!root) {
    return false;
  }

  bool handled = false;
  cJSON *type = cJSON_GetObjectItem(root, "type");
  if (!cJSON_IsString(type) || strcmp(type->valuestring, "set_volume") != 0) {
    goto cleanup;
  }
  handled = true;

  int volume = -1;
  const char *volume_field = NULL;

  cJSON *volume_item = cJSON_GetObjectItem(root, "value");
  if (parse_volume_percent(volume_item, "value", &volume)) {
    volume_field = "value";
  }

  if (!volume_field) {
    volume_item = cJSON_GetObjectItem(root, "volume");
    if (parse_volume_percent(volume_item, "volume", &volume)) {
      volume_field = "volume";
    }
  }

  if (!volume_field) {
    volume_item = cJSON_GetObjectItem(root, "volume_pct");
    if (parse_volume_percent(volume_item, "volume_pct", &volume)) {
      volume_field = "volume_pct";
    }
  }

  if (!volume_field) {
    ESP_LOGW(TAG, "set_volume ignored: invalid value (expected 0..100 or 0..1)");
    goto cleanup;
  }

  esp_err_t apply_err = audio_volume_service_set((uint32_t)volume);
  if (apply_err != ESP_OK) {
    ESP_LOGW(TAG, "set_volume failed to apply: %s", esp_err_to_name(apply_err));
    goto cleanup;
  }

  schedule_volume_persist((uint32_t)volume);
  ESP_LOGI(TAG, "set_volume applied: %d%% (source=%s, persist scheduled)",
           volume, volume_field);

cleanup:
  cJSON_Delete(root);
  return handled;
}

static void control_message_task(void *arg) {
  control_msg_t msg = {0};

  while (s_state.client_running ||
         (s_control_msg_queue && uxQueueMessagesWaiting(s_control_msg_queue) > 0)) {
    if (!s_control_msg_queue) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (xQueueReceive(s_control_msg_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
      continue;
    }

    if (!msg.payload || msg.payload_len == 0) {
      continue;
    }

    if (is_set_volume_type_message(msg.payload) &&
        handle_set_volume_command(msg.payload, msg.payload_len)) {
      // set_volume is handled independently from session state
    } else if (is_gpio_command_type_message(msg.payload)) {
      gpio_control_handle_command(msg.payload, msg.payload_len);
    }

    free(msg.payload);
    msg.payload = NULL;
    msg.payload_len = 0;
  }

  s_control_msg_task = NULL;
  vTaskDelete(NULL);
}

// ----------------------------------------------------------------------------
// WebSocket Event Handler
// ----------------------------------------------------------------------------

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

  switch (event_id) {
  case WEBSOCKET_EVENT_CONNECTED:
    ESP_LOGI(TAG, "WebSocket: persistent connection established");
    s_state.ws_connected = true;
    s_state.last_connect_attempt_us = 0;
    break;

  case WEBSOCKET_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "WebSocket: disconnected from server");
    s_state.ws_connected = false;
    s_state.last_playback_rx_us = 0;
    s_state.playback_active = false;
    reset_text_payload_buffer();
    stop_local_audio_session(true);
    break;

  case WEBSOCKET_EVENT_DATA:
    if (data->op_code == 0x02) {
      // Binary data: [4 bytes server_id][320 bytes PCM]
      if (s_state.session_active && s_state.playback_buffer && data->data_ptr &&
          data->data_len >= 4) {
        int32_t server_id;
        memcpy(&server_id, data->data_ptr, sizeof(int32_t));

        // Track the last received server ID for AEC fallback during buffering
        // Save last received ID for AEC fallback during buffering
        s_last_received_server_id = server_id;

        const uint8_t *pcm_data =
            (const uint8_t *)data->data_ptr + sizeof(int32_t);
        size_t pcm_len = data->data_len - sizeof(int32_t);

        if (pcm_len > 0) {
          audio_buffer_write_with_id(s_state.playback_buffer, pcm_data, pcm_len,
                                     server_id);
          s_state.last_playback_rx_us = esp_timer_get_time();
          s_state.playback_active = true;

          // Audio data arrived - we are speaking
          // Check first 3 bytes for non-silence
          bool has_sound = false;
          for (int i = 0; i < 3 && i < pcm_len; i++) {
            if (pcm_data[i] != 0) {
              has_sound = true;
              break;
            }
          }

          if (has_sound) {
            led_control_set_state(LED_STATE_SPEAKING);
          }
        }
      }
    } else if (data->op_code == 0x01) {
      // Text data
      if (data->data_ptr && data->data_len > 0) {
        char *payload = NULL;
        size_t payload_len = 0;
        if (!assemble_text_payload(data, &payload, &payload_len)) {
          break;
        }

        bool is_set_volume = is_set_volume_type_message(payload);
        bool is_gpio_cmd = is_gpio_command_type_message(payload);

        // No active session: route control JSON to low-priority worker to keep
        // callback fast for wake-word mode.
        if (!s_state.session_active && (is_set_volume || is_gpio_cmd)) {
          if (enqueue_control_message(payload, payload_len)) {
            // Ownership transferred to control_message_task.
            payload = NULL;
          } else {
            ESP_LOGW(TAG, "Control queue is full, dropping control message");
          }
        } else if (strstr(payload, "\"session_start\"") != NULL) {
          ESP_LOGI(TAG, "Received command: session_start");
          if (start_local_audio_session() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start local session");
          } else {
            session_state_mark_started_external();
          }
        } else if (strstr(payload, "\"session_error\"") != NULL) {
          ESP_LOGW(TAG, "Received command: session_error");
          stop_local_audio_session(false);
          afe_pipeline_notify_session_end();
        } else if (strstr(payload, "\"session_stop\"") != NULL ||
                   strstr(payload, "\"session_close\"") != NULL ||
                   strstr(payload, "\"session_closed\"") != NULL) {
          ESP_LOGI(TAG, "Received session stop command");
          stop_local_audio_session(true);
        } else if (strstr(payload, "\"ping\"") != NULL) {
          // Reply to server ping
          const char *pong_msg = "{\"type\":\"pong\"}";
          if (is_ws_connected()) {
            esp_websocket_client_send_text(s_state.ws_client, pong_msg,
                                           strlen(pong_msg),
                                           pdMS_TO_TICKS(200));
          }
        } else if (strstr(payload, "\"pong\"") != NULL) {
          ESP_LOGD(TAG, "Received pong");
        } else if (strstr(payload, "\"interrupted\"") != NULL) {
          ESP_LOGI(TAG, "Received interrupt command, clearing buffer");

          // Color toggling logic by user request
          if (s_state.session_active) {
            led_state_t cur_state = led_control_get_state();
            if (cur_state == LED_STATE_SPEAKING ||
                cur_state == LED_STATE_LISTENING) {
              // Was green -> set red
              led_control_set_state(LED_STATE_ERROR);
            } else if (cur_state == LED_STATE_ERROR) {
              // Was red -> set green
              led_control_set_state(LED_STATE_SPEAKING);
            }
          }

          if (s_state.playback_buffer) {
            audio_buffer_clear(s_state.playback_buffer);
          }
          id_ring_reset(&s_id_ring);
          // IMPORTANT: Do NOT reset s_current_played_id and
          // s_last_received_server_id! Keep last known IDs for AEC to process
          // the "tail" of echo from audio that was already in the DMA/speaker
          // pipeline. IMPORTANT: Do NOT reset s_current_played_id and
          // s_last_received_server_id! Keep last known IDs to process
          // echo "tail" from audio already in DMA/speaker path.
          // s_current_played_id = -1;  // REMOVED - keep for AEC tail
          // processing
          afe_pipeline_reset();
          s_state.playback_active = false;
          s_state.last_playback_rx_us = 0;
        } else if (is_set_volume) {
          // Active session: keep previous direct behavior for stream path.
          handle_set_volume_command(payload, payload_len);
        } else if (is_gpio_cmd) {
          // Active session: keep previous direct behavior for stream path.
          gpio_control_handle_command(payload, payload_len);
        } else {
          int preview_len = payload_len < 512 ? (int)payload_len : 512;
          ESP_LOGI(TAG, "Received text message (%u bytes): %.*s%s",
                   (unsigned)payload_len, preview_len, payload,
                   payload_len > 512 ? "..." : "");
          // Text usually arrives before audio or during "thinking"
          if (s_state.session_active) {
            led_control_set_state(LED_STATE_THINKING);
          }
        }
        if (payload) {
          free(payload);
        }
      }
    } else if (data->op_code == 0x08) {
      // Close frame
      uint16_t close_code = 0;
      if (data->data_len >= 2) {
        close_code =
            (uint16_t)data->data_ptr[0] << 8 | (uint8_t)data->data_ptr[1];
      }
      ESP_LOGW(TAG, "WebSocket closed by server, code: %d", close_code);
      s_state.ws_connected = false;
      reset_text_payload_buffer();
      stop_local_audio_session(true);
    }
    break;

  case WEBSOCKET_EVENT_ERROR:
    ESP_LOGE(TAG, "WebSocket: connection error");
    if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
      ESP_LOGE(TAG, "  esp-tls: 0x%x", data->error_handle.esp_tls_last_esp_err);
      ESP_LOGE(TAG, "  tls stack: 0x%x", data->error_handle.esp_tls_stack_err);
      ESP_LOGE(TAG, "  socket: %d",
               data->error_handle.esp_transport_sock_errno);
    }

    s_state.ws_connected = false;
    reset_text_payload_buffer();

    if (s_state.session_active) {
      ESP_LOGW(TAG, "Ending logical session due to WebSocket error");
      xTaskCreate(async_session_cleanup_task, "ws_cleanup", 4096, NULL, 5, NULL);
    }
    break;

  default:
    break;
  }
}

// ----------------------------------------------------------------------------
// Transmission task (Queue -> WebSocket)
// ----------------------------------------------------------------------------

static void network_tx_task(void *arg) {
  ESP_LOGI(TAG, "Network TX task started");
  tx_item_t item;
  QueueHandle_t tx_queue = s_state.tx_queue;

  if (!tx_queue) {
    ESP_LOGW(TAG, "TX queue is NULL, stopping network TX task");
    s_state.tx_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  while (s_state.tasks_should_run) {
    if (xQueueReceive(tx_queue, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (s_state.ws_client && s_state.ws_connected && s_state.session_active) {
        esp_websocket_client_send_bin(s_state.ws_client,
                                      (const char *)item.data, item.len,
                                      pdMS_TO_TICKS(500));
      }
      free(item.data); // Free memory allocated by sender
    }
  }

  // Drain queue on exit
  while (xQueueReceive(tx_queue, &item, 0) == pdTRUE) {
    free(item.data);
  }

  // Queue owner is net_tx_task. Delete it only after task exits.
  if (s_state.tx_queue == tx_queue) {
    vQueueDelete(tx_queue);
    s_state.tx_queue = NULL;
  }

  ESP_LOGI(TAG, "Network TX task finished");
  s_state.tx_task = NULL;
  vTaskDelete(NULL);
}

// ----------------------------------------------------------------------------
// Playback task (Buffer -> I2S)
// ----------------------------------------------------------------------------

static void audio_playback_task(void *arg) {
  ESP_LOGI(TAG, "Playback task started");

  // Playback buffers
  const size_t mono_buf_size = PLAYBACK_CHUNK_SAMPLES * sizeof(int16_t);
  const size_t stereo_buf_size = mono_buf_size * 2;
  int16_t *play_buf = heap_caps_malloc(mono_buf_size, MALLOC_CAP_DMA);
  int16_t *tx_buf = heap_caps_malloc(stereo_buf_size, MALLOC_CAP_DMA);

  if (!play_buf || !tx_buf) {
    ESP_LOGE(TAG, "Failed to allocate playback buffer");
    goto cleanup;
  }

  while (s_state.tasks_should_run) {
    int32_t frame_id = -1;

    // Read from adaptive buffer with ID output
    if (s_state.playback_buffer) {
      audio_buffer_read_with_id(s_state.playback_buffer, (uint8_t *)play_buf,
                                mono_buf_size, &frame_id);
      update_playback_activity(
          audio_buffer_is_buffering(s_state.playback_buffer));

      // Push ID into ring buffer and pop with delay
      // ONLY when a real frame_id is received (not -1)
      // This keeps push/pop balanced and prevents buffer drain
      if (frame_id >= 0) {
        id_ring_push(&s_id_ring, frame_id);
        // Pop only after push - otherwise ring drains
        s_current_played_id = id_ring_pop_delayed(&s_id_ring, DMA_DELAY_FRAMES);
      }
      // If buffering is active, s_current_played_id keeps
      // last valid value (or -1 if no real data yet)

      // Notify mic_capture_task that new played_id is ready
      if (s_mic_task_handle) {
        xTaskNotifyGive(s_mic_task_handle);
      }
    } else {
      memset(play_buf, 0, mono_buf_size);
      update_playback_activity(true);
    }

    audio_volume_service_apply_pcm16(play_buf, PLAYBACK_CHUNK_SAMPLES);

    for (size_t i = 0; i < PLAYBACK_CHUNK_SAMPLES; i++) {
      int16_t sample = play_buf[i];
      tx_buf[i * 2] = sample;
      tx_buf[i * 2 + 1] = sample;
    }

    // Write to speaker I2S
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(s_i2s_tx_chan, tx_buf, stereo_buf_size,
                                      &bytes_written, pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
      ESP_LOGW(TAG, "I2S write error: %s", esp_err_to_name(err));
    } else {
      // Written to I2S successfully -> send copy to AFE reference
      size_t mono_samples_written = bytes_written / (2 * sizeof(int16_t));
      if (mono_samples_written > 0) {
        afe_pipeline_feed_reference(play_buf,
                                    mono_samples_written * sizeof(int16_t));
      }
    }
  }

cleanup:
  if (play_buf) {
    heap_caps_free(play_buf);
  }
  if (tx_buf) {
    heap_caps_free(tx_buf);
  }

  ESP_LOGI(TAG, "Playback task finished");
  s_state.playback_task = NULL;
  s_state.playback_active = false;
  s_state.last_playback_rx_us = 0;
  vTaskDelete(NULL);
}

// ----------------------------------------------------------------------------
// Raw microphone PCM capture task (for server-side AEC)
// ----------------------------------------------------------------------------

static void mic_capture_task(void *arg) {
  ESP_LOGI(TAG, "Microphone capture task started");

  // Frame size: 10ms @ 16kHz, 32-bit I2S stereo
  const size_t frame_samples = MIC_CHUNK_SAMPLES; // 160 samples
  const size_t i2s_frame_bytes =
      frame_samples * 2 * sizeof(int32_t); // Stereo 32-bit
  const size_t pcm_frame_bytes = frame_samples * sizeof(int16_t); // Mono 16-bit

  int32_t *i2s_buf = heap_caps_malloc(i2s_frame_bytes, MALLOC_CAP_DMA);
  int16_t *pcm_buf = heap_caps_malloc(pcm_frame_bytes, MALLOC_CAP_INTERNAL);

  if (!i2s_buf || !pcm_buf) {
    ESP_LOGE(TAG, "Failed to allocate microphone buffers");
    goto cleanup;
  }

  int err_count = 0;
  int waiting_frames = 0;        // Frame counter while waiting for AEC ready
  bool aec_ready_logged = false; // First valid ID logged flag

  while (s_state.tasks_should_run) {
    // If logical session is inactive, just wait
    if (!s_state.session_active || !s_state.ws_connected) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_i2s_rx_chan, i2s_buf, i2s_frame_bytes,
                                     &bytes_read, pdMS_TO_TICKS(100));

    if (err != ESP_OK || bytes_read == 0) {
      err_count++;
      if (err_count % 50 == 0) {
        ESP_LOGW(TAG, "Mic capture read fail: err=%d, bytes=%zu", err,
                 bytes_read);
      }
      continue;
    }

    // Half-duplex: do not send mic while playback is active
    if (s_half_duplex_mode && s_state.playback_active) {
      // Drain accumulated notifications to avoid counter overflow
      ulTaskNotifyTake(pdTRUE, 0);
      waiting_frames = 0;
      aec_ready_logged = false;
      continue;
    }

    // Convert 32-bit I2S (INMP441 24-bit left-aligned) to 16-bit PCM
    size_t samples_read = bytes_read / (2 * sizeof(int32_t));
    for (size_t i = 0; i < samples_read && i < frame_samples; i++) {
      // Left channel = i*2, proper conversion with clipping
      pcm_buf[i] = i2s_sample32_to_pcm16(i2s_buf[i * 2]);
    }

    // Wait for notification from playback_task (played_id synchronization)
    // 20ms timeout prevents deadlock when playback is absent
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));

    // Get current seq_id of played frame (already synchronized)
    int32_t seq_id = voice_client_get_current_played_id();

    // AEC FALLBACK: If played_id is -1 (buffering), use last received server ID
    // This allows server to apply approximate AEC during startup/buffering
    // If played_id = -1 (buffering), use last received ID.
    // This lets server apply approximate AEC during startup/buffering.
    if (seq_id < 0 && s_last_received_server_id >= 0) {
      seq_id = s_last_received_server_id;
      // Log only first few times to avoid spam
      if (waiting_frames < 10) {
        ESP_LOGI(TAG, "AEC fallback: using last_received_id=%ld instead of -1",
                 (long)seq_id);
      }
    }

    // Log transition to AEC-ready mode
    if (seq_id >= 0 && !aec_ready_logged) {
      ESP_LOGI(TAG,
               "AEC sync: first valid played_id=%ld after %d frames (~%dms)",
               (long)seq_id, waiting_frames, waiting_frames * 10);
      aec_ready_logged = true;
    } else if (seq_id < 0) {
      waiting_frames++;
    }

    // ALWAYS send mic audio:
    // - played_id=-1: buffering without fallback, server applies fallback AEC
    // - played_id>=0: playback or fallback, server applies AEC with
    // reference
    voice_client_send_audio((const uint8_t *)pcm_buf, pcm_frame_bytes, seq_id);
  }

cleanup:
  if (i2s_buf)
    heap_caps_free(i2s_buf);
  if (pcm_buf)
    heap_caps_free(pcm_buf);

  ESP_LOGI(TAG, "Microphone capture task finished");
  s_state.mic_capture_task = NULL;
  vTaskDelete(NULL);
}

// ----------------------------------------------------------------------------
// Helper functions
// ----------------------------------------------------------------------------

static void stop_audio_tasks(void) {
  ESP_LOGI(TAG, "Stopping audio tasks...");

  s_state.tasks_should_run = false;
  s_state.playback_active = false;
  s_mic_task_handle = NULL; // Reset handle for Task Notification
  s_state.last_playback_rx_us = 0;

  // Wait for tasks to finish
  int timeout = 20; // 2 seconds
  while (
      (s_state.playback_task || s_state.tx_task || s_state.mic_capture_task) &&
      timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (s_state.playback_task || s_state.tx_task || s_state.mic_capture_task) {
    ESP_LOGW(TAG, "Tasks did not finish in time");
  }
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

static esp_err_t ensure_ws_client_locked(void) {
  if (s_state.ws_client) {
    return ESP_OK;
  }

  if (!s_cfg) {
    s_cfg = runtime_config_get();
  }

  // Load embedded certificate
  extern const char public_cert_pem_start[];

  snprintf(s_state.uri, sizeof(s_state.uri), "wss://%s:%d%s",
           s_cfg->server_host, (int)s_cfg->server_port, s_cfg->voice_ws_path);

  int header_len = snprintf(s_state.headers, sizeof(s_state.headers),
                            "Authorization: Bearer %s\r\n"
                            "X-Client-Mode: %s\r\n",
                            s_cfg->voice_api_key,
                            duplex_mode_to_string(s_effective_duplex_mode));
  if (header_len < 0 || (size_t)header_len >= sizeof(s_state.headers)) {
    ESP_LOGE(TAG, "WS header buffer overflow at base headers");
    return ESP_ERR_INVALID_SIZE;
  }

  size_t used = (size_t)header_len;
  if (s_cfg->server_location[0] != '\0') {
    int n = snprintf(s_state.headers + used, sizeof(s_state.headers) - used,
                     "X-Location: %s\r\n", s_cfg->server_location);
    if (n < 0 || (size_t)n >= sizeof(s_state.headers) - used) {
      ESP_LOGE(TAG, "WS header buffer overflow at X-Location");
      return ESP_ERR_INVALID_SIZE;
    }
    used += (size_t)n;
  }

  if (s_cfg->server_parameter[0] != '\0') {
    int n = snprintf(s_state.headers + used, sizeof(s_state.headers) - used,
                     "X-Parameter: %s\r\n", s_cfg->server_parameter);
    if (n < 0 || (size_t)n >= sizeof(s_state.headers) - used) {
      ESP_LOGE(TAG, "WS header buffer overflow at X-Parameter");
      return ESP_ERR_INVALID_SIZE;
    }
    used += (size_t)n;
  }

  esp_websocket_client_config_t ws_cfg = {
      .uri = s_state.uri,
      .buffer_size = 4096,
      .task_stack = 6144,
      .pingpong_timeout_sec = 30,
      .disable_auto_reconnect = true,
      .network_timeout_ms = 10000,
      .cert_pem = public_cert_pem_start,
      .skip_cert_common_name_check = true,
      .headers = s_state.headers,
  };

  s_state.ws_client = esp_websocket_client_init(&ws_cfg);
  if (!s_state.ws_client) {
    ESP_LOGE(TAG, "Failed to create WebSocket client");
    return ESP_FAIL;
  }

  esp_websocket_register_events(s_state.ws_client, WEBSOCKET_EVENT_ANY,
                                websocket_event_handler, NULL);
  ESP_LOGI(TAG, "WS custom headers: client_mode=%s, location=%s, parameter=%s",
           duplex_mode_to_string(s_effective_duplex_mode),
           (s_cfg->server_location[0] != '\0') ? s_cfg->server_location : "(empty)",
           (s_cfg->server_parameter[0] != '\0') ? s_cfg->server_parameter : "(empty)");
  ESP_LOGI(TAG, "WebSocket client prepared: %s", s_state.uri);
  return ESP_OK;
}

static void reset_ws_client_locked(const char *reason) {
  esp_websocket_client_handle_t ws_client = s_state.ws_client;
  s_state.ws_client = NULL;
  s_state.ws_connected = false;
  reset_text_payload_buffer();

  if (!ws_client) {
    return;
  }

  ESP_LOGW(TAG, "Reset WebSocket client (%s)",
           reason ? reason : "unspecified");

  // IMPORTANT: stop/destroy may trigger DISCONNECTED callback, which calls
  // stop_local_audio_session() and takes state_mutex. Holding mutex during
  // stop/destroy would cause deadlock.
  xSemaphoreGive(s_state.state_mutex);
  esp_websocket_client_stop(ws_client);
  esp_websocket_client_destroy(ws_client);
  xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
}

static bool is_ws_connected(void) {
  return s_state.ws_client && s_state.ws_connected;
}

static esp_err_t request_session_start(void) {
  if (!is_ws_connected()) {
    ESP_LOGW(TAG, "session_start: no active WSS connection");
    return ESP_ERR_INVALID_STATE;
  }

  const char *msg = "{\"type\":\"session_start\"}";
  int written = esp_websocket_client_send_text(s_state.ws_client, msg, strlen(msg),
                                               pdMS_TO_TICKS(500));
  return (written >= 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t request_session_stop(void) {
  if (!is_ws_connected()) {
    return ESP_ERR_INVALID_STATE;
  }

  const char *msg = "{\"type\":\"session_stop\"}";
  int written = esp_websocket_client_send_text(s_state.ws_client, msg, strlen(msg),
                                               pdMS_TO_TICKS(500));
  return (written >= 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t start_local_audio_session(void) {
  if (!s_cfg) {
    s_cfg = runtime_config_get();
  }

  xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);

  if (s_state.session_active) {
    xSemaphoreGive(s_state.state_mutex);
    return ESP_OK;
  }

  if (!is_ws_connected()) {
    xSemaphoreGive(s_state.state_mutex);
    return ESP_ERR_INVALID_STATE;
  }

  if (s_state.playback_task || s_state.tx_task || s_state.mic_capture_task) {
    ESP_LOGW(TAG, "Previous audio tasks are still shutting down, start deferred");
    xSemaphoreGive(s_state.state_mutex);
    return ESP_ERR_INVALID_STATE;
  }

  if (s_state.tx_queue) {
    tx_item_t item;
    while (xQueueReceive(s_state.tx_queue, &item, 0) == pdTRUE) {
      free(item.data);
    }
    vQueueDelete(s_state.tx_queue);
    s_state.tx_queue = NULL;
  }

  s_state.playback_buffer = audio_buffer_create(
      s_cfg->audio_playback_sample_rate, s_cfg->audio_buffer_max_seconds,
      s_cfg->audio_buffer_start_threshold_ms);
  if (!s_state.playback_buffer) {
    ESP_LOGE(TAG, "Failed to create playback buffer");
    xSemaphoreGive(s_state.state_mutex);
    return ESP_ERR_NO_MEM;
  }

  s_state.tx_queue = xQueueCreate(10, sizeof(tx_item_t));
  if (!s_state.tx_queue) {
    ESP_LOGE(TAG, "Failed to create TX queue");
    audio_buffer_destroy(s_state.playback_buffer);
    s_state.playback_buffer = NULL;
    xSemaphoreGive(s_state.state_mutex);
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err =
      reconfigure_speaker_sample_rate(s_cfg->audio_playback_sample_rate);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to switch speaker sample rate: %s",
             esp_err_to_name(err));
  }

  s_state.tasks_should_run = true;
  s_state.playback_active = false;
  s_state.last_playback_rx_us = 0;
  id_ring_reset(&s_id_ring);
  s_current_played_id = -1;
  s_last_received_server_id = -1;

  BaseType_t ret = xTaskCreatePinnedToCore(audio_playback_task, "playback_task",
                                           AUDIO_TASK_STACK_SIZE, NULL, 10,
                                           &s_state.playback_task, 1);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create playback_task");
    vQueueDelete(s_state.tx_queue);
    s_state.tx_queue = NULL;
    audio_buffer_destroy(s_state.playback_buffer);
    s_state.playback_buffer = NULL;
    s_state.tasks_should_run = false;
    xSemaphoreGive(s_state.state_mutex);
    return ESP_FAIL;
  }

  ret = xTaskCreatePinnedToCore(network_tx_task, "net_tx_task", 4096, NULL, 4,
                                &s_state.tx_task, 0);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create net_tx_task");
    s_state.tasks_should_run = false;
    xSemaphoreGive(s_state.state_mutex);
    stop_local_audio_session(false);
    return ESP_FAIL;
  }

  ret = xTaskCreatePinnedToCore(mic_capture_task, "mic_capture", 4096, NULL, 10,
                                &s_mic_task_handle, 1);
  s_state.mic_capture_task = s_mic_task_handle;
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create mic_capture_task");
    s_state.tasks_should_run = false;
    xSemaphoreGive(s_state.state_mutex);
    stop_local_audio_session(false);
    return ESP_FAIL;
  }

  s_state.session_active = true;
  xSemaphoreGive(s_state.state_mutex);

  afe_pipeline_notify_session_start();
  ESP_LOGI(TAG, "Local voice session started");
  return ESP_OK;
}

static bool stop_local_audio_session(bool notify_session_end) {
  bool was_active = false;

  xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
  if (s_state.session_active || s_state.playback_buffer || s_state.tx_queue ||
      s_state.playback_task || s_state.tx_task || s_state.mic_capture_task) {
    was_active = true;
  }

  s_state.session_active = false;
  s_state.playback_active = false;
  s_state.last_playback_rx_us = 0;
  s_current_played_id = -1;
  s_last_received_server_id = -1;
  id_ring_reset(&s_id_ring);

  if (s_state.tasks_should_run || s_state.playback_task || s_state.tx_task ||
      s_state.mic_capture_task) {
    stop_audio_tasks();
  }

  if (s_state.tx_queue && !s_state.tx_task) {
    tx_item_t item;
    while (xQueueReceive(s_state.tx_queue, &item, 0) == pdTRUE) {
      free(item.data);
    }
    vQueueDelete(s_state.tx_queue);
    s_state.tx_queue = NULL;
  } else if (s_state.tx_queue && s_state.tx_task) {
    ESP_LOGW(TAG, "TX task still running, defer tx_queue deletion");
  }

  if (s_state.playback_buffer) {
    audio_buffer_destroy(s_state.playback_buffer);
    s_state.playback_buffer = NULL;
  }

  reconfigure_speaker_sample_rate(16000);
  xSemaphoreGive(s_state.state_mutex);

  if (notify_session_end) {
    afe_pipeline_notify_session_end();
  }

  return was_active;
}

static void connection_maintainer_task(void *arg) {
  ESP_LOGI(TAG, "Connection maintainer started");

  while (s_state.client_running) {
    if (!wifi_manager_is_connected()) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    bool should_reconnect = false;
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);

    bool ws_alive = s_state.ws_client &&
                    esp_websocket_client_is_connected(s_state.ws_client);
    s_state.ws_connected = ws_alive;

    if (ensure_ws_client_locked() == ESP_OK && !ws_alive) {
      if (s_state.last_connect_attempt_us == 0 ||
          (now - s_state.last_connect_attempt_us) >=
              (int64_t)RECONNECT_INTERVAL_MS * 1000) {
        s_state.last_connect_attempt_us = now;
        should_reconnect = true;
      }
    } else if (ws_alive) {
      s_state.last_connect_attempt_us = 0;
    }

    xSemaphoreGive(s_state.state_mutex);

    if (should_reconnect) {
      ESP_LOGI(TAG, "Attempting to connect to voice-assistant...");

      esp_websocket_client_handle_t ws_handle = NULL;
      xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
      // Recreate client on each attempt to avoid stuck transport/ssl state
      // after long server offline periods.
      reset_ws_client_locked("scheduled reconnect");
      if (ensure_ws_client_locked() == ESP_OK) {
        ws_handle = s_state.ws_client;
      }
      xSemaphoreGive(s_state.state_mutex);

      if (ws_handle) {
        esp_err_t err = esp_websocket_client_start(ws_handle);
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "WebSocket start failed: %s", esp_err_to_name(err));
          xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
          if (s_state.ws_client == ws_handle) {
            reset_ws_client_locked("start failed");
          }
          xSemaphoreGive(s_state.state_mutex);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }

  ESP_LOGI(TAG, "Connection maintainer stopped");
  s_state.connection_task = NULL;
  vTaskDelete(NULL);
}

static void heartbeat_task(void *arg) {
  ESP_LOGI(TAG, "Heartbeat task started");

  while (s_state.client_running) {
    if (is_ws_connected() && !s_state.session_active) {
      const char *ping_msg = "{\"type\":\"ping\"}";
      int written = esp_websocket_client_send_text(
          s_state.ws_client, ping_msg, strlen(ping_msg), pdMS_TO_TICKS(200));

      if (written < 0) {
        ESP_LOGW(TAG, "Heartbeat ping failed, forcing reconnect");
        xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
        reset_ws_client_locked("heartbeat ping failed");
        xSemaphoreGive(s_state.state_mutex);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
  }

  ESP_LOGI(TAG, "Heartbeat task stopped");
  s_state.heartbeat_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t voice_client_init(voice_client_session_end_cb_t on_session_end) {
  ESP_LOGI(TAG, "Initializing voice client (persistent connection)");

  reset_text_payload_buffer();
  memset(&s_state, 0, sizeof(s_state));
  s_state.on_session_end = on_session_end;
  s_state.client_running = true;

  esp_err_t cfg_err = runtime_config_init();
  if (cfg_err != ESP_OK) {
    ESP_LOGW(TAG, "runtime_config_init failed: %s (using defaults)",
             esp_err_to_name(cfg_err));
  }
  s_cfg = runtime_config_get();

  s_state.state_mutex = xSemaphoreCreateMutex();
  if (!s_state.state_mutex) {
    ESP_LOGE(TAG, "Failed to create mutex");
    return ESP_ERR_NO_MEM;
  }

  s_control_msg_queue = xQueueCreate(CONTROL_MSG_QUEUE_LEN, sizeof(control_msg_t));
  if (!s_control_msg_queue) {
    ESP_LOGE(TAG, "Failed to create control message queue");
    vSemaphoreDelete(s_state.state_mutex);
    s_state.state_mutex = NULL;
    return ESP_ERR_NO_MEM;
  }

  BaseType_t control_ret = xTaskCreatePinnedToCore(
      control_message_task, "ctrl_msg", CONTROL_MSG_TASK_STACK, NULL,
      CONTROL_MSG_TASK_PRIO, &s_control_msg_task, 0);
  if (control_ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create control message task");
    vQueueDelete(s_control_msg_queue);
    s_control_msg_queue = NULL;
    vSemaphoreDelete(s_state.state_mutex);
    s_state.state_mutex = NULL;
    return ESP_FAIL;
  }

  s_volume_persist_queue = xQueueCreate(1, sizeof(uint32_t));
  if (!s_volume_persist_queue) {
    ESP_LOGE(TAG, "Failed to create volume persist queue");
    if (s_control_msg_task) {
      vTaskDelete(s_control_msg_task);
      s_control_msg_task = NULL;
    }
    if (s_control_msg_queue) {
      vQueueDelete(s_control_msg_queue);
      s_control_msg_queue = NULL;
    }
    vSemaphoreDelete(s_state.state_mutex);
    s_state.state_mutex = NULL;
    return ESP_ERR_NO_MEM;
  }

  BaseType_t persist_ret = xTaskCreatePinnedToCore(
      volume_persist_task, "vol_persist", VOLUME_PERSIST_TASK_STACK, NULL,
      VOLUME_PERSIST_TASK_PRIO, &s_volume_persist_task, 0);
  if (persist_ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create volume persist task");
    vQueueDelete(s_volume_persist_queue);
    s_volume_persist_queue = NULL;
    if (s_control_msg_task) {
      vTaskDelete(s_control_msg_task);
      s_control_msg_task = NULL;
    }
    if (s_control_msg_queue) {
      vQueueDelete(s_control_msg_queue);
      s_control_msg_queue = NULL;
    }
    vSemaphoreDelete(s_state.state_mutex);
    s_state.state_mutex = NULL;
    return ESP_FAIL;
  }

  resolve_speak_mode_config();

  BaseType_t ret = xTaskCreatePinnedToCore(connection_maintainer_task,
                                           "ws_maintainer", 4096, NULL, 5,
                                           &s_state.connection_task, 0);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create connection maintainer");
    if (s_volume_persist_task) {
      vTaskDelete(s_volume_persist_task);
      s_volume_persist_task = NULL;
    }
    if (s_volume_persist_queue) {
      vQueueDelete(s_volume_persist_queue);
      s_volume_persist_queue = NULL;
    }
    if (s_control_msg_task) {
      vTaskDelete(s_control_msg_task);
      s_control_msg_task = NULL;
    }
    if (s_control_msg_queue) {
      vQueueDelete(s_control_msg_queue);
      s_control_msg_queue = NULL;
    }
    vSemaphoreDelete(s_state.state_mutex);
    s_state.state_mutex = NULL;
    return ESP_FAIL;
  }

  ret = xTaskCreatePinnedToCore(heartbeat_task, "ws_heartbeat", 3072, NULL, 4,
                                &s_state.heartbeat_task, 0);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create heartbeat task");
    s_state.client_running = false;
    if (s_volume_persist_task) {
      vTaskDelete(s_volume_persist_task);
      s_volume_persist_task = NULL;
    }
    if (s_volume_persist_queue) {
      vQueueDelete(s_volume_persist_queue);
      s_volume_persist_queue = NULL;
    }
    if (s_control_msg_task) {
      vTaskDelete(s_control_msg_task);
      s_control_msg_task = NULL;
    }
    if (s_control_msg_queue) {
      vQueueDelete(s_control_msg_queue);
      s_control_msg_queue = NULL;
    }
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t voice_client_start(void) {
  xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
  if (s_state.session_active) {
    xSemaphoreGive(s_state.state_mutex);
    return ESP_OK;
  }

  esp_err_t err = request_session_start();
  xSemaphoreGive(s_state.state_mutex);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "session_start sent");
  }
  return err;
}

esp_err_t voice_client_stop(void) {
  xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
  esp_err_t err = request_session_stop();
  bool connected = is_ws_connected();
  xSemaphoreGive(s_state.state_mutex);

  if (!connected) {
    stop_local_audio_session(true);
    return ESP_ERR_INVALID_STATE;
  }

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "session_stop sent");
  }
  return err;
}

bool voice_client_is_active(void) { return s_state.session_active; }

bool voice_client_is_connected(void) { return is_ws_connected(); }

esp_err_t voice_client_send_audio(const uint8_t *data, size_t len,
                                  int32_t played_id) {
  if (!s_state.ws_client || !s_state.ws_connected || !s_state.session_active ||
      !s_state.tx_queue) {
    return ESP_ERR_INVALID_STATE;
  }

  size_t packet_len = sizeof(int32_t) + len;
  uint8_t *packet = malloc(packet_len);
  if (!packet) {
    ESP_LOGE(TAG, "OOM in send_audio");
    return ESP_ERR_NO_MEM;
  }

  memcpy(packet, &played_id, sizeof(int32_t));
  memcpy(packet + sizeof(int32_t), data, len);

  tx_item_t item = {.data = packet, .len = packet_len};
  if (xQueueSend(s_state.tx_queue, &item, 0) != pdTRUE) {
    free(packet);
    return ESP_FAIL;
  }

  return ESP_OK;
}

int32_t voice_client_get_current_played_id(void) { return s_current_played_id; }

audio_buffer_t *voice_client_get_playback_buffer(void) {
  return s_state.playback_buffer;
}

esp_err_t voice_client_deinit(void) {
  s_state.client_running = false;
  reset_text_payload_buffer();
  stop_local_audio_session(false);

  // Give background task time to finish
  for (int i = 0;
       i < 20 &&
       (s_state.connection_task || s_state.heartbeat_task || s_control_msg_task ||
        s_volume_persist_task);
       i++) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (s_control_msg_queue && !s_control_msg_task) {
    control_msg_t msg = {0};
    while (xQueueReceive(s_control_msg_queue, &msg, 0) == pdTRUE) {
      if (msg.payload) {
        free(msg.payload);
      }
    }
    vQueueDelete(s_control_msg_queue);
    s_control_msg_queue = NULL;
  } else if (s_control_msg_task) {
    ESP_LOGW(TAG, "Control message task still running during deinit");
  }

  if (s_volume_persist_queue && !s_volume_persist_task) {
    vQueueDelete(s_volume_persist_queue);
    s_volume_persist_queue = NULL;
  } else if (s_volume_persist_task) {
    ESP_LOGW(TAG, "Volume persist task still running during deinit");
  }

  xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
  reset_ws_client_locked("deinit");
  xSemaphoreGive(s_state.state_mutex);

  if (s_state.state_mutex) {
    vSemaphoreDelete(s_state.state_mutex);
    s_state.state_mutex = NULL;
  }

  return ESP_OK;
}

static void update_playback_activity(bool buffering) {
  int64_t now = esp_timer_get_time();

  if (!buffering) {
    s_state.playback_active = true;
    return;
  }

  if (s_state.last_playback_rx_us > 0 &&
      (now - s_state.last_playback_rx_us) <
          (int64_t)PLAYBACK_ACTIVE_HOLD_MS * 1000) {
    s_state.playback_active = true;
    return;
  }

  s_state.playback_active = false;
}
