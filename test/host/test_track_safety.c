#include <stdio.h>
#include <stdlib.h>

#include "track_safety.h"

static void expect_int(const char *name, long actual, long expected) {
  if (actual != expected) {
    fprintf(stderr, "%s: expected %ld, got %ld\n", name, expected, actual);
    exit(1);
  }
}

static track_safety_t armed(void) {
  track_safety_t s;
  track_safety_init(&s);
  track_safety_step(&s, TRACK_EVT_ARM, false);
  return s;
}

static void test_initial_state(void) {
  track_safety_t s;
  track_safety_init(&s);
  expect_int("starts disarmed", s.state, TRACK_STATE_DISARMED);
  expect_int("not armed helper", track_safety_is_armed(&s), 0);
}

static void test_disarmed_rejects_control(void) {
  track_safety_t s;
  track_safety_init(&s);
  expect_int("control rejected", track_safety_step(&s, TRACK_EVT_CONTROL, true), TRACK_ACTION_REJECT);
  expect_int("still disarmed", s.state, TRACK_STATE_DISARMED);
  expect_int("stop brakes", track_safety_step(&s, TRACK_EVT_STOP, false), TRACK_ACTION_BRAKE);
  expect_int("timeout is noop", track_safety_step(&s, TRACK_EVT_TIMEOUT, false), TRACK_ACTION_NONE);
  expect_int("lost controller brakes", track_safety_step(&s, TRACK_EVT_CONTROLLER_LOST, false), TRACK_ACTION_BRAKE);
  expect_int("still disarmed after all", s.state, TRACK_STATE_DISARMED);
}

static void test_arm_and_drive(void) {
  track_safety_t s;
  track_safety_init(&s);
  expect_int("arm brakes first", track_safety_step(&s, TRACK_EVT_ARM, false), TRACK_ACTION_BRAKE);
  expect_int("armed", s.state, TRACK_STATE_ARMED);
  expect_int("armed helper", track_safety_is_armed(&s), 1);

  expect_int("zero control drives", track_safety_step(&s, TRACK_EVT_CONTROL, false), TRACK_ACTION_DRIVE);
  expect_int("zero control stays armed", s.state, TRACK_STATE_ARMED);
  expect_int("moving control drives", track_safety_step(&s, TRACK_EVT_CONTROL, true), TRACK_ACTION_DRIVE);
  expect_int("moving control runs", s.state, TRACK_STATE_RUNNING);
  expect_int("zero again drives", track_safety_step(&s, TRACK_EVT_CONTROL, false), TRACK_ACTION_DRIVE);
  expect_int("back to armed", s.state, TRACK_STATE_ARMED);
}

static void test_running_exits(void) {
  track_safety_t s = armed();
  track_safety_step(&s, TRACK_EVT_CONTROL, true);
  expect_int("stop brakes", track_safety_step(&s, TRACK_EVT_STOP, false), TRACK_ACTION_BRAKE);
  expect_int("stop disarms", s.state, TRACK_STATE_DISARMED);

  s = armed();
  track_safety_step(&s, TRACK_EVT_CONTROL, true);
  expect_int("timeout brakes", track_safety_step(&s, TRACK_EVT_TIMEOUT, false), TRACK_ACTION_BRAKE);
  expect_int("timeout disarms", s.state, TRACK_STATE_DISARMED);

  s = armed();
  track_safety_step(&s, TRACK_EVT_CONTROL, true);
  expect_int("lost brakes", track_safety_step(&s, TRACK_EVT_CONTROLLER_LOST, false), TRACK_ACTION_BRAKE);
  expect_int("lost disarms", s.state, TRACK_STATE_DISARMED);

  s = armed();
  track_safety_step(&s, TRACK_EVT_CONTROL, true);
  expect_int("re-arm brakes", track_safety_step(&s, TRACK_EVT_ARM, false), TRACK_ACTION_BRAKE);
  expect_int("re-arm goes to armed", s.state, TRACK_STATE_ARMED);
}

static void test_fault(void) {
  track_safety_t s = armed();
  track_safety_step(&s, TRACK_EVT_CONTROL, true);
  expect_int("fault brakes", track_safety_step(&s, TRACK_EVT_FAULT, false), TRACK_ACTION_BRAKE);
  expect_int("in fault", s.state, TRACK_STATE_FAULT);

  expect_int("arm rejected in fault", track_safety_step(&s, TRACK_EVT_ARM, false), TRACK_ACTION_REJECT);
  expect_int("control rejected in fault", track_safety_step(&s, TRACK_EVT_CONTROL, true), TRACK_ACTION_REJECT);
  expect_int("timeout noop in fault", track_safety_step(&s, TRACK_EVT_TIMEOUT, false), TRACK_ACTION_NONE);
  expect_int("still fault", s.state, TRACK_STATE_FAULT);

  expect_int("stop clears fault with brake", track_safety_step(&s, TRACK_EVT_STOP, false), TRACK_ACTION_BRAKE);
  expect_int("back to disarmed", s.state, TRACK_STATE_DISARMED);

  track_safety_init(&s);
  expect_int("fault from disarmed", track_safety_step(&s, TRACK_EVT_FAULT, false), TRACK_ACTION_BRAKE);
  expect_int("fault state from disarmed", s.state, TRACK_STATE_FAULT);
}

static void test_seq_filter(void) {
  track_safety_t s = armed();
  expect_int("first seq accepted", track_safety_accept_seq(&s, 10), 1);
  expect_int("newer accepted", track_safety_accept_seq(&s, 11), 1);
  expect_int("duplicate rejected", track_safety_accept_seq(&s, 11), 0);
  expect_int("older rejected", track_safety_accept_seq(&s, 5), 0);
  expect_int("gap accepted", track_safety_accept_seq(&s, 100), 1);

  /* u32 wrap-around */
  s = armed();
  expect_int("near max accepted", track_safety_accept_seq(&s, 0xFFFFFFF0u), 1);
  expect_int("wrapped accepted", track_safety_accept_seq(&s, 3), 1);
  expect_int("pre-wrap now old", track_safety_accept_seq(&s, 0xFFFFFFFFu), 0);

  /* Re-arm resets the window so a client restarting from seq 0 works. */
  s = armed();
  track_safety_accept_seq(&s, 500);
  track_safety_step(&s, TRACK_EVT_STOP, false);
  track_safety_step(&s, TRACK_EVT_ARM, false);
  expect_int("seq window reset after re-arm", track_safety_accept_seq(&s, 0), 1);
}

static void test_names(void) {
  expect_int("name disarmed", track_state_name(TRACK_STATE_DISARMED)[0], 'D');
  expect_int("name armed", track_state_name(TRACK_STATE_ARMED)[0], 'A');
  expect_int("name running", track_state_name(TRACK_STATE_RUNNING)[0], 'R');
  expect_int("name fault", track_state_name(TRACK_STATE_FAULT)[0], 'F');
}

int main(void) {
  test_initial_state();
  test_disarmed_rejects_control();
  test_arm_and_drive();
  test_running_exits();
  test_fault();
  test_seq_filter();
  test_names();
  puts("track safety tests passed");
  return 0;
}
