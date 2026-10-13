#ifndef TRACK_MATH_H
#define TRACK_MATH_H

#include <stdbool.h>
#include <stdint.h>

#include "track_config.h"

/*
 * Pure math and protocol parsing. No ESP-IDF dependency.
 *
 * Client -> firmware text frames:
 *   arm                  request control and arm the drive
 *   stop                 emergency stop, disarm (also clears FAULT)
 *   c:<seq>:<L>:<R>      control frame, seq is a monotonically increasing u32
 *   cfg:get              request current config
 *   cfg:save             persist current config to NVS
 *   cfg:<key>=<value>    set one config key (only while DISARMED or FAULT)
 */

typedef enum {
  TRACK_CMD_INVALID = 0,
  TRACK_CMD_ARM,
  TRACK_CMD_STOP,
  TRACK_CMD_CONTROL,
  TRACK_CMD_CFG_GET,
  TRACK_CMD_CFG_SAVE,
  TRACK_CMD_CFG_SET,
} track_cmd_type_t;

#define TRACK_CFG_KEY_LEN 8

typedef struct {
  track_cmd_type_t type;
  uint32_t seq;
  int16_t left_pct;
  int16_t right_pct;
  char key[TRACK_CFG_KEY_LEN];
  long value;
} track_command_t;

/* Lever percentage (-100..100) to signed PWM (-255..255) using deadzone,
 * minimum effective PWM and a square response curve. */
int16_t track_percent_to_pwm(int16_t pct, const track_config_t *cfg);

/* Parses one text frame. Returns false and sets out->type = TRACK_CMD_INVALID
 * on any format error. Percentages are clamped to -100..100. */
bool track_parse_command(const char *message, track_command_t *out);

/* Moves current toward target by at most step. */
int16_t track_approach_with_step(int16_t current, int16_t target, uint8_t step);

#endif
