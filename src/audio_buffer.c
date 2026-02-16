/**
 * @file audio_buffer.c
 * @brief Adaptive ring buffer for audio playback
 *
 * Buffering logic ported from Go reference (voice-client-go/main.go):
 *
 * 1. BUFFERING MODE (isBuffering = true):
 *    - If buffer is empty -> switch to buffering mode
 *    - Wait until startThreshold (300ms) data is accumulated
 *    - Return silence while buffering
 *
 * 2. PLAYBACK MODE (isBuffering = false):
 *    - Serve data from buffer
 *    - If less data than requested (underrun), return available data,
 *      pad with silence, and switch back to buffering mode
 *
 * 3. OVERFLOW PROTECTION:
 *    - Buffer is limited by maxSeconds (60 seconds)
 *    - Old data is discarded on overflow
 */

#include "audio_buffer.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "AUDIO_BUF";

// Buffer level logging interval (us)
#define LOG_INTERVAL_US (500 * 1000) // 500ms

// ----------------------------------------------------------------------------
// Creation and destruction
// ----------------------------------------------------------------------------

audio_buffer_t *audio_buffer_create(uint32_t sample_rate, uint32_t max_seconds,
                                    uint32_t start_threshold_ms) {
  // Allocate structure from regular memory
  audio_buffer_t *buffer = calloc(1, sizeof(audio_buffer_t));
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate memory for buffer structure");
    return NULL;
  }

  // Parameters
  buffer->sample_rate = sample_rate;
  buffer->bytes_per_sample = 2; // 16-bit mono
  buffer->frame_size = 320;     // 10ms @ 16kHz (160 samples * 2 bytes)
  buffer->start_threshold =
      (sample_rate * buffer->bytes_per_sample * start_threshold_ms) / 1000;
  buffer->capacity = sample_rate * buffer->bytes_per_sample * max_seconds;
  buffer->id_capacity = buffer->capacity / buffer->frame_size;

  ESP_LOGI(TAG,
           "Creating buffer: capacity=%u bytes (%.1f sec), threshold=%u "
           "bytes (%.0f ms), id_capacity=%u",
           (unsigned)buffer->capacity, (float)max_seconds,
           (unsigned)buffer->start_threshold, (float)start_threshold_ms,
           (unsigned)buffer->id_capacity);

  // Allocate data in PSRAM
  buffer->data = heap_caps_calloc(1, buffer->capacity,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer->data) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM for buffer data (%u bytes)",
             (unsigned)buffer->capacity);
    free(buffer);
    return NULL;
  }

  // Allocate ID buffer in PSRAM (parallel buffer)
  buffer->id_buffer = heap_caps_calloc(buffer->id_capacity, sizeof(int32_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer->id_buffer) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM for ID buffer (%u entries)",
             (unsigned)buffer->id_capacity);
    heap_caps_free(buffer->data);
    free(buffer);
    return NULL;
  }
  // Initialize IDs with -1 (invalid ID)
  for (size_t i = 0; i < buffer->id_capacity; i++) {
    buffer->id_buffer[i] = -1;
  }

  // Create mutex
  buffer->mutex = xSemaphoreCreateMutex();
  if (!buffer->mutex) {
    ESP_LOGE(TAG, "Failed to create mutex");
    heap_caps_free(buffer->id_buffer);
    heap_caps_free(buffer->data);
    free(buffer);
    return NULL;
  }

  // Start in buffering mode
  buffer->is_buffering = true;
  buffer->head = 0;
  buffer->tail = 0;
  buffer->count = 0;
  buffer->id_head = 0;
  buffer->id_tail = 0;
  buffer->id_count = 0;
  buffer->last_log_time = 0;

  ESP_LOGI(TAG, "Buffer created successfully");
  return buffer;
}

void audio_buffer_destroy(audio_buffer_t *buffer) {
  if (!buffer) {
    return;
  }

  ESP_LOGI(TAG, "Destroying buffer");

  if (buffer->mutex) {
    vSemaphoreDelete(buffer->mutex);
  }

  if (buffer->id_buffer) {
    heap_caps_free(buffer->id_buffer);
  }

  if (buffer->data) {
    heap_caps_free(buffer->data);
  }

  free(buffer);
}

// ----------------------------------------------------------------------------
// Write data
// ----------------------------------------------------------------------------

size_t audio_buffer_write(audio_buffer_t *buffer, const uint8_t *data,
                          size_t len) {
  if (!buffer || !data || len == 0) {
    return 0;
  }

  xSemaphoreTake(buffer->mutex, portMAX_DELAY);

  // If input data exceeds capacity, keep only the tail
  if (len > buffer->capacity) {
    data += len - buffer->capacity;
    len = buffer->capacity;
  }

  // Write data
  size_t written = 0;
  while (written < len) {
    size_t chunk = len - written;

    // Limit chunk size to end of buffer
    if (buffer->head + chunk > buffer->capacity) {
      chunk = buffer->capacity - buffer->head;
    }

    memcpy(buffer->data + buffer->head, data + written, chunk);
    buffer->head = (buffer->head + chunk) % buffer->capacity;
    written += chunk;
    buffer->count += chunk;
  }

  // Overflow protection: discard old data
  if (buffer->count > buffer->capacity) {
    size_t overflow = buffer->count - buffer->capacity;
    buffer->tail = (buffer->tail + overflow) % buffer->capacity;
    buffer->count = buffer->capacity;
  }

  // Buffer level logging
  int64_t now = esp_timer_get_time();
  if (now - buffer->last_log_time > LOG_INTERVAL_US) {
    float level_sec = (float)buffer->count /
                      (float)(buffer->sample_rate * buffer->bytes_per_sample);
    const char *state = buffer->is_buffering ? "BUF " : "PLAY";
    ESP_LOGI(TAG, "Buffer: %.2fs [%s]", level_sec, state);
    buffer->last_log_time = now;
  }

  xSemaphoreGive(buffer->mutex);
  return written;
}

// ----------------------------------------------------------------------------
// Read data (adaptive logic)
// ----------------------------------------------------------------------------

size_t audio_buffer_read(audio_buffer_t *buffer, uint8_t *data, size_t len) {
  if (!buffer || !data || len == 0) {
    return 0;
  }

  xSemaphoreTake(buffer->mutex, portMAX_DELAY);

  // If buffer is empty, switch to buffering mode
  if (buffer->count == 0) {
    buffer->is_buffering = true;
  }

  // In buffering mode, wait for enough data
  if (buffer->is_buffering) {
    if (buffer->count < buffer->start_threshold) {
      // Return silence
      memset(data, 0, len);
      xSemaphoreGive(buffer->mutex);
      return len;
    }
    // Threshold reached - start playback
    buffer->is_buffering = false;
    ESP_LOGI(TAG, "Starting playback (buffered %.2f sec)",
             (float)buffer->count /
                 (float)(buffer->sample_rate * buffer->bytes_per_sample));
  }

  // If enough data is available, read full chunk
  if (buffer->count >= len) {
    size_t read_total = 0;
    while (read_total < len) {
      size_t chunk = len - read_total;

      // Limit chunk size to end of buffer
      if (buffer->tail + chunk > buffer->capacity) {
        chunk = buffer->capacity - buffer->tail;
      }

      memcpy(data + read_total, buffer->data + buffer->tail, chunk);
      buffer->tail = (buffer->tail + chunk) % buffer->capacity;
      read_total += chunk;
      buffer->count -= chunk;
    }

    xSemaphoreGive(buffer->mutex);
    return len;
  }

  // Underrun: less data than requested
  // Return available data + silence, then switch to buffering mode
  if (buffer->count > 0) {
    size_t available = buffer->count;
    size_t read_total = 0;

    while (read_total < available) {
      size_t chunk = available - read_total;

      if (buffer->tail + chunk > buffer->capacity) {
        chunk = buffer->capacity - buffer->tail;
      }

      memcpy(data + read_total, buffer->data + buffer->tail, chunk);
      buffer->tail = (buffer->tail + chunk) % buffer->capacity;
      read_total += chunk;
    }

    buffer->count = 0;

    // Fill remainder with silence
    memset(data + available, 0, len - available);

    // Switch to buffering mode
    buffer->is_buffering = true;
    ESP_LOGW(TAG, "Underrun: delivered %u bytes, remainder is silence",
             (unsigned)available);

    xSemaphoreGive(buffer->mutex);
    return len;
  }

  // Buffer is empty - return silence
  memset(data, 0, len);
  xSemaphoreGive(buffer->mutex);
  return len;
}

// ----------------------------------------------------------------------------
// Clear buffer (on interruption)
// ----------------------------------------------------------------------------

void audio_buffer_clear(audio_buffer_t *buffer) {
  if (!buffer) {
    return;
  }

  xSemaphoreTake(buffer->mutex, portMAX_DELAY);

  buffer->head = 0;
  buffer->tail = 0;
  buffer->count = 0;
  buffer->id_head = 0;
  buffer->id_tail = 0;
  buffer->id_count = 0;
  // Do NOT reset is_buffering - let Read decide

  ESP_LOGI(TAG, "Buffer cleared (interrupted)");

  xSemaphoreGive(buffer->mutex);
}

// ----------------------------------------------------------------------------
// Write/read with ID (for AEC synchronization)
// ----------------------------------------------------------------------------

size_t audio_buffer_write_with_id(audio_buffer_t *buffer, const uint8_t *data,
                                  size_t len, int32_t id) {
  if (!buffer || !data || len == 0) {
    return 0;
  }

  xSemaphoreTake(buffer->mutex, portMAX_DELAY);

  // Write ID to parallel buffer (one ID per frame_size bytes)
  size_t frames_to_write = len / buffer->frame_size;

  if (frames_to_write > 0 && buffer->id_buffer) {
    for (size_t f = 0; f < frames_to_write; f++) {
      buffer->id_buffer[buffer->id_head] = id;
      buffer->id_head = (buffer->id_head + 1) % buffer->id_capacity;

      if (buffer->id_count < buffer->id_capacity) {
        buffer->id_count++;
      } else {
        buffer->id_tail = (buffer->id_tail + 1) % buffer->id_capacity;
      }
    }
  }

  // Write audio data (same logic as audio_buffer_write)
  if (len > buffer->capacity) {
    data += len - buffer->capacity;
    len = buffer->capacity;
  }

  size_t written = 0;
  while (written < len) {
    size_t chunk = len - written;
    if (buffer->head + chunk > buffer->capacity) {
      chunk = buffer->capacity - buffer->head;
    }
    memcpy(buffer->data + buffer->head, data + written, chunk);
    buffer->head = (buffer->head + chunk) % buffer->capacity;
    written += chunk;
    buffer->count += chunk;
  }

  // Overflow protection
  if (buffer->count > buffer->capacity) {
    size_t overflow = buffer->count - buffer->capacity;
    buffer->tail = (buffer->tail + overflow) % buffer->capacity;
    buffer->count = buffer->capacity;
  }

  xSemaphoreGive(buffer->mutex);
  return written;
}

size_t audio_buffer_read_with_id(audio_buffer_t *buffer, uint8_t *data,
                                 size_t len, int32_t *out_id) {
  if (!buffer || !data || len == 0) {
    if (out_id)
      *out_id = -1;
    return 0;
  }

  xSemaphoreTake(buffer->mutex, portMAX_DELAY);

  // If buffer is empty, switch to buffering mode
  if (buffer->count == 0) {
    buffer->is_buffering = true;
  }

  // In buffering mode, wait for enough data
  if (buffer->is_buffering) {
    if (buffer->count < buffer->start_threshold) {
      memset(data, 0, len);
      if (out_id)
        *out_id = -1; // Silence - no ID
      xSemaphoreGive(buffer->mutex);
      return len;
    }
    buffer->is_buffering = false;
    ESP_LOGI(TAG, "Starting playback (buffered %.2f sec)",
             (float)buffer->count /
                 (float)(buffer->sample_rate * buffer->bytes_per_sample));
  }

  // Enough data available - read a full frame
  if (buffer->count >= len) {
    // IMPORTANT: Read ID AFTER buffering check, BEFORE reading data
    // This guarantees ID matches returned data
    int32_t frame_id = -1;
    if (buffer->id_buffer && buffer->id_count > 0) {
      frame_id = buffer->id_buffer[buffer->id_tail];
    }

    size_t read_total = 0;
    while (read_total < len) {
      size_t chunk = len - read_total;
      if (buffer->tail + chunk > buffer->capacity) {
        chunk = buffer->capacity - buffer->tail;
      }
      memcpy(data + read_total, buffer->data + buffer->tail, chunk);
      buffer->tail = (buffer->tail + chunk) % buffer->capacity;
      read_total += chunk;
      buffer->count -= chunk;
    }

    // Advance ID buffer (one ID per frame)
    size_t frames_read = len / buffer->frame_size;
    if (buffer->id_buffer && frames_read > 0) {
      for (size_t f = 0; f < frames_read && buffer->id_count > 0; f++) {
        buffer->id_tail = (buffer->id_tail + 1) % buffer->id_capacity;
        buffer->id_count--;
      }
    }

    if (out_id)
      *out_id = frame_id;
    xSemaphoreGive(buffer->mutex);
    return len;
  }

  // Underrun - return available data
  if (buffer->count > 0) {
    // Read ID before data
    int32_t frame_id = -1;
    if (buffer->id_buffer && buffer->id_count > 0) {
      frame_id = buffer->id_buffer[buffer->id_tail];
    }

    size_t available = buffer->count;
    size_t read_total = 0;
    while (read_total < available) {
      size_t chunk = available - read_total;
      if (buffer->tail + chunk > buffer->capacity) {
        chunk = buffer->capacity - buffer->tail;
      }
      memcpy(data + read_total, buffer->data + buffer->tail, chunk);
      buffer->tail = (buffer->tail + chunk) % buffer->capacity;
      read_total += chunk;
    }
    buffer->count = 0;

    // Reset ID buffer on underrun
    buffer->id_tail = buffer->id_head;
    buffer->id_count = 0;

    memset(data + available, 0, len - available);
    buffer->is_buffering = true;

    if (out_id)
      *out_id = frame_id;
    xSemaphoreGive(buffer->mutex);
    return len;
  }

  // Buffer is empty
  memset(data, 0, len);
  if (out_id)
    *out_id = -1;
  xSemaphoreGive(buffer->mutex);
  return len;
}

// ----------------------------------------------------------------------------
// Helper functions
// ----------------------------------------------------------------------------

float audio_buffer_get_level_seconds(const audio_buffer_t *buffer) {
  if (!buffer || buffer->sample_rate == 0) {
    return 0.0f;
  }

  return (float)buffer->count /
         (float)(buffer->sample_rate * buffer->bytes_per_sample);
}

bool audio_buffer_is_buffering(const audio_buffer_t *buffer) {
  if (!buffer) {
    return true;
  }
  return buffer->is_buffering;
}

size_t audio_buffer_get_count(const audio_buffer_t *buffer) {
  if (!buffer) {
    return 0;
  }
  return buffer->count;
}
