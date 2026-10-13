#ifndef TRACK_CONFIG_STORE_H
#define TRACK_CONFIG_STORE_H

#include "esp_err.h"
#include "track_config.h"

/* Loads config from NVS. Falls back to defaults (and returns ESP_ERR_NOT_FOUND
 * or the NVS error) when nothing valid is stored. cfg is always usable after
 * this call. */
esp_err_t track_config_load(track_config_t *cfg);

esp_err_t track_config_save(const track_config_t *cfg);

#endif
