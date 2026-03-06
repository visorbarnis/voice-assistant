/**
 * @file runtime_config.h
 * @brief Runtime configuration loaded from NVS with compile-time fallback
 *
 * Configuration source priority:
 * 1) NVS namespaces (written from settings.bin)
 * 2) sdkconfig (CONFIG_*) defaults
 */

#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RUNTIME_CONFIG_MAX_SSID_LEN 32
#define RUNTIME_CONFIG_MAX_WIFI_PASSWORD_LEN 64
#define RUNTIME_CONFIG_MAX_HOST_LEN 95
#define RUNTIME_CONFIG_MAX_WS_PATH_LEN 63
#define RUNTIME_CONFIG_MAX_API_KEY_LEN 127
#define RUNTIME_CONFIG_MAX_LOCATION_LEN 63
#define RUNTIME_CONFIG_MAX_PARAMETER_LEN 63
#define RUNTIME_CONFIG_MAX_MODE_LEN 31

/**
 * @brief Effective runtime configuration used by the firmware
 */
typedef struct {
  char wifi_ssid[RUNTIME_CONFIG_MAX_SSID_LEN + 1];
  char wifi_password[RUNTIME_CONFIG_MAX_WIFI_PASSWORD_LEN + 1];
  uint32_t wifi_max_retry;

  char server_host[RUNTIME_CONFIG_MAX_HOST_LEN + 1];
  uint32_t server_port;
  char server_location[RUNTIME_CONFIG_MAX_LOCATION_LEN + 1];
  char server_parameter[RUNTIME_CONFIG_MAX_PARAMETER_LEN + 1];
  char voice_ws_path[RUNTIME_CONFIG_MAX_WS_PATH_LEN + 1];
  char voice_api_key[RUNTIME_CONFIG_MAX_API_KEY_LEN + 1];
  char voice_client_mode[RUNTIME_CONFIG_MAX_MODE_LEN + 1];
  char speak_mode[RUNTIME_CONFIG_MAX_MODE_LEN + 1];
  char wake_detection_mode[RUNTIME_CONFIG_MAX_MODE_LEN + 1]; // normal|aggressive|strict(legacy)
  uint32_t wake_sensitivity_level; // 0..10, where 10 is most sensitive

  uint32_t audio_playback_sample_rate;
  uint32_t audio_playback_volume_percent;
  uint32_t audio_buffer_start_threshold_ms;
  uint32_t audio_buffer_max_seconds;
} runtime_config_t;

/**
 * @brief Load configuration from NVS over sdkconfig defaults
 *
 * Requires NVS to be initialized before call.
 *
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_INITIALIZED if NVS is not ready
 */
esp_err_t runtime_config_init(void);

/**
 * @brief Get current effective config
 *
 * If runtime_config_init() was not called yet, returns sdkconfig defaults.
 *
 * @return Pointer to internal immutable configuration
 */
const runtime_config_t *runtime_config_get(void);

/**
 * @brief Log active runtime configuration (sensitive values masked)
 */
void runtime_config_log_summary(void);

/**
 * @brief Set playback volume and persist it in NVS
 *
 * @param volume_percent Playback volume in range 0..100
 * @return ESP_OK on success
 */
esp_err_t runtime_config_set_audio_playback_volume_percent(
    uint32_t volume_percent);

#ifdef __cplusplus
}
#endif

#endif // RUNTIME_CONFIG_H
