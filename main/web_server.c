#include "web_server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

#include "camera_stream.h"
#include "camera_tilt.h"
#include "track_drive.h"
#include "track_math.h"
#include "web_ui.h"

static const char *TAG = "web_server";
static httpd_handle_t server;

typedef struct {
  int fd;
} video_ws_client_t;

static int16_t clamp_percent(long value) {
  if (value < -100) {
    return -100;
  }
  if (value > 100) {
    return 100;
  }
  return (int16_t)value;
}

static esp_err_t dispatch_control_message(const char *message) {
  if (message == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (strcmp(message, "stop") == 0) {
    track_drive_stop();
    return ESP_OK;
  }

  int16_t left = 0;
  int16_t right = 0;
  if (track_parse_command(message, &left, &right)) {
    track_drive_set_percent(left, right);
    return ESP_OK;
  }

  const char tilt_prefix[] = "tilt:";
  if (strncmp(message, tilt_prefix, sizeof(tilt_prefix) - 1) == 0) {
    char *end = NULL;
    const char *value_start = message + sizeof(tilt_prefix) - 1;
    const long value = strtol(value_start, &end, 10);
    if (end != value_start && end != NULL && *end == '\0') {
      camera_tilt_set_percent(clamp_percent(value));
    }
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

static void video_ws_task(void *arg) {
  video_ws_client_t *client = (video_ws_client_t *)arg;
  const int fd = client->fd;
  free(client);

  const int flag = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
  struct timeval timeout = {
      .tv_sec = 0,
      .tv_usec = 200000,
  };
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  uint8_t *scratch =
      heap_caps_malloc(camera_stream_max_frame_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (scratch == NULL) {
    scratch = heap_caps_malloc(camera_stream_max_frame_bytes(), MALLOC_CAP_8BIT);
  }
  if (scratch == NULL) {
    ESP_LOGW(TAG, "video websocket buffer unavailable");
    httpd_sess_trigger_close(server, fd);
    vTaskDelete(NULL);
  }

  ESP_LOGI(TAG, "video websocket connected");
  uint32_t last_seq = 0;
  while (true) {
    camera_stream_wait_for_frame(last_seq);

    size_t len = 0;
    uint32_t seq = 0;
    esp_err_t err =
        camera_stream_copy_latest(scratch, camera_stream_max_frame_bytes(), &len, &seq);
    if (err != ESP_OK || seq == last_seq || len == 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    last_seq = seq;

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = scratch,
        .len = len,
    };
    err = httpd_ws_send_frame_async(server, fd, &frame);
    if (err != ESP_OK) {
      break;
    }
  }

  free(scratch);
  httpd_sess_trigger_close(server, fd);
  ESP_LOGI(TAG, "video websocket disconnected");
  vTaskDelete(NULL);
}

static esp_err_t video_ws_handler(httpd_req_t *req) {
  if (req->method != HTTP_GET) {
    return ESP_OK;
  }

  video_ws_client_t *client = calloc(1, sizeof(*client));
  if (client == NULL) {
    return ESP_ERR_NO_MEM;
  }
  client->fd = httpd_req_to_sockfd(req);

  BaseType_t ok =
      xTaskCreatePinnedToCore(video_ws_task, "video_ws", 6144, client, 4, NULL, 0);
  if (ok != pdPASS) {
    free(client);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t web_server_start(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.core_id = 0;
  config.task_priority = 5;
  config.stack_size = 8192;
  config.max_open_sockets = 8;
  config.max_uri_handlers = 4;
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

  const httpd_uri_t video_ws = {
      .uri = "/video-ws",
      .method = HTTP_GET,
      .handler = video_ws_handler,
      .user_ctx = NULL,
      .is_websocket = true,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &video_ws));

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
