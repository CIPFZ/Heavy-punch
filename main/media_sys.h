#ifndef MEDIA_SYS_H
#define MEDIA_SYS_H

#include "esp_err.h"
#include "esp_webrtc.h"

esp_err_t media_sys_init(void);
esp_err_t media_sys_get_provider(esp_webrtc_media_provider_t *provider);

#endif
