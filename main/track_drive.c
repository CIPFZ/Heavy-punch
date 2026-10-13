#include "track_drive.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

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

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Protected by s_lock. */
static int16_t s_left_target;
static int16_t s_right_target;
static bool s_braked = true;
static bool s_fault;
static bool s_invert_left;
static bool s_invert_right;

/* Only touched by the task that calls track_drive_update(). */
static int16_t s_left_current;
static int16_t s_right_current;

static void write_pwm(ledc_channel_t channel, int16_t pwm) {
  const uint32_t duty = (uint32_t)(pwm < 0 ? -pwm : pwm);
  ledc_set_duty(PWM_MODE, channel, duty);
  ledc_update_duty(PWM_MODE, channel);
}

static void write_motor(gpio_num_t in1, gpio_num_t in2, ledc_channel_t channel, int16_t pwm,
                        bool invert) {
  if (invert) {
    pwm = (int16_t)-pwm;
  }
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

static void write_brake_outputs(void) {
  gpio_set_level(AIN1, 1);
  gpio_set_level(AIN2, 1);
  gpio_set_level(BIN1, 1);
  gpio_set_level(BIN2, 1);
  write_pwm(PWM_LEFT_CHANNEL, 0);
  write_pwm(PWM_RIGHT_CHANNEL, 0);
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

  const ledc_channel_config_t channels[] = {
      {.gpio_num = PWMA, .speed_mode = PWM_MODE, .channel = PWM_LEFT_CHANNEL,
       .intr_type = LEDC_INTR_DISABLE, .timer_sel = PWM_TIMER, .duty = 0, .hpoint = 0},
      {.gpio_num = PWMB, .speed_mode = PWM_MODE, .channel = PWM_RIGHT_CHANNEL,
       .intr_type = LEDC_INTR_DISABLE, .timer_sel = PWM_TIMER, .duty = 0, .hpoint = 0},
  };
  for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channels[i]), TAG, "ledc channel config failed");
  }

  track_drive_brake();
  ESP_LOGI(TAG, "initialized: left pwm=%d in1=%d in2=%d, right pwm=%d in1=%d in2=%d",
           PWMA, AIN1, AIN2, PWMB, BIN1, BIN2);
  return ESP_OK;
}

void track_drive_set_invert(bool invert_left, bool invert_right) {
  portENTER_CRITICAL(&s_lock);
  s_invert_left = invert_left;
  s_invert_right = invert_right;
  portEXIT_CRITICAL(&s_lock);
}

void track_drive_set_target(int16_t left_pwm, int16_t right_pwm) {
  portENTER_CRITICAL(&s_lock);
  if (!s_fault) {
    s_left_target = left_pwm;
    s_right_target = right_pwm;
    s_braked = false;
  }
  portEXIT_CRITICAL(&s_lock);
}

void track_drive_brake(void) {
  portENTER_CRITICAL(&s_lock);
  s_left_target = 0;
  s_right_target = 0;
  s_braked = true;
  portEXIT_CRITICAL(&s_lock);
  write_brake_outputs();
}

void track_drive_fault_brake(void) {
  portENTER_CRITICAL(&s_lock);
  s_left_target = 0;
  s_right_target = 0;
  s_braked = true;
  s_fault = true;
  portEXIT_CRITICAL(&s_lock);
  write_brake_outputs();
}

void track_drive_clear_fault(void) {
  portENTER_CRITICAL(&s_lock);
  s_fault = false;
  portEXIT_CRITICAL(&s_lock);
}

bool track_drive_fault_latched(void) {
  portENTER_CRITICAL(&s_lock);
  const bool fault = s_fault;
  portEXIT_CRITICAL(&s_lock);
  return fault;
}

void track_drive_update(uint8_t slew_step) {
  portENTER_CRITICAL(&s_lock);
  const bool hold_brake = s_braked || s_fault;
  const int16_t left_target = s_left_target;
  const int16_t right_target = s_right_target;
  const bool invert_left = s_invert_left;
  const bool invert_right = s_invert_right;
  portEXIT_CRITICAL(&s_lock);

  if (hold_brake) {
    s_left_current = 0;
    s_right_current = 0;
    write_brake_outputs();
    return;
  }

  s_left_current = track_approach_with_step(s_left_current, left_target, slew_step);
  s_right_current = track_approach_with_step(s_right_current, right_target, slew_step);
  write_motor(AIN1, AIN2, PWM_LEFT_CHANNEL, s_left_current, invert_left);
  write_motor(BIN1, BIN2, PWM_RIGHT_CHANNEL, s_right_current, invert_right);
}

void track_drive_get_actual(int16_t *left_pwm, int16_t *right_pwm) {
  if (left_pwm) {
    *left_pwm = s_left_current;
  }
  if (right_pwm) {
    *right_pwm = s_right_current;
  }
}
