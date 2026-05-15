#ifndef TRACK_DRIVE_H
#define TRACK_DRIVE_H

#include <stdint.h>

#include "esp_err.h"

#define TRACK_DRIVE_UPDATE_INTERVAL_MS 15
#define TRACK_COMMAND_TIMEOUT_MS 350

esp_err_t track_drive_init(void);
void track_drive_set_percent(int16_t left_pct, int16_t right_pct);
void track_drive_stop(void);
void track_drive_update(void);

#endif
