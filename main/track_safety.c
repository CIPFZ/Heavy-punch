#include "track_safety.h"

void track_safety_init(track_safety_t *s) {
  s->state = TRACK_STATE_DISARMED;
  s->have_seq = false;
  s->last_seq = 0;
}

static track_action_t enter(track_safety_t *s, track_state_t next, track_action_t action) {
  s->state = next;
  return action;
}

track_action_t track_safety_step(track_safety_t *s, track_event_t evt, bool moving) {
  /* FAULT dominates every state. */
  if (evt == TRACK_EVT_FAULT) {
    return enter(s, TRACK_STATE_FAULT, TRACK_ACTION_BRAKE);
  }

  switch (s->state) {
    case TRACK_STATE_DISARMED:
      switch (evt) {
        case TRACK_EVT_ARM:
          s->have_seq = false;
          return enter(s, TRACK_STATE_ARMED, TRACK_ACTION_BRAKE);
        case TRACK_EVT_CONTROL:
          return TRACK_ACTION_REJECT;
        case TRACK_EVT_STOP:
        case TRACK_EVT_CONTROLLER_LOST:
          return TRACK_ACTION_BRAKE;
        default:
          return TRACK_ACTION_NONE;
      }

    case TRACK_STATE_ARMED:
    case TRACK_STATE_RUNNING:
      switch (evt) {
        case TRACK_EVT_ARM:
          /* Re-arm from the same controller: restart the seq window, stay safe. */
          s->have_seq = false;
          return enter(s, TRACK_STATE_ARMED, TRACK_ACTION_BRAKE);
        case TRACK_EVT_CONTROL:
          return enter(s, moving ? TRACK_STATE_RUNNING : TRACK_STATE_ARMED, TRACK_ACTION_DRIVE);
        case TRACK_EVT_STOP:
        case TRACK_EVT_TIMEOUT:
        case TRACK_EVT_CONTROLLER_LOST:
          return enter(s, TRACK_STATE_DISARMED, TRACK_ACTION_BRAKE);
        default:
          return TRACK_ACTION_NONE;
      }

    case TRACK_STATE_FAULT:
      switch (evt) {
        case TRACK_EVT_STOP:
          /* Operator acknowledged the fault. */
          return enter(s, TRACK_STATE_DISARMED, TRACK_ACTION_BRAKE);
        case TRACK_EVT_ARM:
        case TRACK_EVT_CONTROL:
          return TRACK_ACTION_REJECT;
        case TRACK_EVT_CONTROLLER_LOST:
          return TRACK_ACTION_BRAKE;
        default:
          return TRACK_ACTION_NONE;
      }
  }
  return TRACK_ACTION_NONE;
}

bool track_safety_accept_seq(track_safety_t *s, uint32_t seq) {
  if (!s->have_seq) {
    s->have_seq = true;
    s->last_seq = seq;
    return true;
  }
  /* Signed difference handles u32 wrap-around. */
  const int32_t delta = (int32_t)(seq - s->last_seq);
  if (delta <= 0) {
    return false;
  }
  s->last_seq = seq;
  return true;
}

const char *track_state_name(track_state_t state) {
  switch (state) {
    case TRACK_STATE_DISARMED:
      return "DISARMED";
    case TRACK_STATE_ARMED:
      return "ARMED";
    case TRACK_STATE_RUNNING:
      return "RUNNING";
    case TRACK_STATE_FAULT:
      return "FAULT";
  }
  return "UNKNOWN";
}
