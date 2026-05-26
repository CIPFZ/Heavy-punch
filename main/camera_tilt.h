#ifndef CAMERA_TILT_H
#define CAMERA_TILT_H

#include <stdint.h>

#include "esp_err.h"

#define CAMERA_TILT_MIN_PERCENT -100
#define CAMERA_TILT_MAX_PERCENT 100
#define CAMERA_TILT_DEFAULT_PERCENT 0

esp_err_t camera_tilt_init(void);
void camera_tilt_set_percent(int16_t percent);

#endif
