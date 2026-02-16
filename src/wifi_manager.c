/**
 * @file wifi_manager.c
 * @brief WiFi Manager - WiFi connection management implementation
 *
 * Uses ESP-IDF WiFi API to connect to an access point.
 * Configuration is loaded from runtime NVS settings with
 * fallback to menuconfig (Kconfig) defaults.
 */

#include "wifi_manager.h"
#include "runtime_config.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "WIFI_MGR";

// ----------------------------------------------------------------------------
// Event group bits
// ----------------------------------------------------------------------------
#define WIFI_CONNECTED_BIT BIT0 // Successful connection
#define WIFI_FAIL_BIT BIT1      // Connection error

// ----------------------------------------------------------------------------
// Global variables
// ----------------------------------------------------------------------------
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static int s_retry_num = 0;
static uint32_t s_max_retry = 0;
static const runtime_config_t *s_cfg = NULL;
static bool s_is_connected = false;
static bool s_is_initialized = false;

// ----------------------------------------------------------------------------
// Event Handler
// ----------------------------------------------------------------------------

/**
 * @brief WiFi and IP event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      ESP_LOGI(TAG, "WiFi STA started, connecting...");
      esp_wifi_connect();
      break;

    case WIFI_EVENT_STA_DISCONNECTED: {
      wifi_event_sta_disconnected_t *event =
          (wifi_event_sta_disconnected_t *)event_data;
      ESP_LOGW(TAG, "Disconnected from WiFi, reason: %d", event->reason);
      s_is_connected = false;

      if (s_retry_num < (int)s_max_retry) {
        s_retry_num++;
        ESP_LOGI(TAG, "Reconnection attempt %d/%lu...", s_retry_num,
                 (unsigned long)s_max_retry);
        esp_wifi_connect();
      } else {
        ESP_LOGE(TAG, "Maximum connection retry attempts exceeded");
        if (s_wifi_event_group) {
          xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
      }
      break;
    }

    case WIFI_EVENT_STA_CONNECTED:
      ESP_LOGI(TAG, "Connected to WiFi, waiting for IP...");
      break;

    default:
      break;
    }
  } else if (event_base == IP_EVENT) {
    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
      ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
      ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
      s_retry_num = 0;
      s_is_connected = true;
      if (s_wifi_event_group) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      }
      break;
    }

    case IP_EVENT_STA_LOST_IP:
      ESP_LOGW(TAG, "IP address lost");
      s_is_connected = false;
      break;

    default:
      break;
    }
  }
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

esp_err_t wifi_manager_init(void) {
  if (s_is_initialized) {
    ESP_LOGW(TAG, "WiFi already initialized");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Initializing WiFi...");

  esp_err_t cfg_err = runtime_config_init();
  if (cfg_err != ESP_OK) {
    ESP_LOGW(TAG, "runtime_config_init failed: %s (using defaults)",
             esp_err_to_name(cfg_err));
  }
  s_cfg = runtime_config_get();
  s_max_retry = s_cfg->wifi_max_retry;

  // Create event group for synchronization
  s_wifi_event_group = xEventGroupCreate();
  if (!s_wifi_event_group) {
    ESP_LOGE(TAG, "Failed to create event group");
    return ESP_ERR_NO_MEM;
  }

  // Initialize network stack
  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");

  // Create default event loop (might already exist)
  esp_err_t ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
    return ret;
  }

  // Create default STA netif
  s_sta_netif = esp_netif_create_default_wifi_sta();
  if (!s_sta_netif) {
    ESP_LOGE(TAG, "Failed to create netif");
    return ESP_FAIL;
  }

  // Initialize WiFi with default configuration
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

  // Register event handlers
  ESP_RETURN_ON_ERROR(
      esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          &wifi_event_handler, NULL, NULL),
      TAG, "Failed to register WIFI_EVENT handler");

  ESP_RETURN_ON_ERROR(
      esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                          &wifi_event_handler, NULL, NULL),
      TAG, "Failed to register IP_EVENT handler");

  // Configure WiFi
  wifi_config_t wifi_config = {
      .sta =
          {
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .pmf_cfg =
                  {
                      .capable = true,
                      .required = false,
                  },
          },
  };

  strlcpy((char *)wifi_config.sta.ssid, s_cfg->wifi_ssid,
          sizeof(wifi_config.sta.ssid));
  strlcpy((char *)wifi_config.sta.password, s_cfg->wifi_password,
          sizeof(wifi_config.sta.password));

  // If password is empty, use an open network
  if (strlen(s_cfg->wifi_password) == 0) {
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                      "esp_wifi_set_mode failed");

  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                      "esp_wifi_set_config failed");

  s_is_initialized = true;
  ESP_LOGI(TAG, "WiFi initialized, SSID: %s", s_cfg->wifi_ssid);

  return ESP_OK;
}

esp_err_t wifi_manager_connect(void) {
  if (!s_is_initialized) {
    ESP_LOGE(TAG, "WiFi is not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (s_is_connected) {
    ESP_LOGI(TAG, "Already connected to WiFi");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Connecting to WiFi: %s", s_cfg->wifi_ssid);

  // Reset retry counter
  s_retry_num = 0;

  // Clear event group bits
  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

  // Start WiFi
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

  // Wait for connection
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Successfully connected to WiFi");
    return ESP_OK;
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "Failed to connect to WiFi");
    return ESP_FAIL;
  }

  ESP_LOGE(TAG, "Unexpected connection error");
  return ESP_FAIL;
}

esp_err_t wifi_manager_disconnect(void) {
  if (!s_is_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "Disconnecting from WiFi...");
  esp_wifi_disconnect();
  esp_wifi_stop();
  s_is_connected = false;

  return ESP_OK;
}

bool wifi_manager_is_connected(void) { return s_is_connected; }

esp_err_t wifi_manager_deinit(void) {
  if (!s_is_initialized) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Deinitializing WiFi...");

  wifi_manager_disconnect();
  esp_wifi_deinit();

  if (s_sta_netif) {
    esp_netif_destroy(s_sta_netif);
    s_sta_netif = NULL;
  }

  if (s_wifi_event_group) {
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;
  }

  s_is_initialized = false;
  return ESP_OK;
}
