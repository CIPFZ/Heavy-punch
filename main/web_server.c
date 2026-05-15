#include "web_server.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "track_drive.h"
#include "track_math.h"
#include "web_ui.h"

static const char *TAG = "web_server";
static httpd_handle_t server;

static esp_err_t root_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, WEB_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    ESP_LOGI(TAG, "websocket connected");
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {
      .type = HTTPD_WS_TYPE_TEXT,
  };
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) {
    return err;
  }

  if (frame.len == 0 || frame.len >= 48) {
    return ESP_OK;
  }

  char message[48];
  frame.payload = (uint8_t *)message;
  err = httpd_ws_recv_frame(req, &frame, sizeof(message) - 1);
  if (err != ESP_OK) {
    return err;
  }
  message[frame.len] = '\0';

  int16_t left = 0;
  int16_t right = 0;
  if (track_parse_command(message, &left, &right)) {
    track_drive_set_percent(left, right);
  }

  return ESP_OK;
}

esp_err_t web_server_start(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;

  esp_err_t err = httpd_start(&server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
    return err;
  }

  const httpd_uri_t root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = root_get_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));

  const httpd_uri_t ws = {
      .uri = "/ws",
      .method = HTTP_GET,
      .handler = ws_handler,
      .user_ctx = NULL,
      .is_websocket = true,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ws));

  ESP_LOGI(TAG, "started on http://192.168.4.1");
  return ESP_OK;
}
