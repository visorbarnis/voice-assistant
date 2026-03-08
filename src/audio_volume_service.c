/**
 * @file audio_volume_service.c
 * @brief Output volume control for PCM audio sent to I2S
 */

#include "audio_volume_service.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "AUDIO_VOL";
static volatile uint32_t s_volume_percent = 100;
static bool s_initialized = false;
static const uint32_t MAX_GAIN_PERCENT = 100;

static bool is_valid_volume(uint32_t volume_percent) {
  return volume_percent <= 100;
}

static uint32_t volume_to_gain_percent(uint32_t volume_percent) {
  return (volume_percent * MAX_GAIN_PERCENT + 50U) / 100U;
}

esp_err_t audio_volume_service_init(uint32_t volume_percent) {
  if (!is_valid_volume(volume_percent)) {
    return ESP_ERR_INVALID_ARG;
  }

  s_volume_percent = volume_percent;
  s_initialized = true;
  ESP_LOGI(TAG, "Playback volume initialized: %" PRIu32 "%% (gain=%" PRIu32 "%%)",
           volume_percent, volume_to_gain_percent(volume_percent));
  return ESP_OK;
}

esp_err_t audio_volume_service_set(uint32_t volume_percent) {
  if (!is_valid_volume(volume_percent)) {
    return ESP_ERR_INVALID_ARG;
  }

  s_volume_percent = volume_percent;
  if (s_initialized) {
    ESP_LOGI(TAG, "Playback volume updated: %" PRIu32 "%% (gain=%" PRIu32 "%%)",
             volume_percent, volume_to_gain_percent(volume_percent));
  }
  return ESP_OK;
}

uint32_t audio_volume_service_get(void) { return s_volume_percent; }

void audio_volume_service_apply_pcm16(int16_t *samples, size_t sample_count) {
  if (!samples || sample_count == 0) {
    return;
  }

  uint32_t gain_percent = volume_to_gain_percent(s_volume_percent);
  if (gain_percent == 100) {
    return;
  }

  if (gain_percent == 0) {
    memset(samples, 0, sample_count * sizeof(int16_t));
    return;
  }

  for (size_t i = 0; i < sample_count; i++) {
    int32_t scaled = ((int32_t)samples[i] * (int32_t)gain_percent) / 100;
    if (scaled > INT16_MAX) {
      scaled = INT16_MAX;
    } else if (scaled < INT16_MIN) {
      scaled = INT16_MIN;
    }
    samples[i] = (int16_t)scaled;
  }
}
