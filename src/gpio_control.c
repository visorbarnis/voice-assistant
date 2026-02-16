/**
 * @file gpio_control.c
 * @brief GPIO pin control module for backend commands
 */

#include "gpio_control.h"

#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "GPIO_CTRL";

// Bitmask of initialized pins (GPIO 0-63)
static uint64_t s_initialized_pins = 0;

esp_err_t gpio_control_init(void) {
  ESP_LOGI(TAG, "GPIO control module initialized");
  s_initialized_pins = 0;
  return ESP_OK;
}

esp_err_t gpio_control_set_pin(int pin, int value) {
  // Validate pin number
  if (pin < 0 || pin >= GPIO_NUM_MAX) {
    ESP_LOGE(TAG, "Invalid pin number: %d", pin);
    return ESP_ERR_INVALID_ARG;
  }

  // Initialize pin on first use
  uint64_t pin_mask = (1ULL << pin);
  if (!(s_initialized_pins & pin_mask)) {
    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to configure pin %d: %s", pin,
               esp_err_to_name(err));
      return err;
    }

    s_initialized_pins |= pin_mask;
    ESP_LOGI(TAG, "Pin %d configured as output", pin);
  }

  // Set state
  esp_err_t err = gpio_set_level((gpio_num_t)pin, value ? 1 : 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set pin %d: %s", pin, esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "Set pin %d = %d", pin, value ? 1 : 0);
  return ESP_OK;
}

esp_err_t gpio_control_handle_command(const char *json_str, size_t json_len) {
  if (!json_str || json_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  cJSON *root = cJSON_ParseWithLength(json_str, json_len);
  if (!root) {
    ESP_LOGW(TAG, "JSON parsing error");
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t result = ESP_ERR_INVALID_ARG;

  // Check type == "gpio_command"
  cJSON *type = cJSON_GetObjectItem(root, "type");
  if (!cJSON_IsString(type) || strcmp(type->valuestring, "gpio_command") != 0) {
    goto cleanup;
  }

  // Get args
  cJSON *args = cJSON_GetObjectItem(root, "args");
  if (!cJSON_IsObject(args)) {
    ESP_LOGW(TAG, "Missing args object");
    goto cleanup;
  }

  // Extract pin and value
  cJSON *pin_item = cJSON_GetObjectItem(args, "pin");
  cJSON *value_item = cJSON_GetObjectItem(args, "value");

  // Debug: print args contents
  char *args_str = cJSON_PrintUnformatted(args);
  if (args_str) {
    ESP_LOGI(TAG, "args = %s", args_str);
    free(args_str);
  }

  if (!cJSON_IsNumber(pin_item) || !cJSON_IsNumber(value_item)) {
    ESP_LOGW(TAG, "Invalid pin or value format (pin_type=%d, value_type=%d)",
             pin_item ? pin_item->type : -1,
             value_item ? value_item->type : -1);
    goto cleanup;
  }

  int pin = pin_item->valueint;
  int value = value_item->valueint;

  // Log functionName for debugging
  cJSON *func_name = cJSON_GetObjectItem(root, "functionName");
  if (cJSON_IsString(func_name)) {
    ESP_LOGI(TAG, "Executing function: %s", func_name->valuestring);
  }

  result = gpio_control_set_pin(pin, value);

cleanup:
  cJSON_Delete(root);
  return result;
}
