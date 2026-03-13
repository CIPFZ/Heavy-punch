#ifndef WEB_UI_H
#define WEB_UI_H

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    WEB_UI_MODE_PROVISIONING = 0,
    WEB_UI_MODE_CHAT = 1,
} web_ui_mode_t;

esp_err_t web_ui_start(web_ui_mode_t mode);
bool web_ui_is_running(void);

#endif // WEB_UI_H
