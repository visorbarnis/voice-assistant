/**
 * @file led_control.c
 * @brief WS2812 control implementation via RMT
 */

#include "led_control.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include <math.h>

static const char *TAG = "LED_CTRL";
static led_strip_handle_t s_led_strip = NULL;
static volatile led_state_t s_current_state = LED_STATE_OFF;
static TaskHandle_t s_led_task_handle = NULL;

// Animation variables
static uint32_t s_tick = 0;

/**
 * @brief Helper function to set color (RGB)
 *
 * Does nothing if LED is not initialized.
 */
static void set_color(uint8_t r, uint8_t g, uint8_t b) {
  if (s_led_strip) {
    led_strip_set_pixel(s_led_strip, 0, r, g, b);
    led_strip_refresh(s_led_strip);
  }
}

/**
 * @brief LED animation task
 *
 * Runs at low priority. If system is overloaded,
 * animation may lag, which is acceptable.
 */
static void led_anim_task(void *arg) {
  ESP_LOGI(TAG, "LED animation task started");

  while (1) {
    led_state_t state = s_current_state;
    s_tick++;

    switch (state) {
    case LED_STATE_OFF:
      set_color(0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(100)); // Infrequent updates
      break;

    case LED_STATE_IDLE: {
      // White "breathing" effect
      // Use sine wave for smoothness
      float val = (sinf(s_tick * 0.1f) + 1.0f) * 0.5f; // 0.0 .. 1.0
      uint8_t brightness = (uint8_t)(val * 50.0f); // Max brightness 50 (was 20)
      if (brightness < 5)
        brightness = 5; // Minimum threshold
      set_color(brightness, brightness, brightness);
      vTaskDelay(pdMS_TO_TICKS(50));
      break;
    }

    case LED_STATE_LISTENING:
      // Static green (high brightness)
      set_color(0, 150, 0); // Was 50
      vTaskDelay(pdMS_TO_TICKS(100));
      break;

    case LED_STATE_THINKING: {
      // Blue spinning/blinking
      // Simple blink 0..50..0
      float val = (sinf(s_tick * 0.3f) + 1.0f) * 0.5f;
      uint8_t b = (uint8_t)(val * 150.0f); // Was 60
      set_color(0, 0, b);
      vTaskDelay(pdMS_TO_TICKS(30));
      break;
    }

    case LED_STATE_SPEAKING: {
      // Iridescent bright green effect
      // s_tick increments every cycle (roughly every 30-50ms, depends
      // on delay)

      // 1. Main brightness pulse (fast)
      float bright_pulse = (sinf(s_tick * 0.4f) + 1.0f) * 0.5f; // 0..1

      // Hue shift (Lime <-> Green <-> Teal tint)
      float hue_shift = (sinf(s_tick * 0.2f) + 1.0f) * 0.5f; // 0..1

      // High base brightness: 100..255
      // Green channel dominates
      uint8_t g = (uint8_t)(100 + bright_pulse * 155); // 100..255 (Very bright!)

      // Red adds yellowness (Lime)
      uint8_t r = (uint8_t)(hue_shift * 80); // 0..80

      // Blue adds turquoise (Teal)
      uint8_t b = (uint8_t)((1.0f - hue_shift) * 60); // 60..0

      set_color(r, g, b);
      vTaskDelay(pdMS_TO_TICKS(30)); // Fast updates for smoothness
      break;
    }

    case LED_STATE_ERROR:
      // Red blinking
      if ((s_tick % 20) < 10) {
        set_color(200, 0, 0); // Was 50
      } else {
        set_color(0, 0, 0);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      break;

    default:
      set_color(0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(200));
      break;
    }
  }
}

esp_err_t led_control_init(gpio_num_t gpio_num) {
  ESP_LOGI(TAG, "Initializing LED control on GPIO %d", gpio_num);

  led_strip_config_t strip_config = {
      .strip_gpio_num = gpio_num,
      .max_leds = 1,
  };

  led_strip_rmt_config_t rmt_config = {
      .resolution_hz = 10 * 1000 * 1000, // 10MHz
      .flags.with_dma = false,           // DMA is not required for one LED
  };

  ESP_ERROR_CHECK(
      led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip));
  led_strip_clear(s_led_strip);

  // Create task with priority just above IDLE (lowest practical)
  // tskIDLE_PRIORITY = 0.
  // This ensures LED handling does not interfere with audio
  // (audio tasks are typically priority 5-10+).
  BaseType_t ret = xTaskCreate(led_anim_task, "led_anim", 4096, NULL,
                               tskIDLE_PRIORITY + 1, &s_led_task_handle);

  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create LED task");
    return ESP_FAIL;
  }

  return ESP_OK;
}

void led_control_set_state(led_state_t state) {
  s_current_state = state;
  // Reset tick for more predictable animation on state changes
  // (optional) s_tick = 0;
}

led_state_t led_control_get_state(void) { return s_current_state; }
