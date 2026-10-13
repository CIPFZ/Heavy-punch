#ifndef TRACK_SAFETY_H
#define TRACK_SAFETY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Drive safety state machine. Pure C, no ESP-IDF dependency.
 *
 *   DISARMED --arm--> ARMED <--control(zero)--> RUNNING
 *      ^                 |  control(moving) ^      |
 *      |                 +-------------------+      |
 *      +---- stop / timeout / controller lost ------+
 *
 *   any state --fault--> FAULT --stop--> DISARMED
 *
 * Control frames are only honoured in ARMED/RUNNING. Leaving FAULT requires an
 * explicit `stop` from the operator so a reconnecting client cannot restart
 * the tracks by accident.
 */

typedef enum {
  TRACK_STATE_DISARMED = 0,
  TRACK_STATE_ARMED,
  TRACK_STATE_RUNNING,
  TRACK_STATE_FAULT,
} track_state_t;

typedef enum {
  TRACK_EVT_ARM = 0,
  TRACK_EVT_CONTROL,
  TRACK_EVT_STOP,
  TRACK_EVT_TIMEOUT,
  TRACK_EVT_CONTROLLER_LOST,
  TRACK_EVT_FAULT,
} track_event_t;

typedef enum {
  TRACK_ACTION_NONE = 0, /* nothing to do */
  TRACK_ACTION_BRAKE,    /* brake both tracks now */
  TRACK_ACTION_DRIVE,    /* apply the control frame */
  TRACK_ACTION_REJECT,   /* event not allowed in this state */
} track_action_t;

typedef struct {
  track_state_t state;
  bool have_seq;
  uint32_t last_seq;
} track_safety_t;

void track_safety_init(track_safety_t *s);

/* Feeds one event. `moving` is only meaningful for TRACK_EVT_CONTROL and tells
 * whether the frame requests non-zero output. */
track_action_t track_safety_step(track_safety_t *s, track_event_t evt, bool moving);

/* Sequence filter for control frames. Accepts the first frame after arming and
 * any frame whose seq is strictly newer (wrap-safe). Rejected frames must be
 * dropped without touching the drive. */
bool track_safety_accept_seq(track_safety_t *s, uint32_t seq);

static inline bool track_safety_is_armed(const track_safety_t *s) {
  return s->state == TRACK_STATE_ARMED || s->state == TRACK_STATE_RUNNING;
}

const char *track_state_name(track_state_t state);

#endif
