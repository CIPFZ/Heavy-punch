#include "track_config.h"

#include <stdio.h>
#include <string.h>

void track_config_defaults(track_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->version = TRACK_CFG_VERSION;
  cfg->deadzone_pct = 8;
  cfg->min_pwm = 85;
  cfg->slew_step = 12;
  cfg->timeout_ms = 350;
  cfg->lowbat_mv = 0;
  cfg->invert_left = false;
  cfg->invert_right = false;
}

static bool in_range(long value, long lo, long hi) {
  return value >= lo && value <= hi;
}

bool track_config_set(track_config_t *cfg, const char *key, long value) {
  if (cfg == NULL || key == NULL) {
    return false;
  }
  if (strcmp(key, "dz") == 0) {
    if (!in_range(value, 0, 40)) return false;
    cfg->deadzone_pct = (uint8_t)value;
    return true;
  }
  if (strcmp(key, "minpwm") == 0) {
    if (!in_range(value, 0, TRACK_PWM_MAX - 1)) return false;
    cfg->min_pwm = (uint8_t)value;
    return true;
  }
  if (strcmp(key, "slew") == 0) {
    if (!in_range(value, 1, 255)) return false;
    cfg->slew_step = (uint8_t)value;
    return true;
  }
  if (strcmp(key, "timeout") == 0) {
    if (!in_range(value, 100, 5000)) return false;
    cfg->timeout_ms = (uint16_t)value;
    return true;
  }
  if (strcmp(key, "lowbat") == 0) {
    if (!in_range(value, 0, 30000)) return false;
    cfg->lowbat_mv = (uint16_t)value;
    return true;
  }
  if (strcmp(key, "invl") == 0) {
    if (!in_range(value, 0, 1)) return false;
    cfg->invert_left = value != 0;
    return true;
  }
  if (strcmp(key, "invr") == 0) {
    if (!in_range(value, 0, 1)) return false;
    cfg->invert_right = value != 0;
    return true;
  }
  return false;
}

int track_config_format(const track_config_t *cfg, char *buf, size_t len) {
  if (cfg == NULL || buf == NULL || len == 0) {
    return -1;
  }
  const int n = snprintf(buf, len, "dz=%u,minpwm=%u,slew=%u,timeout=%u,lowbat=%u,invl=%u,invr=%u",
                         cfg->deadzone_pct, cfg->min_pwm, cfg->slew_step, cfg->timeout_ms,
                         cfg->lowbat_mv, cfg->invert_left ? 1u : 0u, cfg->invert_right ? 1u : 0u);
  if (n < 0 || (size_t)n >= len) {
    return -1;
  }
  return n;
}

bool track_config_valid(const track_config_t *cfg) {
  if (cfg == NULL || cfg->version != TRACK_CFG_VERSION) {
    return false;
  }
  return cfg->deadzone_pct <= 40 && cfg->min_pwm < TRACK_PWM_MAX && cfg->slew_step >= 1 &&
         cfg->timeout_ms >= 100 && cfg->timeout_ms <= 5000 && cfg->lowbat_mv <= 30000;
}
