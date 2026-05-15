#include <stdio.h>
#include <stdlib.h>

#include "track_math.h"

static void expect_int(const char *name, int actual, int expected) {
  if (actual != expected) {
    fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
    exit(1);
  }
}

static void test_percent_to_pwm(void) {
  expect_int("zero maps to zero", track_percent_to_pwm(0), 0);
  expect_int("deadzone positive maps to zero", track_percent_to_pwm(8), 0);
  expect_int("deadzone negative maps to zero", track_percent_to_pwm(-8), 0);
  expect_int("small movement gets minimum pwm", track_percent_to_pwm(9), 85);
  expect_int("negative small movement gets negative minimum pwm", track_percent_to_pwm(-9), -85);
  expect_int("full forward maps to max pwm", track_percent_to_pwm(100), 255);
  expect_int("full reverse maps to negative max pwm", track_percent_to_pwm(-100), -255);
  expect_int("high values clamp", track_percent_to_pwm(180), 255);
  expect_int("low values clamp", track_percent_to_pwm(-180), -255);
}

static void test_parse_tracks_command(void) {
  int16_t left = 0;
  int16_t right = 0;
  expect_int("valid command accepted", track_parse_command("tracks:42:-17", &left, &right), 1);
  expect_int("left parsed", left, 42);
  expect_int("right parsed", right, -17);

  expect_int("values clamp accepted", track_parse_command("tracks:120:-140", &left, &right), 1);
  expect_int("left clamped", left, 100);
  expect_int("right clamped", right, -100);

  expect_int("stop command accepted", track_parse_command("stop", &left, &right), 1);
  expect_int("stop left", left, 0);
  expect_int("stop right", right, 0);

  expect_int("missing prefix rejected", track_parse_command("42:-17", &left, &right), 0);
  expect_int("extra suffix rejected", track_parse_command("tracks:42:-17:x", &left, &right), 0);
  expect_int("non number rejected", track_parse_command("tracks:go:-17", &left, &right), 0);
  expect_int("empty rejected", track_parse_command("", &left, &right), 0);
  expect_int("null rejected", track_parse_command(NULL, &left, &right), 0);
}

static void test_slew(void) {
  expect_int("slew up by step", track_approach_with_step(0, 100, 12), 12);
  expect_int("slew down by step", track_approach_with_step(0, -100, 12), -12);
  expect_int("slew stops at target up", track_approach_with_step(94, 100, 12), 100);
  expect_int("slew stops at target down", track_approach_with_step(-94, -100, 12), -100);
  expect_int("zero step holds current", track_approach_with_step(10, 100, 0), 10);
}

int main(void) {
  test_percent_to_pwm();
  test_parse_tracks_command();
  test_slew();
  puts("track math tests passed");
  return 0;
}
