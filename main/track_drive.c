#include "track_drive.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "track_math.h"

#define PWMA GPIO_NUM_5
#define AIN2 GPIO_NUM_6
#define AIN1 GPIO_NUM_7
#define PWMB GPIO_NUM_18
#define BIN2 GPIO_NUM_17
#define BIN1 GPIO_NUM_16

#define PWM_FREQ_HZ 5000
#define PWM_TIMER LEDC_TIMER_0
#define PWM_MODE LEDC_LOW_SPEED_MODE
#define PWM_RESOLUTION LEDC_TIMER_8_BIT
#define PWM_LEFT_CHANNEL LEDC_CHANNEL_0
#define PWM_RIGHT_CHANNEL LEDC_CHANNEL_1

static const char *TAG = "track_drive";

static int16_t left_target_pwm;
static int16_t right_target_pwm;
static int16_t left_current_pwm;
static int16_t right_current_pwm;
static int64_t last_command_us;
static int64_t last_update_us;

static void write_pwm(ledc_channel_t channel, int16_t pwm) {
  const uint32_t duty = (uint32_t)(pwm < 0 ? -pwm : pwm);
  ledc_set_duty(PWM_MODE, channel, duty);
  ledc_update_duty(PWM_MODE, channel);
}

static void write_motor(gpio_num_t in1, gpio_num_t in2, ledc_channel_t channel, int16_t pwm) {
  if (pwm > 0) {
    gpio_set_level(in1, 1);
    gpio_set_level(in2, 0);
  } else if (pwm < 0) {
    gpio_set_level(in1, 0);
    gpio_set_level(in2, 1);
  } else {
    gpio_set_level(in1, 0);
    gpio_set_level(in2, 0);
  }
  write_pwm(channel, pwm);
}

static void brake_motor(gpio_num_t in1, gpio_num_t in2, ledc_channel_t channel) {
  gpio_set_level(in1, 1);
  gpio_set_level(in2, 1);
  write_pwm(channel, 0);
}

esp_err_t track_drive_init(void) {
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << AIN1) | (1ULL << AIN2) | (1ULL << BIN1) | (1ULL << BIN2),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio config failed");

  ledc_timer_config_t timer_conf = {
      .speed_mode = PWM_MODE,
      .duty_resolution = PWM_RESOLUTION,
      .timer_num = PWM_TIMER,
      .freq_hz = PWM_FREQ_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_conf), TAG, "ledc timer config failed");

  ledc_channel_config_t left_channel = {
      .gpio_num = PWMA,
      .speed_mode = PWM_MODE,
      .channel = PWM_LEFT_CHANNEL,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = PWM_TIMER,
      .duty = 0,
      .hpoint = 0,
  };
  ESP_RETURN_ON_ERROR(ledc_channel_config(&left_channel), TAG, "left ledc config failed");

  ledc_channel_config_t right_channel = {
      .gpio_num = PWMB,
      .speed_mode = PWM_MODE,
      .channel = PWM_RIGHT_CHANNEL,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = PWM_TIMER,
      .duty = 0,
      .hpoint = 0,
  };
  ESP_RETURN_ON_ERROR(ledc_channel_config(&right_channel), TAG, "right ledc config failed");

  track_drive_stop();
  ESP_LOGI(TAG, "initialized: left pwm=%d in1=%d in2=%d, right pwm=%d in1=%d in2=%d",
           PWMA, AIN1, AIN2, PWMB, BIN1, BIN2);
  return ESP_OK;
}

void track_drive_set_percent(int16_t left_pct, int16_t right_pct) {
  left_target_pwm = track_percent_to_pwm(left_pct);
  right_target_pwm = track_percent_to_pwm(right_pct);
  last_command_us = esp_timer_get_time();
}

void track_drive_stop(void) {
  left_target_pwm = 0;
  right_target_pwm = 0;
  left_current_pwm = 0;
  right_current_pwm = 0;
  last_command_us = esp_timer_get_time();
  brake_motor(AIN1, AIN2, PWM_LEFT_CHANNEL);
  brake_motor(BIN1, BIN2, PWM_RIGHT_CHANNEL);
}

void track_drive_update(void) {
  const int64_t now = esp_timer_get_time();
  if (now - last_update_us < TRACK_DRIVE_UPDATE_INTERVAL_MS * 1000LL) {
    return;
  }
  last_update_us = now;

  if (now - last_command_us > TRACK_COMMAND_TIMEOUT_MS * 1000LL) {
    left_target_pwm = 0;
    right_target_pwm = 0;
  }

  left_current_pwm = track_approach_with_step(left_current_pwm, left_target_pwm, TRACK_SLEW_STEP);
  right_current_pwm = track_approach_with_step(right_current_pwm, right_target_pwm, TRACK_SLEW_STEP);

  write_motor(AIN1, AIN2, PWM_LEFT_CHANNEL, left_current_pwm);
  write_motor(BIN1, BIN2, PWM_RIGHT_CHANNEL, right_current_pwm);
}
