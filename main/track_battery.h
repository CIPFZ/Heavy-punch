#ifndef TRACK_BATTERY_H
#define TRACK_BATTERY_H

#include <stdint.h>

#include "esp_err.h"

/*
 * Pack voltage sensing through a resistor divider into ADC1.
 * Default wiring: GPIO4 (ADC1_CH3), divider R1=100k (top) / R2=10k (bottom).
 * Adjust the macros in track_battery.c to match the board.
 */

esp_err_t track_battery_init(void);

/* Returns estimated pack voltage in millivolts, or 0 if sensing is unavailable. */
uint32_t track_battery_read_mv(void);

#endif
