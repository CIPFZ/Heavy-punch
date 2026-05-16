#include "camera_stream.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

#define CAM_PIN_PWDN 41
#define CAM_PIN_RESET 42
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 9
#define CAM_PIN_SIOC 8
#define CAM_PIN_FLASH 3

#define CAM_PIN_D7 40
#define CAM_PIN_D6 39
#define CAM_PIN_D5 38
#define CAM_PIN_D4 11
#define CAM_PIN_D3 10
#define CAM_PIN_D2 4
#define CAM_PIN_D1 2
#define CAM_PIN_D0 1
#define CAM_PIN_VSYNC 21
#define CAM_PIN_HREF 47
#define CAM_PIN_PCLK 14

#define CAMERA_XCLK_FREQ_HZ 20000000
#define CAMERA_FRAME_SIZE FRAMESIZE_HVGA
#define CAMERA_JPEG_QUALITY 20
#define CAMERA_FB_COUNT 2
#define CAMERA_CAPTURE_INTERVAL_MS 100
#define CAMERA_STREAM_WAIT_MS 120
#define CAMERA_LATEST_FRAME_MAX_BYTES (96 * 1024)
#define CAMERA_FRAME_READY_BIT BIT0
#define CAMERA_STREAM_PORT 81
#define CAMERA_MAX_STREAM_CLIENTS 5

#define OV2640_BANK_SENSOR 0x01
#define OV2640_REG_COM3 0x0C
#define OV2640_REG_COM8 0x13
#define OV2640_COM3_BAND_50HZ 0x3C
#define OV2640_COM8_BANDING_AGC_AEC 0xE5

static const char *TAG = "camera_stream";
static SemaphoreHandle_t latest_frame_mutex;
static SemaphoreHandle_t stream_client_slots;
static EventGroupHandle_t frame_events;
static TaskHandle_t capture_task_handle;
static TaskHandle_t stream_server_task_handle;
static bool camera_ready;

typedef struct {
  uint8_t *buf;
  size_t len;
  uint32_t seq;
  bool ready;
} latest_frame_t;

static latest_frame_t latest_frame;

static const char *frame_size_name(framesize_t frame_size) {
  switch (frame_size) {
    case FRAMESIZE_QVGA:
      return "QVGA 320x240";
    case FRAMESIZE_CIF:
      return "CIF 400x296";
    case FRAMESIZE_HVGA:
      return "HVGA 480x320";
    case FRAMESIZE_VGA:
      return "VGA 640x480";
    default:
      return "custom";
  }
}

static void tune_sensor(sensor_t *sensor) {
  if (sensor == NULL) {
    return;
  }
  sensor->set_framesize(sensor, CAMERA_FRAME_SIZE);
  sensor->set_quality(sensor, CAMERA_JPEG_QUALITY);
  sensor->set_brightness(sensor, 0);
  sensor->set_contrast(sensor, 1);
  sensor->set_saturation(sensor, 0);
  sensor->set_sharpness(sensor, 2);
  sensor->set_vflip(sensor, 1);
  sensor->set_hmirror(sensor, 1);
  sensor->set_dcw(sensor, 1);
  sensor->set_whitebal(sensor, 1);
  sensor->set_awb_gain(sensor, 1);
  sensor->set_exposure_ctrl(sensor, 1);
  sensor->set_gain_ctrl(sensor, 1);
  sensor->set_lenc(sensor, 1);
  sensor->set_reg(sensor, (OV2640_BANK_SENSOR << 8) | OV2640_REG_COM3, 0xFF, OV2640_COM3_BAND_50HZ);
  sensor->set_reg(sensor, (OV2640_BANK_SENSOR << 8) | OV2640_REG_COM8, 0xFF, OV2640_COM8_BANDING_AGC_AEC);
}

static esp_err_t copy_latest_frame(uint8_t *dst, size_t dst_capacity, size_t *out_len, uint32_t *out_seq) {
  if (dst == NULL || out_len == NULL || out_seq == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(latest_frame_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  if (!latest_frame.ready || latest_frame.len == 0) {
    xSemaphoreGive(latest_frame_mutex);
    return ESP_ERR_NOT_FOUND;
  }

  if (latest_frame.len > dst_capacity) {
    xSemaphoreGive(latest_frame_mutex);
    return ESP_ERR_NO_MEM;
  }

  memcpy(dst, latest_frame.buf, latest_frame.len);
  *out_len = latest_frame.len;
  *out_seq = latest_frame.seq;
  xSemaphoreGive(latest_frame_mutex);
  return ESP_OK;
}

static bool wait_for_new_frame(uint32_t last_seq) {
  for (int i = 0; i < 4; ++i) {
    if (xSemaphoreTake(latest_frame_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      const bool ready = latest_frame.ready && latest_frame.seq != last_seq;
      xSemaphoreGive(latest_frame_mutex);
      if (ready) {
        return true;
      }
    }
    xEventGroupWaitBits(frame_events, CAMERA_FRAME_READY_BIT, pdTRUE, pdFALSE,
                        pdMS_TO_TICKS(CAMERA_STREAM_WAIT_MS / 4));
  }
  return true;
}

static esp_err_t socket_send_all(int fd, const void *data, size_t len) {
  const uint8_t *cursor = (const uint8_t *)data;
  while (len > 0) {
    const int sent = send(fd, cursor, len, 0);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ESP_FAIL;
    }
    if (sent == 0) {
      return ESP_FAIL;
    }
    cursor += sent;
    len -= (size_t)sent;
  }
  return ESP_OK;
}

static void camera_capture_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "capture task started on core %d", xPortGetCoreID());

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
      ESP_LOGW(TAG, "frame capture failed");
      vTaskDelay(pdMS_TO_TICKS(CAMERA_CAPTURE_INTERVAL_MS));
      continue;
    }

    if (fb->len <= CAMERA_LATEST_FRAME_MAX_BYTES &&
        xSemaphoreTake(latest_frame_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      memcpy(latest_frame.buf, fb->buf, fb->len);
      latest_frame.len = fb->len;
      latest_frame.seq++;
      latest_frame.ready = true;
      xSemaphoreGive(latest_frame_mutex);
      xEventGroupSetBits(frame_events, CAMERA_FRAME_READY_BIT);
    } else if (fb->len > CAMERA_LATEST_FRAME_MAX_BYTES) {
      ESP_LOGW(TAG, "drop oversized jpeg: %u bytes", (unsigned)fb->len);
    }

    esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_CAPTURE_INTERVAL_MS));
  }
}

esp_err_t camera_stream_capture_handler(httpd_req_t *req) {
  if (!camera_ready) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "camera not ready");
  }

  uint8_t *scratch = heap_caps_malloc(CAMERA_LATEST_FRAME_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (scratch == NULL) {
    scratch = heap_caps_malloc(CAMERA_LATEST_FRAME_MAX_BYTES, MALLOC_CAP_8BIT);
  }
  if (scratch == NULL) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "snapshot buffer unavailable");
  }

  size_t len = 0;
  uint32_t seq = 0;
  esp_err_t err = copy_latest_frame(scratch, CAMERA_LATEST_FRAME_MAX_BYTES, &len, &seq);
  if (err != ESP_OK) {
    free(scratch);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "no frame available");
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  err = httpd_resp_send(req, (const char *)scratch, len);
  free(scratch);
  return err;
}

static void stream_client_task(void *arg) {
  static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
  static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
  static const char *STREAM_HEADERS =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace;boundary=frame\r\n"
      "Cache-Control: no-store\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Connection: close\r\n\r\n";

  const int client_fd = (int)(intptr_t)arg;
  char part_buf[72];

  ESP_LOGI(TAG, "stream client connected");
  uint8_t *scratch = heap_caps_malloc(CAMERA_LATEST_FRAME_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (scratch == NULL) {
    scratch = heap_caps_malloc(CAMERA_LATEST_FRAME_MAX_BYTES, MALLOC_CAP_8BIT);
  }
  if (scratch == NULL) {
    close(client_fd);
    xSemaphoreGive(stream_client_slots);
    vTaskDelete(NULL);
  }

  esp_err_t err = socket_send_all(client_fd, STREAM_HEADERS, strlen(STREAM_HEADERS));
  uint32_t last_seq = 0;

  while (err == ESP_OK) {
    wait_for_new_frame(last_seq);

    size_t frame_len = 0;
    uint32_t seq = 0;
    err = copy_latest_frame(scratch, CAMERA_LATEST_FRAME_MAX_BYTES, &frame_len, &seq);
    if (err != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(20));
      err = ESP_OK;
      continue;
    }
    last_seq = seq;

    const int hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, (unsigned)frame_len);
    err = socket_send_all(client_fd, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (err == ESP_OK) {
      err = socket_send_all(client_fd, part_buf, (size_t)hlen);
    }
    if (err == ESP_OK) {
      err = socket_send_all(client_fd, scratch, frame_len);
    }
    if (err == ESP_OK) {
      err = socket_send_all(client_fd, "\r\n", 2);
    }
  }

  free(scratch);
  shutdown(client_fd, 0);
  close(client_fd);
  xSemaphoreGive(stream_client_slots);
  ESP_LOGI(TAG, "stream client disconnected");
  vTaskDelete(NULL);
}

static void stream_server_task(void *arg) {
  (void)arg;

  const int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listen_fd < 0) {
    ESP_LOGE(TAG, "socket create failed: errno=%d", errno);
    vTaskDelete(NULL);
  }

  const int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(CAMERA_STREAM_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "bind failed: errno=%d", errno);
    close(listen_fd);
    vTaskDelete(NULL);
  }

  if (listen(listen_fd, CAMERA_MAX_STREAM_CLIENTS) != 0) {
    ESP_LOGE(TAG, "listen failed: errno=%d", errno);
    close(listen_fd);
    vTaskDelete(NULL);
  }

  ESP_LOGI(TAG, "started on http://192.168.4.1:%d/stream", CAMERA_STREAM_PORT);

  while (true) {
    struct sockaddr_in source_addr = {0};
    socklen_t addr_len = sizeof(source_addr);
    const int client_fd = accept(listen_fd, (struct sockaddr *)&source_addr, &addr_len);
    if (client_fd < 0) {
      ESP_LOGW(TAG, "accept failed: errno=%d", errno);
      continue;
    }

    if (xSemaphoreTake(stream_client_slots, 0) != pdTRUE) {
      static const char *busy =
          "HTTP/1.1 503 Service Unavailable\r\n"
          "Content-Type: text/plain\r\n"
          "Connection: close\r\n\r\n"
          "stream client limit reached\n";
      socket_send_all(client_fd, busy, strlen(busy));
      close(client_fd);
      continue;
    }

    const int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    BaseType_t ok = xTaskCreatePinnedToCore(stream_client_task, "mjpeg_client", 6144,
                                           (void *)(intptr_t)client_fd, 4, NULL, 1);
    if (ok != pdPASS) {
      close(client_fd);
      xSemaphoreGive(stream_client_slots);
    }
  }
}

esp_err_t camera_stream_init(void) {
  latest_frame_mutex = xSemaphoreCreateMutex();
  stream_client_slots = xSemaphoreCreateCounting(CAMERA_MAX_STREAM_CLIENTS, CAMERA_MAX_STREAM_CLIENTS);
  frame_events = xEventGroupCreate();
  latest_frame.buf = heap_caps_malloc(CAMERA_LATEST_FRAME_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (latest_frame.buf == NULL) {
    latest_frame.buf = heap_caps_malloc(CAMERA_LATEST_FRAME_MAX_BYTES, MALLOC_CAP_8BIT);
  }
  if (latest_frame_mutex == NULL || stream_client_slots == NULL || frame_events == NULL ||
      latest_frame.buf == NULL) {
    return ESP_ERR_NO_MEM;
  }

  const gpio_config_t flash_cfg = {
      .pin_bit_mask = 1ULL << CAM_PIN_FLASH,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&flash_cfg), TAG, "flash gpio config failed");
  gpio_set_level(CAM_PIN_FLASH, 0);

#if CONFIG_SPIRAM
  const camera_fb_location_t fb_location = CAMERA_FB_IN_PSRAM;
  const uint8_t fb_count = CAMERA_FB_COUNT;
#else
  const camera_fb_location_t fb_location = CAMERA_FB_IN_DRAM;
  const uint8_t fb_count = 1;
#endif

  camera_config_t config = {
      .pin_pwdn = CAM_PIN_PWDN,
      .pin_reset = CAM_PIN_RESET,
      .pin_xclk = CAM_PIN_XCLK,
      .pin_sccb_sda = CAM_PIN_SIOD,
      .pin_sccb_scl = CAM_PIN_SIOC,
      .pin_d7 = CAM_PIN_D7,
      .pin_d6 = CAM_PIN_D6,
      .pin_d5 = CAM_PIN_D5,
      .pin_d4 = CAM_PIN_D4,
      .pin_d3 = CAM_PIN_D3,
      .pin_d2 = CAM_PIN_D2,
      .pin_d1 = CAM_PIN_D1,
      .pin_d0 = CAM_PIN_D0,
      .pin_vsync = CAM_PIN_VSYNC,
      .pin_href = CAM_PIN_HREF,
      .pin_pclk = CAM_PIN_PCLK,
      .xclk_freq_hz = CAMERA_XCLK_FREQ_HZ,
      .ledc_timer = LEDC_TIMER_1,
      .ledc_channel = LEDC_CHANNEL_2,
      .pixel_format = PIXFORMAT_JPEG,
      .frame_size = CAMERA_FRAME_SIZE,
      .jpeg_quality = CAMERA_JPEG_QUALITY,
      .fb_count = fb_count,
      .fb_location = fb_location,
      .grab_mode = CAMERA_GRAB_LATEST,
  };

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
    return err;
  }

  tune_sensor(esp_camera_sensor_get());
  camera_ready = true;
  BaseType_t task_ok = xTaskCreatePinnedToCore(camera_capture_task, "camera_capture", 6144, NULL, 6,
                                              &capture_task_handle, 1);
  if (task_ok != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "initialized: %s jpeg quality=%d xclk=%d capture_fps_limit=%d fb_count=%u psram=%u",
           frame_size_name(CAMERA_FRAME_SIZE), CAMERA_JPEG_QUALITY, CAMERA_XCLK_FREQ_HZ,
           1000 / CAMERA_CAPTURE_INTERVAL_MS, (unsigned)fb_count,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return ESP_OK;
}

esp_err_t camera_stream_start(void) {
  BaseType_t ok = xTaskCreatePinnedToCore(stream_server_task, "mjpeg_server", 4096, NULL, 4,
                                         &stream_server_task_handle, 1);
  if (ok != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
