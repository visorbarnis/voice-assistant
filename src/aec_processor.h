/**
 * @file aec_processor.h
 * @brief Acoustic Echo Cancellation (AEC) module based on ESP-AFE
 *
 * Provides an interface to process microphone signal and remove
 * speaker echo. Uses ESP32-S3 hardware acceleration.
 */

#ifndef AEC_PROCESSOR_H
#define AEC_PROCESSOR_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Default configuration
#define AEC_SAMPLE_RATE 16000
#define AEC_CHANNELS_MIC 1
#define AEC_CHANNELS_REF 1

/**
 * @brief Initialize AEC processor
 *
 * Configures ESP-AFE to work in AEC (Echo Cancellation) mode.
 *
 * @param sample_rate Sampling rate (usually 16000)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t aec_processor_init(int sample_rate);

/**
 * @brief Get required chunk size (in samples)
 *
 * AFE requires input in strictly defined chunk sizes.
 * Use this value when reading from I2S.
 *
 * @return int Number of samples (e.g. 512) or 0 if not initialized
 */
int aec_processor_get_chunk_size(void);

/**
 * @brief Get AFE output chunk size (in samples)
 *
 * Output frame size may differ from input size.
 *
 * @return int Number of samples or 0 if not initialized
 */
int aec_processor_get_fetch_chunk_size(void);

/**
 * @brief Release AEC resources
 */
void aec_processor_deinit(void);

/**
 * @brief Feed reference signal (audio sent to speaker)
 *
 * This data is stored in an internal buffer and used
 * to subtract echo from microphone signal.
 *
 * @param data PCM data (16-bit)
 * @param len Data size in bytes
 */
void aec_processor_feed_reference(const int16_t *data, size_t len);

/**
 * @brief Process microphone signal
 *
 * Accepts raw microphone signal, synchronizes it with reference,
 * feeds it to AFE, and returns cleaned output signal.
 *
 * @param mic_data Input microphone data (16-bit)
 * @param len Data size in bytes. On return, contains output data size.
 * @param out_data Buffer to write cleaned signal
 * @return esp_err_t ESP_OK on success
 */
esp_err_t aec_processor_process(const int16_t *mic_data, size_t len,
                                int16_t *out_data, size_t *out_len);

/**
 * @brief Reset internal AEC buffers
 */
void aec_processor_reset(void);

#ifdef __cplusplus
}
#endif

#endif // AEC_PROCESSOR_H
