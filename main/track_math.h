#ifndef TRACK_MATH_H
#define TRACK_MATH_H

#include <stdint.h>

#define TRACK_PWM_MAX 255
#define TRACK_DEADZONE_PERCENT 8
#define TRACK_MIN_EFFECTIVE_PWM 85
#define TRACK_SLEW_STEP 12

int16_t track_percent_to_pwm(int16_t pct);
int track_parse_command(const char *message, int16_t *left_pct, int16_t *right_pct);
int16_t track_approach_with_step(int16_t current, int16_t target, uint8_t step);

#endif
