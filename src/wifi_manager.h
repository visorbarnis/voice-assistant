/**
 * @file wifi_manager.h
 * @brief WiFi Manager - WiFi connection management
 *
 * Provides functions for initializing, connecting, and monitoring
 * WiFi connection state on ESP32-S3.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi subsystem
 *
 * Initializes NVS, netif, event loop, and WiFi in STA mode.
 * Should be called once at application startup.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Connect to WiFi access point
 *
 * Uses SSID/password from runtime configuration (NVS), with
 * compile-time fallback to menuconfig defaults.
 * Blocks until connected successfully or until the maximum
 * number of retry attempts is exceeded.
 *
 * @return ESP_OK on successful connection, ESP_FAIL on error
 */
esp_err_t wifi_manager_connect(void);

/**
 * @brief Disconnect from WiFi
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Check connection state
 *
 * @return true if connected and IP address acquired
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Deinitialize WiFi
 *
 * Stops WiFi and releases resources.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
