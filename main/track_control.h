#ifndef TRACK_CONTROL_H
#define TRACK_CONTROL_H

#include <stdbool.h>

#include "esp_err.h"
#include "track_math.h"

/*
 * The control task is the single owner of the safety state machine and the
 * only writer to the drive layer during normal operation. Everything else
 * (web server, session management) talks to it through a queue.
 */

esp_err_t track_control_start(void);

/* Posts a parsed command from client `fd`. Returns false if the queue is full
 * (the frame is dropped; the next one arrives within 80 ms anyway). */
bool track_control_post(int fd, const track_command_t *cmd);

/* Posts a controller-disconnect notification. */
void track_control_post_controller_lost(void);

#endif
