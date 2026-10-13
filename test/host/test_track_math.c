#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "track_config.h"
#include "track_math.h"

static void expect_int(const char *name, long actual, long expected) {
  if (actual != expected) {
    fprintf(stderr, "%s: expected %ld, got %ld\n", name, expected, actual);
    exit(1);
  }
}

static void expect_str(const char *name, const char *actual, const char *expected) {
  if (strcmp(actual, expected) != 0) {
    fprintf(stderr, "%s: expected \"%s\", got \"%s\"\n", name, expected, actual);
    exit(1);
  }
}

static void test_percent_to_pwm(void) {
  track_config_t cfg;
  track_config_defaults(&cfg);
  expect_int("zero maps to zero", track_percent_to_pwm(0, &cfg), 0);
  expect_int("deadzone positive maps to zero", track_percent_to_pwm(8, &cfg), 0);
  expect_int("deadzone negative maps to zero", track_percent_to_pwm(-8, &cfg), 0);
  expect_int("small movement gets minimum pwm", track_percent_to_pwm(9, &cfg), 85);
  expect_int("negative small movement gets negative minimum pwm", track_percent_to_pwm(-9, &cfg), -85);
  expect_int("full forward maps to max pwm", track_percent_to_pwm(100, &cfg), 255);
  expect_int("full reverse maps to negative max pwm", track_percent_to_pwm(-100, &cfg), -255);
  expect_int("high values clamp", track_percent_to_pwm(180, &cfg), 255);
  expect_int("low values clamp", track_percent_to_pwm(-180, &cfg), -255);

  /* Curve is monotonic. */
  int16_t prev = 0;
  for (int16_t p = 0; p <= 100; p++) {
    const int16_t v = track_percent_to_pwm(p, &cfg);
    if (v < prev) {
      fprintf(stderr, "curve not monotonic at %d\n", p);
      exit(1);
    }
    prev = v;
  }

  /* Config changes take effect. */
  cfg.deadzone_pct = 0;
  cfg.min_pwm = 0;
  expect_int("no deadzone, 1% is non-zero or zero but not negative", track_percent_to_pwm(1, &cfg) >= 0, 1);
  expect_int("min pwm zero, full is max", track_percent_to_pwm(100, &cfg), 255);
  cfg.deadzone_pct = 20;
  cfg.min_pwm = 85;
  expect_int("wider deadzone", track_percent_to_pwm(20, &cfg), 0);
  expect_int("just past wider deadzone gets min pwm", track_percent_to_pwm(21, &cfg), 85);
}

static void test_parse_control(void) {
  track_command_t cmd;
  expect_int("valid control accepted", track_parse_command("c:7:42:-17", &cmd), 1);
  expect_int("type control", cmd.type, TRACK_CMD_CONTROL);
  expect_int("seq parsed", (long)cmd.seq, 7);
  expect_int("left parsed", cmd.left_pct, 42);
  expect_int("right parsed", cmd.right_pct, -17);

  expect_int("values clamp accepted", track_parse_command("c:1:120:-140", &cmd), 1);
  expect_int("left clamped", cmd.left_pct, 100);
  expect_int("right clamped", cmd.right_pct, -100);

  expect_int("large seq ok", track_parse_command("c:4294967295:0:0", &cmd), 1);
  expect_int("large seq value", (long)(cmd.seq == 4294967295u), 1);

  expect_int("missing seq rejected", track_parse_command("c:42:-17", &cmd), 0);
  expect_int("rejected sets invalid", cmd.type, TRACK_CMD_INVALID);
  expect_int("negative seq rejected", track_parse_command("c:-1:0:0", &cmd), 0);
  expect_int("extra suffix rejected", track_parse_command("c:1:42:-17:x", &cmd), 0);
  expect_int("non number rejected", track_parse_command("c:1:go:-17", &cmd), 0);
  expect_int("old protocol rejected", track_parse_command("tracks:42:-17", &cmd), 0);
  expect_int("empty rejected", track_parse_command("", &cmd), 0);
  expect_int("null rejected", track_parse_command(NULL, &cmd), 0);
  expect_int("null out rejected", track_parse_command("arm", NULL), 0);
}

static void test_parse_simple(void) {
  track_command_t cmd;
  expect_int("arm accepted", track_parse_command("arm", &cmd), 1);
  expect_int("arm type", cmd.type, TRACK_CMD_ARM);
  expect_int("stop accepted", track_parse_command("stop", &cmd), 1);
  expect_int("stop type", cmd.type, TRACK_CMD_STOP);
  expect_int("ARM uppercase rejected", track_parse_command("ARM", &cmd), 0);
  expect_int("arm with suffix rejected", track_parse_command("armx", &cmd), 0);
}

static void test_parse_cfg(void) {
  track_command_t cmd;
  expect_int("cfg get", track_parse_command("cfg:get", &cmd), 1);
  expect_int("cfg get type", cmd.type, TRACK_CMD_CFG_GET);
  expect_int("cfg save", track_parse_command("cfg:save", &cmd), 1);
  expect_int("cfg save type", cmd.type, TRACK_CMD_CFG_SAVE);

  expect_int("cfg set", track_parse_command("cfg:timeout=500", &cmd), 1);
  expect_int("cfg set type", cmd.type, TRACK_CMD_CFG_SET);
  expect_str("cfg set key", cmd.key, "timeout");
  expect_int("cfg set value", cmd.value, 500);

  expect_int("cfg set negative value parses", track_parse_command("cfg:dz=-3", &cmd), 1);
  expect_int("cfg set negative value", cmd.value, -3);

  expect_int("cfg empty key rejected", track_parse_command("cfg:=5", &cmd), 0);
  expect_int("cfg missing value rejected", track_parse_command("cfg:dz=", &cmd), 0);
  expect_int("cfg long key rejected", track_parse_command("cfg:abcdefghij=1", &cmd), 0);
  expect_int("cfg bad key char rejected", track_parse_command("cfg:d-z=1", &cmd), 0);
  expect_int("cfg trailing junk rejected", track_parse_command("cfg:dz=1x", &cmd), 0);
  expect_int("cfg unknown verb rejected", track_parse_command("cfg:reset", &cmd), 0);
}

static void test_slew(void) {
  expect_int("slew up by step", track_approach_with_step(0, 100, 12), 12);
  expect_int("slew down by step", track_approach_with_step(0, -100, 12), -12);
  expect_int("slew stops at target up", track_approach_with_step(94, 100, 12), 100);
  expect_int("slew stops at target down", track_approach_with_step(-94, -100, 12), -100);
  expect_int("zero step holds current", track_approach_with_step(10, 100, 0), 10);
  expect_int("at target stays", track_approach_with_step(50, 50, 12), 50);
}

int main(void) {
  test_percent_to_pwm();
  test_parse_control();
  test_parse_simple();
  test_parse_cfg();
  test_slew();
  puts("track math tests passed");
  return 0;
}
