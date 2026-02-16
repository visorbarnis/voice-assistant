/**
 * @file audio_buffer.h
 * @brief Adaptive ring buffer for audio playback
 *
 * Implements buffering logic from the Go reference:
 * - Initial buffering (300ms) before playback starts
 * - Underrun handling with return to buffering mode
 * - Overflow protection (limited to 60 seconds)
 * - Thread-safe access with mutex
 */

#ifndef AUDIO_BUFFER_H
#define AUDIO_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Adaptive ring buffer structure
 */
typedef struct {
  uint8_t *data;             ///< Data buffer (in PSRAM)
  int32_t *id_buffer;        ///< Frame ID buffer (parallel, in PSRAM)
  size_t capacity;           ///< Maximum buffer size in bytes
  size_t id_capacity;        ///< Number of ID slots (capacity / frame_size)
  size_t head;               ///< Write position
  size_t tail;               ///< Read position
  size_t count;              ///< Current amount of data
  size_t id_head;            ///< ID write position
  size_t id_tail;            ///< ID read position
  size_t id_count;           ///< Number of IDs in buffer
  bool is_buffering;         ///< Buffering mode flag
  size_t start_threshold;    ///< Playback start threshold (bytes)
  size_t frame_size;         ///< Frame size in bytes (320 for 10ms @ 16kHz)
  SemaphoreHandle_t mutex;   ///< Thread-safety mutex
  int64_t last_log_time;     ///< Timestamp of last log
  uint32_t sample_rate;      ///< Sample rate for time calculations
  uint16_t bytes_per_sample; ///< Bytes per sample (usually 2 for 16-bit mono)
} audio_buffer_t;

/**
 * @brief Create and initialize buffer
 *
 * @param sample_rate Audio sample rate (e.g. 24000)
 * @param max_seconds Maximum buffer size in seconds
 * @param start_threshold_ms Buffering threshold in milliseconds
 * @return Pointer to created buffer or NULL on error
 */
audio_buffer_t *audio_buffer_create(uint32_t sample_rate, uint32_t max_seconds,
                                    uint32_t start_threshold_ms);

/**
 * @brief Destroy buffer
 *
 * @param buffer Buffer pointer
 */
void audio_buffer_destroy(audio_buffer_t *buffer);

/**
 * @brief Write data to buffer
 *
 * On overflow, old data is discarded.
 * Periodically logs buffer level information.
 *
 * @param buffer Buffer pointer
 * @param data Data to write
 * @param len Data size in bytes
 * @return Number of written bytes
 */
size_t audio_buffer_write(audio_buffer_t *buffer, const uint8_t *data,
                          size_t len);

/**
 * @brief Read data from buffer
 *
 * Implements adaptive logic:
 * - If buffer is empty or below threshold, returns silence
 * - On underrun, returns to buffering mode
 *
 * @param buffer Buffer pointer
 * @param data Output data buffer
 * @param len Requested size in bytes
 * @return Number of read bytes (always equals len)
 */
size_t audio_buffer_read(audio_buffer_t *buffer, uint8_t *data, size_t len);

/**
 * @brief Clear buffer
 *
 * Called when interrupt signal is received from server.
 *
 * @param buffer Buffer pointer
 */
void audio_buffer_clear(audio_buffer_t *buffer);

/**
 * @brief Write data to buffer with associated ID
 *
 * @param buffer Buffer pointer
 * @param data Data to write
 * @param len Data size in bytes (must equal frame_size)
 * @param id Frame ID from server
 * @return Number of written bytes
 */
size_t audio_buffer_write_with_id(audio_buffer_t *buffer, const uint8_t *data,
                                  size_t len, int32_t id);

/**
 * @brief Read data from buffer with associated ID output
 *
 * @param buffer Buffer pointer
 * @param data Output data buffer
 * @param len Requested size in bytes
 * @param out_id Pointer to write frame ID (can be NULL)
 * @return Number of read bytes
 */
size_t audio_buffer_read_with_id(audio_buffer_t *buffer, uint8_t *data,
                                 size_t len, int32_t *out_id);

/**
 * @brief Get buffer fill level in seconds
 *
 * @param buffer Buffer pointer
 * @return Fill level in seconds (float)
 */
float audio_buffer_get_level_seconds(const audio_buffer_t *buffer);

/**
 * @brief Check buffering mode
 *
 * @param buffer Buffer pointer
 * @return true if buffer is in accumulation mode
 */
bool audio_buffer_is_buffering(const audio_buffer_t *buffer);

/**
 * @brief Get amount of data in buffer
 *
 * @param buffer Buffer pointer
 * @return Number of bytes in buffer
 */
size_t audio_buffer_get_count(const audio_buffer_t *buffer);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_BUFFER_H
