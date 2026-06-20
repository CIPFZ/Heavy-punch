#ifndef CONTROL_LOOP_H
#define CONTROL_LOOP_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
  int16_t left;
  int16_t right;
} control_tracks_t;

esp_err_t control_loop_init(void);
esp_err_t control_loop_start(void);
bool control_loop_submit_tracks(int16_t left, int16_t right);
bool control_loop_submit_tilt(int16_t percent);
void control_loop_submit_stop(void);

#endif
