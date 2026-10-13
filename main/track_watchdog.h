#ifndef TRACK_WATCHDOG_H
#define TRACK_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Independent motion watchdog. Runs on the esp_timer task, which has a higher
 * priority than the control task, so it still fires if the control task is
 * blocked or starved. On expiry it calls track_drive_fault_brake() directly
 * (no queue, no state machine) and records the trip for the control task.
 */

esp_err_t track_watchdog_init(void);

/* (Re)starts the one-shot timer. Call on every accepted control frame. */
void track_watchdog_feed(uint32_t timeout_ms);

/* Stops the timer. Call when the drive is disarmed. */
void track_watchdog_stop(void);

/* Returns true once per trip and clears the flag. */
bool track_watchdog_take_trip(void);

#endif
