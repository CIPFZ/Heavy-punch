#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "track_config.h"

static void expect_int(const char *name, long actual, long expected) {
  if (actual != expected) {
    fprintf(stderr, "%s: expected %ld, got %ld\n", name, expected, actual);
    exit(1);
  }
}

static void test_defaults(void) {
  track_config_t cfg;
  track_config_defaults(&cfg);
  expect_int("version", cfg.version, TRACK_CFG_VERSION);
  expect_int("deadzone", cfg.deadzone_pct, 8);
  expect_int("min pwm", cfg.min_pwm, 85);
  expect_int("slew", cfg.slew_step, 12);
  expect_int("timeout", cfg.timeout_ms, 350);
  expect_int("lowbat disabled", cfg.lowbat_mv, 0);
  expect_int("invert left", cfg.invert_left, 0);
  expect_int("invert right", cfg.invert_right, 0);
  expect_int("defaults valid", track_config_valid(&cfg), 1);
}

static void test_set(void) {
  track_config_t cfg;
  track_config_defaults(&cfg);

  expect_int("set dz", track_config_set(&cfg, "dz", 12), 1);
  expect_int("dz applied", cfg.deadzone_pct, 12);
  expect_int("dz too high", track_config_set(&cfg, "dz", 41), 0);
  expect_int("dz unchanged after reject", cfg.deadzone_pct, 12);
  expect_int("dz negative", track_config_set(&cfg, "dz", -1), 0);

  expect_int("set minpwm", track_config_set(&cfg, "minpwm", 100), 1);
  expect_int("minpwm max rejected", track_config_set(&cfg, "minpwm", 255), 0);

  expect_int("set slew", track_config_set(&cfg, "slew", 1), 1);
  expect_int("slew zero rejected", track_config_set(&cfg, "slew", 0), 0);
  expect_int("slew 255 ok", track_config_set(&cfg, "slew", 255), 1);

  expect_int("set timeout", track_config_set(&cfg, "timeout", 1000), 1);
  expect_int("timeout too low", track_config_set(&cfg, "timeout", 99), 0);
  expect_int("timeout too high", track_config_set(&cfg, "timeout", 5001), 0);

  expect_int("set lowbat", track_config_set(&cfg, "lowbat", 6600), 1);
  expect_int("lowbat applied", cfg.lowbat_mv, 6600);

  expect_int("set invl", track_config_set(&cfg, "invl", 1), 1);
  expect_int("invl applied", cfg.invert_left, 1);
  expect_int("invr 2 rejected", track_config_set(&cfg, "invr", 2), 0);

  expect_int("unknown key", track_config_set(&cfg, "speed", 1), 0);
  expect_int("null cfg", track_config_set(NULL, "dz", 1), 0);
  expect_int("null key", track_config_set(&cfg, NULL, 1), 0);
  expect_int("still valid", track_config_valid(&cfg), 1);
}

static void test_format(void) {
  track_config_t cfg;
  track_config_defaults(&cfg);
  char buf[96];
  const int n = track_config_format(&cfg, buf, sizeof(buf));
  expect_int("format length positive", n > 0, 1);
  expect_int("format content",
             strcmp(buf, "dz=8,minpwm=85,slew=12,timeout=350,lowbat=0,invl=0,invr=0"), 0);

  char small[8];
  expect_int("format too small", track_config_format(&cfg, small, sizeof(small)), -1);
}

static void test_valid(void) {
  track_config_t cfg;
  track_config_defaults(&cfg);
  cfg.version = 99;
  expect_int("bad version invalid", track_config_valid(&cfg), 0);
  track_config_defaults(&cfg);
  cfg.slew_step = 0;
  expect_int("zero slew invalid", track_config_valid(&cfg), 0);
  track_config_defaults(&cfg);
  cfg.timeout_ms = 10;
  expect_int("tiny timeout invalid", track_config_valid(&cfg), 0);
  expect_int("null invalid", track_config_valid(NULL), 0);
}

int main(void) {
  test_defaults();
  test_set();
  test_format();
  test_valid();
  puts("track config tests passed");
  return 0;
}
