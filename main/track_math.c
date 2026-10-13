#include "track_math.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int16_t clamp_i16(long value, int16_t min_value, int16_t max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return (int16_t)value;
}

int16_t track_percent_to_pwm(int16_t pct, const track_config_t *cfg) {
  pct = clamp_i16(pct, -100, 100);
  const int16_t sign = pct < 0 ? -1 : 1;
  const int16_t abs_pct = pct < 0 ? (int16_t)-pct : pct;
  const int16_t deadzone = cfg->deadzone_pct;

  if (abs_pct <= deadzone) {
    return 0;
  }

  const int32_t span = 100 - deadzone;
  const int32_t linear = ((int32_t)(abs_pct - deadzone) * 100) / span;
  const int32_t curved = (linear * linear) / 100;
  int32_t pwm = cfg->min_pwm + ((int32_t)(TRACK_PWM_MAX - cfg->min_pwm) * curved) / 100;
  pwm = clamp_i16(pwm, 0, TRACK_PWM_MAX);
  return (int16_t)(sign * pwm);
}

/* Parses a decimal integer that must be terminated by `terminator`.
 * Advances *cursor past the terminator on success. */
static bool parse_long(const char **cursor, char terminator, long *out) {
  const char *start = *cursor;
  char *end = NULL;
  errno = 0;
  const long value = strtol(start, &end, 10);
  if (end == start || errno != 0 || *end != terminator) {
    return false;
  }
  *out = value;
  *cursor = terminator == '\0' ? end : end + 1;
  return true;
}

static bool parse_control(const char *body, track_command_t *out) {
  long seq = 0;
  long left = 0;
  long right = 0;
  const char *cursor = body;
  if (!parse_long(&cursor, ':', &seq) || seq < 0) {
    return false;
  }
  if (!parse_long(&cursor, ':', &left)) {
    return false;
  }
  if (!parse_long(&cursor, '\0', &right)) {
    return false;
  }
  out->type = TRACK_CMD_CONTROL;
  out->seq = (uint32_t)seq;
  out->left_pct = clamp_i16(left, -100, 100);
  out->right_pct = clamp_i16(right, -100, 100);
  return true;
}

static bool parse_cfg(const char *body, track_command_t *out) {
  if (strcmp(body, "get") == 0) {
    out->type = TRACK_CMD_CFG_GET;
    return true;
  }
  if (strcmp(body, "save") == 0) {
    out->type = TRACK_CMD_CFG_SAVE;
    return true;
  }
  const char *eq = strchr(body, '=');
  if (eq == NULL || eq == body) {
    return false;
  }
  const size_t key_len = (size_t)(eq - body);
  if (key_len >= TRACK_CFG_KEY_LEN) {
    return false;
  }
  for (size_t i = 0; i < key_len; i++) {
    const char c = body[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
      return false;
    }
  }
  const char *cursor = eq + 1;
  long value = 0;
  if (!parse_long(&cursor, '\0', &value)) {
    return false;
  }
  memcpy(out->key, body, key_len);
  out->key[key_len] = '\0';
  out->value = value;
  out->type = TRACK_CMD_CFG_SET;
  return true;
}

bool track_parse_command(const char *message, track_command_t *out) {
  if (out == NULL) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->type = TRACK_CMD_INVALID;
  if (message == NULL) {
    return false;
  }

  if (strcmp(message, "arm") == 0) {
    out->type = TRACK_CMD_ARM;
    return true;
  }
  if (strcmp(message, "stop") == 0) {
    out->type = TRACK_CMD_STOP;
    return true;
  }
  if (strncmp(message, "c:", 2) == 0) {
    if (parse_control(message + 2, out)) {
      return true;
    }
    out->type = TRACK_CMD_INVALID;
    return false;
  }
  if (strncmp(message, "cfg:", 4) == 0) {
    if (parse_cfg(message + 4, out)) {
      return true;
    }
    out->type = TRACK_CMD_INVALID;
    return false;
  }
  return false;
}

int16_t track_approach_with_step(int16_t current, int16_t target, uint8_t step) {
  if (step == 0) {
    return current;
  }
  if (current < target) {
    const int16_t next = (int16_t)(current + step);
    return next > target ? target : next;
  }
  if (current > target) {
    const int16_t next = (int16_t)(current - step);
    return next < target ? target : next;
  }
  return current;
}
