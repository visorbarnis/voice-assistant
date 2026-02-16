/**
 * @file led_control.h
 * @brief WS2812 RGB LED control (low priority)
 */

#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include <stdint.h>

// LED states
typedef enum {
  LED_STATE_OFF,       // Off
  LED_STATE_IDLE,      // Idle (white breathing)
  LED_STATE_LISTENING, // Listening (green)
  LED_STATE_THINKING,  // Thinking (blue spinner/blink)
  LED_STATE_SPEAKING,  // Speaking (cyan/violet pulse)
  LED_STATE_ERROR,     // Error (red)
} led_state_t;

/**
 * @brief Initialize LED control
 *
 * Creates a low-priority task for LED animation.
 *
 * @param gpio_num GPIO pin connected to WS2812 (usually 48)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_control_init(gpio_num_t gpio_num);

/**
 * @brief Set LED state
 *
 * Thread-safe function. Updates state
 * consumed by the animation task.
 *
 * @param state New state
 */
void led_control_set_state(led_state_t state);

/**
 * @brief Get current LED state
 */
led_state_t led_control_get_state(void);
