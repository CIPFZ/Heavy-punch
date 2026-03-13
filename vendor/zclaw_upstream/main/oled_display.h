#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    OLED_DISPLAY_MODE_BOOT = 0,
    OLED_DISPLAY_MODE_SETUP = 1,
    OLED_DISPLAY_MODE_CHAT = 2,
    OLED_DISPLAY_MODE_SAFE = 3,
} oled_display_mode_t;

esp_err_t oled_display_start(oled_display_mode_t mode);
void oled_display_set_mode(oled_display_mode_t mode);
void oled_display_show_message(const char *line1,
                               const char *line2,
                               const char *line3,
                               const char *line4,
                               uint32_t duration_ms);
bool oled_display_is_available(void);

#endif // OLED_DISPLAY_H
