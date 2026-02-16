/**
 * @file ntp_sync.c
 * @brief Simple NTP time synchronization implementation
 */

#include "ntp_sync.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "NTP_SYNC";

esp_err_t ntp_sync_initialize(void) {
  ESP_LOGI(TAG, "Initializing SNTP");

  // Configure SNTP
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  esp_netif_sntp_init(&config);

  // Wait for time to be set
  time_t now = 0;
  struct tm timeinfo = {0};
  int retry = 0;
  const int retry_count = 15; // Wait up to 30 seconds (15 * 2s)

  while (timeinfo.tm_year < (2026 - 1900) && ++retry < retry_count) {
    ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry,
             retry_count);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    time(&now);
    localtime_r(&now, &timeinfo);
  }

  if (timeinfo.tm_year < (2026 - 1900)) {
    ESP_LOGE(TAG, "Time sync failed!");
    return ESP_ERR_TIMEOUT;
  }

  char strftime_buf[64];
  strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
  ESP_LOGI(TAG, "Time synchronized: %s", strftime_buf);

  return ESP_OK;
}
