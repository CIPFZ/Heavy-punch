#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t camera_stream_init(void);
esp_err_t camera_stream_start(void);
esp_err_t camera_stream_capture_handler(httpd_req_t *req);

#endif
