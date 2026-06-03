#include "web_server.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "camera_tilt.h"
#include "track_drive.h"
#include "track_math.h"
#include "webrtc_app.h"
#include "web_ui.h"
#include "wifi_manager.h"

static const char *TAG = "web_server";
static httpd_handle_t server;

static bool sockaddr_to_ipv4(const struct sockaddr_storage *addr, char *ip, size_t ip_size) {
  if (addr->ss_family == AF_INET) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    return inet_ntop(AF_INET, &in->sin_addr, ip, ip_size) != NULL;
  }
  if (addr->ss_family == AF_INET6) {
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
    const uint8_t *bytes = (const uint8_t *)&in6->sin6_addr;
    const bool v4mapped = memcmp(bytes, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12) == 0;
    if (v4mapped) {
      struct in_addr in4 = {0};
      memcpy(&in4, bytes + 12, sizeof(in4));
      return inet_ntop(AF_INET, &in4, ip, ip_size) != NULL;
    }
  }
  return false;
}

static esp_err_t session_open_handler(httpd_handle_t hd, int sockfd) {
  struct sockaddr_storage addr = {0};
  socklen_t addr_len = sizeof(addr);
  if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) != 0) {
    ESP_LOGW(TAG, "session peer lookup failed sockfd=%d errno=%d", sockfd, errno);
    return ESP_OK;
  }

  char *ip = calloc(1, INET_ADDRSTRLEN);
  if (ip == NULL) {
    return ESP_ERR_NO_MEM;
  }
  if (!sockaddr_to_ipv4(&addr, ip, INET_ADDRSTRLEN)) {
    ESP_LOGW(TAG, "unsupported session peer address family=%d", addr.ss_family);
    free(ip);
    return ESP_OK;
  }
  httpd_sess_set_ctx(hd, sockfd, ip, free);
  ESP_LOGD(TAG, "http session peer ip=%s", ip);
  return ESP_OK;
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

static esp_err_t wifi_status_handler(httpd_req_t *req) {
  wifi_manager_status_t status = {0};
  wifi_manager_get_status(&status);

  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddBoolToObject(root, "sta_configured", status.sta_configured);
  cJSON_AddBoolToObject(root, "sta_connected", status.sta_connected);
  cJSON_AddStringToObject(root, "sta_ssid", status.sta_ssid);
  char ip[16] = {0};
  if (status.sta_connected) {
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&status.sta_ip.ip));
  }
  cJSON_AddStringToObject(root, "sta_ip", ip);
  cJSON_AddStringToObject(root, "ap_url", "http://192.168.4.1");

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json == NULL) {
    return ESP_ERR_NO_MEM;
  }
  httpd_resp_set_type(req, "application/json");
  esp_err_t ret = httpd_resp_sendstr(req, json);
  free(json);
  return ret;
}

static esp_err_t read_json_body(httpd_req_t *req, cJSON **out) {
  if (req->content_len <= 0 || req->content_len > 256) {
    return ESP_ERR_INVALID_SIZE;
  }
  char *body = calloc(1, req->content_len + 1);
  if (body == NULL) {
    return ESP_ERR_NO_MEM;
  }
  int got = 0;
  while (got < req->content_len) {
    int ret = httpd_req_recv(req, body + got, req->content_len - got);
    if (ret <= 0) {
      free(body);
      return ESP_FAIL;
    }
    got += ret;
  }
  *out = cJSON_Parse(body);
  free(body);
  return *out ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t wifi_config_handler(httpd_req_t *req) {
  cJSON *root = NULL;
  if (read_json_body(req, &root) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    return ESP_FAIL;
  }

  const cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
  const cJSON *password = cJSON_GetObjectItem(root, "password");
  esp_err_t err = ESP_ERR_INVALID_ARG;
  if (cJSON_IsString(ssid) && cJSON_IsString(password)) {
    err = wifi_manager_set_sta(ssid->valuestring, password->valuestring);
  }
  cJSON_Delete(root);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid wifi config");
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
}

static esp_err_t wifi_clear_handler(httpd_req_t *req) {
  esp_err_t err = wifi_manager_clear_sta();
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "clear failed");
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
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

esp_err_t web_server_start(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.core_id = 0;
  config.task_priority = 5;
  config.stack_size = 8192;
  config.max_open_sockets = 8;
  config.max_uri_handlers = 8;
  config.lru_purge_enable = true;
  config.open_fn = session_open_handler;

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

  const httpd_uri_t wifi_status = {
      .uri = "/api/wifi",
      .method = HTTP_GET,
      .handler = wifi_status_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_status));

  const httpd_uri_t wifi_config = {
      .uri = "/api/wifi",
      .method = HTTP_POST,
      .handler = wifi_config_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_config));

  const httpd_uri_t wifi_clear = {
      .uri = "/api/wifi/clear",
      .method = HTTP_POST,
      .handler = wifi_clear_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_clear));

  const httpd_uri_t webrtc_page = {
      .uri = "/webrtc",
      .method = HTTP_GET,
      .handler = webrtc_app_page_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &webrtc_page));

  const httpd_uri_t webrtc_signal = {
      .uri = "/webrtc/signal",
      .method = HTTP_GET,
      .handler = webrtc_app_signal_get_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &webrtc_signal));

  const httpd_uri_t webrtc_signal_post = {
      .uri = "/webrtc/signal/post",
      .method = HTTP_POST,
      .handler = webrtc_app_signal_post_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &webrtc_signal_post));

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
