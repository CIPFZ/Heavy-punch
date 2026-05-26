#include "camera_tilt.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"

#define TILT_SERVO_GPIO GPIO_NUM_13
#define TILT_PWM_FREQ_HZ 50
#define TILT_PWM_MODE LEDC_LOW_SPEED_MODE
#define TILT_PWM_TIMER LEDC_TIMER_2
#define TILT_PWM_CHANNEL LEDC_CHANNEL_3
#define TILT_PWM_RESOLUTION LEDC_TIMER_14_BIT
#define TILT_DUTY_MAX ((1 << 14) - 1)
#define TILT_MIN_US 900
#define TILT_CENTER_US 1500
#define TILT_MAX_US 2100
#define TILT_PERIOD_US 20000

static const char *TAG = "camera_tilt";

static int16_t clamp_percent(int16_t value) {
  if (value < CAMERA_TILT_MIN_PERCENT) {
    return CAMERA_TILT_MIN_PERCENT;
  }
  if (value > CAMERA_TILT_MAX_PERCENT) {
    return CAMERA_TILT_MAX_PERCENT;
  }
  return value;
}

static uint32_t percent_to_duty(int16_t percent) {
  percent = clamp_percent(percent);
  const int32_t offset_us = percent >= 0
                                ? ((int32_t)(TILT_MAX_US - TILT_CENTER_US) * percent) / 100
                                : ((int32_t)(TILT_CENTER_US - TILT_MIN_US) * percent) / 100;
  const int32_t pulse_us = TILT_CENTER_US + offset_us;
  return (uint32_t)(((int64_t)pulse_us * TILT_DUTY_MAX) / TILT_PERIOD_US);
}

esp_err_t camera_tilt_init(void) {
  ledc_timer_config_t timer_conf = {
      .speed_mode = TILT_PWM_MODE,
      .duty_resolution = TILT_PWM_RESOLUTION,
      .timer_num = TILT_PWM_TIMER,
      .freq_hz = TILT_PWM_FREQ_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_conf), TAG, "tilt ledc timer config failed");

  ledc_channel_config_t channel_conf = {
      .gpio_num = TILT_SERVO_GPIO,
      .speed_mode = TILT_PWM_MODE,
      .channel = TILT_PWM_CHANNEL,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = TILT_PWM_TIMER,
      .duty = percent_to_duty(CAMERA_TILT_DEFAULT_PERCENT),
      .hpoint = 0,
  };
  ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_conf), TAG, "tilt ledc channel config failed");

  ESP_LOGI(TAG, "initialized: gpio=%d range=%dus..%dus center=%dus", TILT_SERVO_GPIO, TILT_MIN_US,
           TILT_MAX_US, TILT_CENTER_US);
  return ESP_OK;
}

void camera_tilt_set_percent(int16_t percent) {
  const uint32_t duty = percent_to_duty(percent);
  ledc_set_duty(TILT_PWM_MODE, TILT_PWM_CHANNEL, duty);
  ledc_update_duty(TILT_PWM_MODE, TILT_PWM_CHANNEL);
}
