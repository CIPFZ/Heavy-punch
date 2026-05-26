#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t camera_stream_init(void);
esp_err_t camera_stream_start(void);
esp_err_t camera_stream_capture_handler(httpd_req_t *req);
esp_err_t camera_stream_copy_latest(uint8_t *dst, size_t dst_capacity, size_t *out_len,
                                    uint32_t *out_seq);
size_t camera_stream_max_frame_bytes(void);
bool camera_stream_wait_for_frame(uint32_t last_seq);

#endif
