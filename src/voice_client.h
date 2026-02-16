/**
 * @file voice_client.h
 * @brief WebSocket voice client
 *
 * Manages persistent WebSocket connection to the voice server:
 * - Persistent WSS channel maintenance (with auto-reconnect)
 * - Logical session via session_start/session_stop JSON commands
 * - Microphone audio upload during active session
 * - Audio reception for playback during active session
 * - Ping/Pong heartbeat in idle mode
 */

#ifndef VOICE_CLIENT_H
#define VOICE_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "audio_buffer.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback on voice session close
 *
 * Called when WebSocket connection is closed (by server or client).
 * Used to return to wake-word detection mode.
 */
typedef void (*voice_client_session_end_cb_t)(void);

/**
 * @brief Initialize voice client
 *
 * Must be called once at application startup.
 * Initializes internal state and registers callback.
 *
 * @param on_session_end Callback on session close (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t voice_client_init(voice_client_session_end_cb_t on_session_end);

/**
 * @brief Request voice session start
 *
 * Sends session_start JSON request to the server.
 * Actual audio session starts only after incoming
 * session_start command from the server.
 *
 * @return ESP_OK if request was sent
 */
esp_err_t voice_client_start(void);

/**
 * @brief Request voice session stop
 *
 * Sends session_stop JSON request to the server.
 * Persistent WebSocket connection stays open.
 *
 * @return ESP_OK on success
 */
esp_err_t voice_client_stop(void);

/**
 * @brief Check if voice session is active
 *
 * @return true if logical voice session is active
 */
bool voice_client_is_active(void);

/**
 * @brief Check persistent WebSocket connection state
 *
 * @return true if WSS connection is established
 */
bool voice_client_is_connected(void);

/**
 * @brief Send audio data to server
 *
 * Called by audio capture task.
 * Packet format: [played_id (4 bytes)][data]
 *
 * @param data PCM data (16-bit, 16kHz, mono)
 * @param len Data size in bytes
 * @param played_id Frame ID currently being played (for AEC synchronization)
 * @return ESP_OK on success
 */
esp_err_t voice_client_send_audio(const uint8_t *data, size_t len,
                                  int32_t played_id);

/**
 * @brief Get current played_id
 *
 * Returns frame ID that is actually being played right now
 * (including DMA delay).
 *
 * @return Current played_id or -1 if inactive
 */
int32_t voice_client_get_current_played_id(void);

/**
 * @brief Get playback buffer
 *
 * Returns pointer to internal adaptive buffer.
 * Used by playback task.
 *
 * @return Buffer pointer or NULL if session is inactive
 */
audio_buffer_t *voice_client_get_playback_buffer(void);

/**
 * @brief Deinitialize voice client
 *
 * Stops session and releases resources.
 *
 * @return ESP_OK on success
 */
esp_err_t voice_client_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // VOICE_CLIENT_H
