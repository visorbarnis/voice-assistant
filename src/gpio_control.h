/**
 * @file gpio_control.h
 * @brief GPIO pin control module for backend commands
 *
 * Processes JSON commands of type gpio_command and sets
 * the corresponding GPIO pin states.
 */

#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the GPIO control module
 *
 * @return ESP_OK on success
 */
esp_err_t gpio_control_init(void);

/**
 * @brief Set GPIO pin state
 *
 * On first use, the pin is automatically configured as output.
 *
 * @param pin GPIO pin number
 * @param value State (0 = LOW, 1 = HIGH)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid pin number
 */
esp_err_t gpio_control_set_pin(int pin, int value);

/**
 * @brief Handle gpio_command JSON command
 *
 * Parses JSON and calls gpio_control_set_pin with extracted parameters.
 *
 * @param json_str JSON string (null-terminated)
 * @param json_len String length
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on parsing error
 */
esp_err_t gpio_control_handle_command(const char *json_str, size_t json_len);

#ifdef __cplusplus
}
#endif

#endif // GPIO_CONTROL_H
