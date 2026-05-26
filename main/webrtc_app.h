#ifndef WEBRTC_APP_H
#define WEBRTC_APP_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t webrtc_app_init(void);
esp_err_t webrtc_app_start(void);
void webrtc_app_stop(void);

esp_err_t webrtc_app_page_handler(httpd_req_t *req);
esp_err_t webrtc_app_signal_get_handler(httpd_req_t *req);
esp_err_t webrtc_app_signal_post_handler(httpd_req_t *req);

#endif
