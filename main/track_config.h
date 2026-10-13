#ifndef TRACK_CONFIG_H
#define TRACK_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Runtime-tunable drive parameters. Pure C, no ESP-IDF dependency, so this
 * header is shared between firmware and host tests. Persistence lives in
 * track_config_store.c.
 */

#define TRACK_PWM_MAX 255

#define TRACK_CFG_VERSION 1

typedef struct {
  uint8_t version;
  uint8_t deadzone_pct;  /* 0..40, lever travel ignored around center */
  uint8_t min_pwm;       /* 0..254, first PWM value that actually moves the motor */
  uint8_t slew_step;     /* 1..255, max PWM change per drive update tick */
  uint16_t timeout_ms;   /* 100..5000, software failsafe: no frame -> disarm */
  uint16_t lowbat_mv;    /* 0 disables, otherwise pack voltage below this -> FAULT */
  bool invert_left;
  bool invert_right;
} track_config_t;

void track_config_defaults(track_config_t *cfg);

/* Validates the key and range. Returns false and leaves cfg untouched on error.
 * Keys: dz, minpwm, slew, timeout, lowbat, invl, invr */
bool track_config_set(track_config_t *cfg, const char *key, long value);

/* Writes "dz=8,minpwm=85,slew=12,timeout=350,lowbat=0,invl=0,invr=0".
 * Returns number of chars written (excluding NUL), or -1 if buf too small. */
int track_config_format(const track_config_t *cfg, char *buf, size_t len);

/* Returns true if every field is inside its allowed range. */
bool track_config_valid(const track_config_t *cfg);

#endif
