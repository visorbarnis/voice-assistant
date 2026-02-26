/**
 * @file runtime_config.c
 * @brief Runtime configuration loader (NVS + sdkconfig fallback)
 */

#include "runtime_config.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "RUNTIME_CFG";

// NVS namespaces (must match settings.csv)
#define NS_WIFI "wifi_settings"
#define NS_SERVER "server_settings"
#define NS_AUDIO "audio_settings"

// NVS keys (max 15 chars each)
#define KEY_WIFI_SSID "ssid"
#define KEY_WIFI_PASSWORD "password"
#define KEY_WIFI_MAX_RETRY "max_retry"

#define KEY_SERVER_HOST "host"
#define KEY_SERVER_PORT "port"
#define KEY_SERVER_LOCATION "location"
#define KEY_SERVER_PARAMETER "parameter"
#define KEY_SERVER_WS_PATH "ws_path"
#define KEY_SERVER_API_KEY "api_key"
#define KEY_SERVER_MODE "client_mode"
#define KEY_SERVER_SPEAK_MODE "speak_mode"
#define KEY_SERVER_WAKE_MODE "wake_mode"
#define KEY_SERVER_WAKE_LEVEL "wake_level"

#define KEY_AUDIO_PLAYBACK_RATE "playback_rate"
#define KEY_AUDIO_VOLUME_PERCENT "volume_pct"
#define KEY_AUDIO_START_MS "buffer_start_ms"
#define KEY_AUDIO_MAX_SECONDS "buffer_max_s"

#define DEFAULT_WAKE_DETECTION_MODE "strict"
#define DEFAULT_WAKE_SENSITIVITY_LEVEL 6U
#define DEFAULT_AUDIO_PLAYBACK_VOLUME_PERCENT 100U

static runtime_config_t s_config = {0};
static bool s_defaults_loaded = false;
static bool s_nvs_loaded = false;

static void load_defaults(void) {
  memset(&s_config, 0, sizeof(s_config));

  strlcpy(s_config.wifi_ssid, CONFIG_WIFI_SSID, sizeof(s_config.wifi_ssid));
  strlcpy(s_config.wifi_password, CONFIG_WIFI_PASSWORD,
          sizeof(s_config.wifi_password));
  s_config.wifi_max_retry = CONFIG_WIFI_MAXIMUM_RETRY;

  strlcpy(s_config.server_host, CONFIG_VOICE_SERVER_HOST,
          sizeof(s_config.server_host));
  s_config.server_port = CONFIG_VOICE_SERVER_PORT;
  s_config.server_location[0] = '\0';
  s_config.server_parameter[0] = '\0';
  strlcpy(s_config.voice_ws_path, CONFIG_VOICE_WS_PATH,
          sizeof(s_config.voice_ws_path));
  strlcpy(s_config.voice_api_key, CONFIG_VOICE_API_KEY,
          sizeof(s_config.voice_api_key));
  strlcpy(s_config.voice_client_mode, CONFIG_VOICE_CLIENT_MODE,
          sizeof(s_config.voice_client_mode));
  strlcpy(s_config.speak_mode, CONFIG_SPEAK_MODE, sizeof(s_config.speak_mode));
  strlcpy(s_config.wake_detection_mode, DEFAULT_WAKE_DETECTION_MODE,
          sizeof(s_config.wake_detection_mode));
  s_config.wake_sensitivity_level = DEFAULT_WAKE_SENSITIVITY_LEVEL;

  s_config.audio_playback_sample_rate = CONFIG_AUDIO_PLAYBACK_SAMPLE_RATE;
  s_config.audio_playback_volume_percent = DEFAULT_AUDIO_PLAYBACK_VOLUME_PERCENT;
  s_config.audio_buffer_start_threshold_ms =
      CONFIG_AUDIO_BUFFER_START_THRESHOLD_MS;
  s_config.audio_buffer_max_seconds = CONFIG_AUDIO_BUFFER_MAX_SECONDS;

  s_defaults_loaded = true;
}

static void ensure_defaults_loaded(void) {
  if (!s_defaults_loaded) {
    load_defaults();
  }
}

static void sanitize_loaded_config(void) {
  if (s_config.wifi_max_retry == 0 || s_config.wifi_max_retry > 100) {
    s_config.wifi_max_retry = CONFIG_WIFI_MAXIMUM_RETRY;
  }

  if (s_config.server_port == 0 || s_config.server_port > 65535) {
    s_config.server_port = CONFIG_VOICE_SERVER_PORT;
  }

  if (s_config.voice_ws_path[0] == '\0') {
    strlcpy(s_config.voice_ws_path, CONFIG_VOICE_WS_PATH,
            sizeof(s_config.voice_ws_path));
  }

  if (s_config.voice_ws_path[0] != '/') {
    size_t path_len = strnlen(s_config.voice_ws_path,
                              sizeof(s_config.voice_ws_path) - 1);
    if (path_len > sizeof(s_config.voice_ws_path) - 2) {
      path_len = sizeof(s_config.voice_ws_path) - 2;
    }
    memmove(s_config.voice_ws_path + 1, s_config.voice_ws_path, path_len + 1);
    s_config.voice_ws_path[0] = '/';
  }

  if (s_config.audio_playback_sample_rate == 0) {
    s_config.audio_playback_sample_rate = CONFIG_AUDIO_PLAYBACK_SAMPLE_RATE;
  }

  if (s_config.audio_playback_volume_percent > 100) {
    s_config.audio_playback_volume_percent =
        DEFAULT_AUDIO_PLAYBACK_VOLUME_PERCENT;
  }

  if (s_config.audio_buffer_start_threshold_ms == 0 ||
      s_config.audio_buffer_start_threshold_ms > 5000) {
    s_config.audio_buffer_start_threshold_ms =
        CONFIG_AUDIO_BUFFER_START_THRESHOLD_MS;
  }

  if (s_config.audio_buffer_max_seconds == 0 ||
      s_config.audio_buffer_max_seconds > 120) {
    s_config.audio_buffer_max_seconds = CONFIG_AUDIO_BUFFER_MAX_SECONDS;
  }

  if (s_config.server_host[0] == '\0') {
    strlcpy(s_config.server_host, CONFIG_VOICE_SERVER_HOST,
            sizeof(s_config.server_host));
  }

  if (s_config.voice_client_mode[0] == '\0') {
    strlcpy(s_config.voice_client_mode, CONFIG_VOICE_CLIENT_MODE,
            sizeof(s_config.voice_client_mode));
  }

  if (s_config.speak_mode[0] == '\0') {
    strlcpy(s_config.speak_mode, CONFIG_SPEAK_MODE, sizeof(s_config.speak_mode));
  }

  if (s_config.wake_detection_mode[0] == '\0' ||
      (strcmp(s_config.wake_detection_mode, "normal") != 0 &&
       strcmp(s_config.wake_detection_mode, "strict") != 0)) {
    strlcpy(s_config.wake_detection_mode, DEFAULT_WAKE_DETECTION_MODE,
            sizeof(s_config.wake_detection_mode));
  }

  if (s_config.wake_sensitivity_level > 10) {
    s_config.wake_sensitivity_level = DEFAULT_WAKE_SENSITIVITY_LEVEL;
  }
}

static bool nvs_get_str_if_present(nvs_handle_t handle, const char *key,
                                   char *out, size_t out_size,
                                   const char *namespace_name) {
  size_t required_size = out_size;
  esp_err_t err = nvs_get_str(handle, key, out, &required_size);
  if (err == ESP_OK) {
    return true;
  }

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }

  if (err == ESP_ERR_NVS_INVALID_LENGTH) {
    ESP_LOGW(TAG, "NVS key '%s/%s' too long (%u bytes), keeping default",
             namespace_name, key, (unsigned)required_size);
    return false;
  }

  ESP_LOGW(TAG, "Failed to read NVS key '%s/%s': %s", namespace_name, key,
           esp_err_to_name(err));
  return false;
}

static bool nvs_get_u32_if_present(nvs_handle_t handle, const char *key,
                                   uint32_t *out,
                                   const char *namespace_name) {
  esp_err_t err = nvs_get_u32(handle, key, out);
  if (err == ESP_OK) {
    return true;
  }

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }

  ESP_LOGW(TAG, "Failed to read NVS key '%s/%s': %s", namespace_name, key,
           esp_err_to_name(err));
  return false;
}

static esp_err_t load_wifi_overrides(void) {
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(NS_WIFI, NVS_READONLY, &handle);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "NVS namespace '%s' not found, using defaults", NS_WIFI);
    return ESP_OK;
  }

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Cannot open NVS namespace '%s': %s", NS_WIFI,
             esp_err_to_name(err));
    return err;
  }

  nvs_get_str_if_present(handle, KEY_WIFI_SSID, s_config.wifi_ssid,
                         sizeof(s_config.wifi_ssid), NS_WIFI);
  nvs_get_str_if_present(handle, KEY_WIFI_PASSWORD, s_config.wifi_password,
                         sizeof(s_config.wifi_password), NS_WIFI);
  nvs_get_u32_if_present(handle, KEY_WIFI_MAX_RETRY, &s_config.wifi_max_retry,
                         NS_WIFI);

  nvs_close(handle);
  return ESP_OK;
}

static esp_err_t load_server_overrides(void) {
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(NS_SERVER, NVS_READONLY, &handle);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "NVS namespace '%s' not found, using defaults", NS_SERVER);
    return ESP_OK;
  }

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Cannot open NVS namespace '%s': %s", NS_SERVER,
             esp_err_to_name(err));
    return err;
  }

  nvs_get_str_if_present(handle, KEY_SERVER_HOST, s_config.server_host,
                         sizeof(s_config.server_host), NS_SERVER);
  nvs_get_u32_if_present(handle, KEY_SERVER_PORT, &s_config.server_port,
                         NS_SERVER);
  nvs_get_str_if_present(handle, KEY_SERVER_LOCATION, s_config.server_location,
                         sizeof(s_config.server_location), NS_SERVER);
  nvs_get_str_if_present(handle, KEY_SERVER_PARAMETER, s_config.server_parameter,
                         sizeof(s_config.server_parameter), NS_SERVER);
  nvs_get_str_if_present(handle, KEY_SERVER_WS_PATH, s_config.voice_ws_path,
                         sizeof(s_config.voice_ws_path), NS_SERVER);
  nvs_get_str_if_present(handle, KEY_SERVER_API_KEY, s_config.voice_api_key,
                         sizeof(s_config.voice_api_key), NS_SERVER);
  nvs_get_str_if_present(handle, KEY_SERVER_MODE, s_config.voice_client_mode,
                         sizeof(s_config.voice_client_mode), NS_SERVER);
  nvs_get_str_if_present(handle, KEY_SERVER_SPEAK_MODE, s_config.speak_mode,
                         sizeof(s_config.speak_mode), NS_SERVER);
  nvs_get_str_if_present(handle, KEY_SERVER_WAKE_MODE, s_config.wake_detection_mode,
                         sizeof(s_config.wake_detection_mode), NS_SERVER);
  nvs_get_u32_if_present(handle, KEY_SERVER_WAKE_LEVEL,
                         &s_config.wake_sensitivity_level, NS_SERVER);

  nvs_close(handle);
  return ESP_OK;
}

static esp_err_t load_audio_overrides(void) {
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(NS_AUDIO, NVS_READONLY, &handle);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "NVS namespace '%s' not found, using defaults", NS_AUDIO);
    return ESP_OK;
  }

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Cannot open NVS namespace '%s': %s", NS_AUDIO,
             esp_err_to_name(err));
    return err;
  }

  nvs_get_u32_if_present(handle, KEY_AUDIO_PLAYBACK_RATE,
                         &s_config.audio_playback_sample_rate, NS_AUDIO);
  nvs_get_u32_if_present(handle, KEY_AUDIO_VOLUME_PERCENT,
                         &s_config.audio_playback_volume_percent, NS_AUDIO);
  nvs_get_u32_if_present(handle, KEY_AUDIO_START_MS,
                         &s_config.audio_buffer_start_threshold_ms, NS_AUDIO);
  nvs_get_u32_if_present(handle, KEY_AUDIO_MAX_SECONDS,
                         &s_config.audio_buffer_max_seconds, NS_AUDIO);

  nvs_close(handle);
  return ESP_OK;
}

esp_err_t runtime_config_init(void) {
  ensure_defaults_loaded();

  if (s_nvs_loaded) {
    return ESP_OK;
  }

  esp_err_t err_wifi = load_wifi_overrides();
  esp_err_t err_server = load_server_overrides();
  esp_err_t err_audio = load_audio_overrides();

  if (err_wifi == ESP_ERR_NVS_NOT_INITIALIZED ||
      err_server == ESP_ERR_NVS_NOT_INITIALIZED ||
      err_audio == ESP_ERR_NVS_NOT_INITIALIZED) {
    ESP_LOGE(TAG, "NVS is not initialized, runtime overrides are unavailable");
    return ESP_ERR_NVS_NOT_INITIALIZED;
  }

  sanitize_loaded_config();
  s_nvs_loaded = true;

  runtime_config_log_summary();
  return ESP_OK;
}

const runtime_config_t *runtime_config_get(void) {
  ensure_defaults_loaded();
  return &s_config;
}

void runtime_config_log_summary(void) {
  const runtime_config_t *cfg = runtime_config_get();

  ESP_LOGI(TAG, "Active config: WiFi SSID='%s', retry=%" PRIu32,
           cfg->wifi_ssid, cfg->wifi_max_retry);
  ESP_LOGI(TAG, "Active config: Voice endpoint=wss://%s:%" PRIu32 "%s",
           cfg->server_host, cfg->server_port, cfg->voice_ws_path);
  ESP_LOGI(TAG, "Active config: location='%s', parameter='%s'",
           cfg->server_location, cfg->server_parameter);
  ESP_LOGI(TAG,
           "Active config: mode=%s, speak_mode=%s, wake_mode=%s, wake_level=%" PRIu32
           ", api_key_len=%u",
           cfg->voice_client_mode, cfg->speak_mode,
           cfg->wake_detection_mode,
           cfg->wake_sensitivity_level,
           (unsigned)strlen(cfg->voice_api_key));
  ESP_LOGI(TAG,
           "Active config: playback_rate=%" PRIu32
           ", volume_pct=%" PRIu32 ", buffer_start_ms=%" PRIu32
           ", buffer_max_s=%" PRIu32,
           cfg->audio_playback_sample_rate, cfg->audio_playback_volume_percent,
           cfg->audio_buffer_start_threshold_ms,
           cfg->audio_buffer_max_seconds);
}
