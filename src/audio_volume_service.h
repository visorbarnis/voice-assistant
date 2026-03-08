/**
 * @file audio_volume_service.h
 * @brief Output volume control for PCM audio sent to I2S
 */

#ifndef AUDIO_VOLUME_SERVICE_H
#define AUDIO_VOLUME_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize playback volume service
 *
 * @param volume_percent Playback volume in range 0..100.
 *        Internally mapped to gain 0..100%.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t audio_volume_service_init(uint32_t volume_percent);

/**
 * @brief Set playback volume
 *
 * @param volume_percent Playback volume in range 0..100.
 *        Internally mapped to gain 0..100%.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t audio_volume_service_set(uint32_t volume_percent);

/**
 * @brief Get current playback volume
 *
 * @return Volume in percent (0..100)
 */
uint32_t audio_volume_service_get(void);

/**
 * @brief Apply current playback volume to PCM16 buffer in-place
 *
 * @param samples PCM16 samples buffer
 * @param sample_count Number of samples in buffer
 */
void audio_volume_service_apply_pcm16(int16_t *samples, size_t sample_count);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_VOLUME_SERVICE_H
