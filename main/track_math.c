#include "track_math.h"

#include <stdlib.h>
#include <string.h>

static int16_t clamp_i16(int16_t value, int16_t min_value, int16_t max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

int16_t track_percent_to_pwm(int16_t pct) {
  pct = clamp_i16(pct, -100, 100);
  const int16_t sign = pct < 0 ? -1 : 1;
  const int16_t abs_pct = pct < 0 ? (int16_t)-pct : pct;

  if (abs_pct <= TRACK_DEADZONE_PERCENT) {
    return 0;
  }

  const int16_t linear = (int16_t)(((int32_t)(abs_pct - TRACK_DEADZONE_PERCENT) * 100) /
                                  (100 - TRACK_DEADZONE_PERCENT));
  const int16_t curved = (int16_t)(((int32_t)linear * linear) / 100);
  int16_t pwm = TRACK_MIN_EFFECTIVE_PWM +
                (int16_t)(((int32_t)(TRACK_PWM_MAX - TRACK_MIN_EFFECTIVE_PWM) * curved) / 100);
  pwm = clamp_i16(pwm, 0, TRACK_PWM_MAX);
  return sign * pwm;
}

int track_parse_command(const char *message, int16_t *left_pct, int16_t *right_pct) {
  if (message == NULL || left_pct == NULL || right_pct == NULL) {
    return 0;
  }

  if (strcmp(message, "stop") == 0) {
    *left_pct = 0;
    *right_pct = 0;
    return 1;
  }

  const char prefix[] = "tracks:";
  if (strncmp(message, prefix, sizeof(prefix) - 1) != 0) {
    return 0;
  }

  const char *left_start = message + sizeof(prefix) - 1;
  char *left_end = NULL;
  const long left = strtol(left_start, &left_end, 10);
  if (left_end == left_start || left_end == NULL || *left_end != ':') {
    return 0;
  }

  const char *right_start = left_end + 1;
  char *right_end = NULL;
  const long right = strtol(right_start, &right_end, 10);
  if (right_end == right_start || right_end == NULL || *right_end != '\0') {
    return 0;
  }

  *left_pct = clamp_i16((int16_t)left, -100, 100);
  *right_pct = clamp_i16((int16_t)right, -100, 100);
  return 1;
}

int16_t track_approach_with_step(int16_t current, int16_t target, uint8_t step) {
  if (step == 0) {
    return current;
  }
  if (current < target) {
    const int16_t next = current + step;
    return next > target ? target : next;
  }
  if (current > target) {
    const int16_t next = current - step;
    return next < target ? target : next;
  }
  return current;
}
