/**
 * @file ntp_sync.h
 * @brief Simple NTP time synchronization module
 */

#pragma once

#include "esp_err.h"

/**
 * @brief Initialize SNTP and wait for time synchronization
 *
 * Configures SNTP with pool.ntp.org and blocks until time is synchronized
 * (year >= 2026) or timeout occurs.
 *
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if sync failed
 */
esp_err_t ntp_sync_initialize(void);
