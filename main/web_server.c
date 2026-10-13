#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "track_control.h"
#include "track_math.h"

/* Embedded by EMBED_TXTFILES in main/CMakeLists.txt (NUL-terminated). */
extern const char web_ui_html_start[] asm("_binary_web_ui_html_start");

#define MAX_FRAME_LEN 64
#define MAX_OUT_LEN 128

static const char *TAG = "web_server";
static httpd_handle_t s_server;

/* Only touched from the httpd task (handlers, close_fn and queued work). */
static int s_controller_fd = -1;

typedef struct {
  int fd;      /* -1 means broadcast */
  bool status; /* append owner flag per client */
  char text[MAX_OUT_LEN];
} out_msg_t;

static void send_text(int fd, const char *text) {
  httpd_ws_frame_t frame = {
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = (uint8_t *)text,
      .len = strlen(text),
  };
  httpd_ws_send_frame_async(s_server, fd, &frame);
}

static void send_work(void *arg) {
  out_msg_t *msg = (out_msg_t *)arg;

  if (msg->fd >= 0) {
    send_text(msg->fd, msg->text);
    free(msg);
    return;
  }

  size_t count = CONFIG_LWIP_MAX_SOCKETS;
  int fds[CONFIG_LWIP_MAX_SOCKETS];
  if (httpd_get_client_list(s_server, &count, fds) == ESP_OK) {
    char line[MAX_OUT_LEN + 4];
    for (size_t i = 0; i < count; i++) {
      if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
        continue;
      }
      if (msg->status) {
        snprintf(line, sizeof(line), "%s:%d", msg->text, fds[i] == s_controller_fd ? 1 : 0);
        send_text(fds[i], line);
      } else {
        send_text(fds[i], msg->text);
      }
    }
  }
  free(msg);
}

static void enqueue(int fd, bool status, const char *text) {
  if (s_server == NULL || text == NULL) {
    return;
  }
  out_msg_t *msg = calloc(1, sizeof(*msg));
  if (msg == NULL) {
    return;
  }
  msg->fd = fd;
  msg->status = status;
  strlcpy(msg->text, text, sizeof(msg->text));
  if (httpd_queue_work(s_server, send_work, msg) != ESP_OK) {
    free(msg);
  }
}

void web_server_send(int fd, const char *text) {
  enqueue(fd, false, text);
}

void web_server_broadcast_status(const char *text) {
  enqueue(-1, true, text);
}

static esp_err_t root_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, web_ui_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ws_handler(httpd_req_t *req) {
  const int fd = httpd_req_to_sockfd(req);

  if (req->method == HTTP_GET) {
    ESP_LOGI(TAG, "websocket connected fd=%d", fd);
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {0};
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) {
    return err;
  }
  if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0 || frame.len >= MAX_FRAME_LEN) {
    return ESP_OK;
  }

  char message[MAX_FRAME_LEN];
  frame.payload = (uint8_t *)message;
  err = httpd_ws_recv_frame(req, &frame, sizeof(message) - 1);
  if (err != ESP_OK) {
    return err;
  }
  message[frame.len] = '\0';

  track_command_t cmd;
  if (!track_parse_command(message, &cmd)) {
    send_text(fd, "err:bad command");
    return ESP_OK;
  }

  /* Control ownership: first `arm` wins, everyone else is a spectator. */
  if (cmd.type == TRACK_CMD_ARM) {
    if (s_controller_fd >= 0 && s_controller_fd != fd) {
      send_text(fd, "err:busy, another client has control");
      return ESP_OK;
    }
    if (s_controller_fd != fd) {
      s_controller_fd = fd;
      ESP_LOGI(TAG, "fd=%d took control", fd);
    }
  } else if (cmd.type != TRACK_CMD_CFG_GET && fd != s_controller_fd) {
    send_text(fd, "err:not controller");
    return ESP_OK;
  }

  track_control_post(fd, &cmd);
  return ESP_OK;
}

static void on_session_close(httpd_handle_t hd, int sockfd) {
  (void)hd;
  if (sockfd == s_controller_fd) {
    ESP_LOGW(TAG, "controller fd=%d closed", sockfd);
    s_controller_fd = -1;
    track_control_post_controller_lost();
  } else {
    ESP_LOGI(TAG, "client fd=%d closed", sockfd);
  }
  close(sockfd);
}

esp_err_t web_server_start(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;
  config.close_fn = on_session_close;

  esp_err_t err = httpd_start(&s_server, &config);
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
  ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root));

  const httpd_uri_t ws = {
      .uri = "/ws",
      .method = HTTP_GET,
      .handler = ws_handler,
      .user_ctx = NULL,
      .is_websocket = true,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &ws));

  ESP_LOGI(TAG, "started on http://192.168.4.1");
  return ESP_OK;
}
