/**
 * @file afe_pipeline.h
 * @brief Unified AFE pipeline: AEC + WakeNet + VAD
 */

#ifndef AFE_PIPELINE_H
#define AFE_PIPELINE_H

#include "esp_afe_sr_models.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize AFE pipeline
 *
 * @param models Model list from the "model" partition
 * @return esp_err_t
 */
esp_err_t afe_pipeline_init(srmodel_list_t *models);

/**
 * @brief Start AFE pipeline task
 */
esp_err_t afe_pipeline_start(void);

/**
 * @brief Feed reference signal (speaker loopback)
 *
 * @param data PCM 16-bit, mono
 * @param len Size in bytes
 */
void afe_pipeline_feed_reference(const int16_t *data, size_t len);

/**
 * @brief Reset AFE and reference buffers
 */
void afe_pipeline_reset(void);

/**
 * @brief Notify voice session start (called from voice_client)
 */
void afe_pipeline_notify_session_start(void);

/**
 * @brief Notify voice session end (called from voice_client)
 */
void afe_pipeline_notify_session_end(void);

/**
 * @brief Manually activate session (for example, by button)
 *
 * Emulates wake-word detection.
 */
void afe_pipeline_manual_wakeup(void);

/**
 * @brief Callback types
 */
typedef bool (*afe_wake_word_detected_cb_t)(const char *model_name,
                                            float volume_db);
typedef void (*afe_session_end_cb_t)(void);

/**
 * @brief Register Callbacks
 */
void afe_pipeline_register_callbacks(afe_wake_word_detected_cb_t on_wake,
                                     afe_session_end_cb_t on_end);

#ifdef __cplusplus
}
#endif

#endif // AFE_PIPELINE_H
