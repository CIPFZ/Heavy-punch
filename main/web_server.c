#include "web_server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_stream.h"
#include "track_drive.h"
#include "track_math.h"
#include "web_ui.h"

static const char *TAG = "web_server";
static httpd_handle_t server;
static int16_t shared_left_pct;
static int16_t shared_right_pct;
static SemaphoreHandle_t control_state_mutex;

static void set_shared_tracks(int16_t left, int16_t right) {
  if (xSemaphoreTake(control_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    shared_left_pct = left;
    shared_right_pct = right;
    xSemaphoreGive(control_state_mutex);
  }
}

static void get_shared_tracks(int16_t *left, int16_t *right) {
  if (left == NULL || right == NULL) {
    return;
  }
  if (xSemaphoreTake(control_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    *left = shared_left_pct;
    *right = shared_right_pct;
    xSemaphoreGive(control_state_mutex);
  }
}

static int16_t clamp_percent(long value) {
  if (value < -100) {
    return -100;
  }
  if (value > 100) {
    return 100;
  }
  return (int16_t)value;
}

static bool parse_axis_command(const char *message, char *axis, int16_t *pct) {
  if (message == NULL || axis == NULL || pct == NULL) {
    return false;
  }

  const char prefix[] = "axis:";
  if (strncmp(message, prefix, sizeof(prefix) - 1) != 0) {
    return false;
  }

  const char *axis_pos = message + sizeof(prefix) - 1;
  if ((axis_pos[0] != 'L' && axis_pos[0] != 'R') || axis_pos[1] != ':') {
    return false;
  }

  char *end = NULL;
  const long value = strtol(axis_pos + 2, &end, 10);
  if (end == axis_pos + 2 || end == NULL || *end != '\0') {
    return false;
  }

  *axis = axis_pos[0];
  *pct = clamp_percent(value);
  return true;
}

static void apply_axis_percent(char axis, int16_t pct) {
  int16_t left = 0;
  int16_t right = 0;
  get_shared_tracks(&left, &right);

  if (axis == 'L') {
    left = pct;
  } else if (axis == 'R') {
    right = pct;
  }

  set_shared_tracks(left, right);
  track_drive_set_percent(left, right);
}

static esp_err_t send_control_state(httpd_req_t *req) {
  int16_t left = 0;
  int16_t right = 0;
  get_shared_tracks(&left, &right);

  char payload[40];
  const int len = snprintf(payload, sizeof(payload), "state:%d:%d", left, right);
  httpd_ws_frame_t frame = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = (uint8_t *)payload,
      .len = (size_t)len,
  };
  return httpd_ws_send_frame(req, &frame);
}

static esp_err_t dispatch_control_message(const char *message) {
  if (message == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (strcmp(message, "stop") == 0) {
    set_shared_tracks(0, 0);
    track_drive_stop();
    return ESP_OK;
  }

  int16_t left = 0;
  int16_t right = 0;
  char axis = '\0';
  int16_t pct = 0;
  if (parse_axis_command(message, &axis, &pct)) {
    apply_axis_percent(axis, pct);
    return ESP_OK;
  }

  if (track_parse_command(message, &left, &right)) {
    set_shared_tracks(left, right);
    track_drive_set_percent(left, right);
    return ESP_OK;
  }

  if (strncmp(message, "servo:", 6) == 0) {
    ESP_LOGI(TAG, "servo command reserved: %s", message);
    return ESP_OK;
  }

  ESP_LOGW(TAG, "unknown control command: %s", message);
  return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, WEB_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    ESP_LOGI(TAG, "websocket connected");
    send_control_state(req);
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

  return dispatch_control_message(message);
}

esp_err_t web_server_start(void) {
  control_state_mutex = xSemaphoreCreateMutex();
  if (control_state_mutex == NULL) {
    return ESP_ERR_NO_MEM;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.core_id = 0;
  config.task_priority = 5;
  config.stack_size = 8192;
  config.max_open_sockets = 8;
  config.max_uri_handlers = 3;
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

  const httpd_uri_t capture = {
      .uri = "/capture.jpg",
      .method = HTTP_GET,
      .handler = camera_stream_capture_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &capture));

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
