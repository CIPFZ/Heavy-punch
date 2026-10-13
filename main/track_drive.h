#ifndef TRACK_DRIVE_H
#define TRACK_DRIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Hardware output layer for the two track motors (TB6612-style H-bridge:
 * one PWM pin plus two direction pins per motor). All GPIO/LEDC writes live
 * here. The module keeps two latches that are safe to touch from any task:
 *
 *   braked  - set by track_drive_brake(); outputs hold short-brake until a
 *             new target arrives.
 *   fault   - set by track_drive_fault_brake(); outputs hold short-brake and
 *             track_drive_set_target() is ignored until track_drive_clear_fault().
 *             Intended for the independent watchdog so a stuck control task
 *             cannot re-enable the motors.
 */

#define TRACK_DRIVE_UPDATE_INTERVAL_MS 15

esp_err_t track_drive_init(void);

void track_drive_set_invert(bool invert_left, bool invert_right);

/* Sets new signed PWM targets (-255..255). Clears the braked latch. Ignored
 * while the fault latch is set. */
void track_drive_set_target(int16_t left_pwm, int16_t right_pwm);

/* Immediate short-brake on both motors and reset of the slew state. */
void track_drive_brake(void);

/* Immediate short-brake plus fault latch. Safe to call from the esp_timer task. */
void track_drive_fault_brake(void);

void track_drive_clear_fault(void);
bool track_drive_fault_latched(void);

/* Call periodically (every TRACK_DRIVE_UPDATE_INTERVAL_MS). Slews the current
 * output toward the target by at most slew_step and writes the hardware. */
void track_drive_update(uint8_t slew_step);

void track_drive_get_actual(int16_t *left_pwm, int16_t *right_pwm);

#endif
