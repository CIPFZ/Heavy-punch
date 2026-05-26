#include "media_sys.h"

#include <string.h>

#include "esp_capture.h"
#include "esp_capture_defaults.h"
#include "esp_log.h"

#define CAM_PIN_PWDN 41
#define CAM_PIN_RESET 42
#define CAM_PIN_XCLK 15
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

static const char *TAG = "media_sys";
static esp_capture_handle_t capture_handle;
static esp_capture_video_src_if_t *video_src;

esp_err_t media_sys_init(void) {
  esp_capture_video_dvp_src_cfg_t dvp_cfg = {
      .buf_count = 2,
      .reset_pin = CAM_PIN_RESET,
      .pwr_pin = CAM_PIN_PWDN,
      .data = {CAM_PIN_D0, CAM_PIN_D1, CAM_PIN_D2, CAM_PIN_D3,
               CAM_PIN_D4, CAM_PIN_D5, CAM_PIN_D6, CAM_PIN_D7},
      .vsync_pin = CAM_PIN_VSYNC,
      .href_pin = CAM_PIN_HREF,
      .pclk_pin = CAM_PIN_PCLK,
      .xclk_pin = CAM_PIN_XCLK,
      .xclk_freq = 20000000,
  };
  video_src = esp_capture_new_video_dvp_src(&dvp_cfg);
  if (video_src == NULL) {
    ESP_LOGE(TAG, "failed to create DVP video source");
    return ESP_FAIL;
  }

  esp_capture_cfg_t cfg = {
      .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
      .video_src = video_src,
  };
  int ret = esp_capture_open(&cfg, &capture_handle);
  if (ret != 0 || capture_handle == NULL) {
    ESP_LOGE(TAG, "esp_capture_open failed: %d", ret);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "initialized DVP MJPEG capture source");
  return ESP_OK;
}

esp_err_t media_sys_get_provider(esp_webrtc_media_provider_t *provider) {
  if (provider == NULL || capture_handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  memset(provider, 0, sizeof(*provider));
  provider->capture = capture_handle;
  provider->player = NULL;
  return ESP_OK;
}
