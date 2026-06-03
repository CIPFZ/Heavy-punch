#include "esp_check.h"
#include "esp_capture.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "nvs_flash.h"
#include "esp_video_enc_default.h"
#include <string.h>

#include "camera_tilt.h"
#include "media_sys.h"
#include "track_drive.h"
#include "webrtc_app.h"
#include "web_server.h"
#include "wifi_manager.h"

static void media_thread_scheduler(const char *thread_name, media_lib_thread_cfg_t *thread_cfg) {
  if (strcmp(thread_name, "pc_task") == 0) {
    thread_cfg->stack_size = 12 * 1024;
    thread_cfg->priority = 10;
  } else if (strcmp(thread_name, "pc_send") == 0) {
    thread_cfg->stack_size = 8 * 1024;
    thread_cfg->priority = 10;
  } else if (strcmp(thread_name, "venc_0") == 0 || strcmp(thread_name, "vid_enc") == 0) {
    thread_cfg->stack_size = 48 * 1024;
    thread_cfg->priority = 10;
  }
}

static void capture_thread_scheduler(const char *thread_name,
                                     esp_capture_thread_schedule_cfg_t *thread_cfg) {
  if (strcmp(thread_name, "venc_0") == 0 || strcmp(thread_name, "venc_1") == 0) {
    thread_cfg->stack_size = 48 * 1024;
    thread_cfg->priority = 5;
    thread_cfg->core_id = 1;
  }
}

static void drive_task(void *arg) {
  (void)arg;
  while (true) {
    track_drive_update();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(media_lib_add_default_adapter());
  ESP_ERROR_CHECK(esp_video_enc_register_default() == 0 ? ESP_OK : ESP_FAIL);
  media_lib_thread_set_schedule_cb(media_thread_scheduler);
  ESP_ERROR_CHECK(esp_capture_set_thread_scheduler(capture_thread_scheduler) == 0 ? ESP_OK : ESP_FAIL);
  ESP_ERROR_CHECK(track_drive_init());
  ESP_ERROR_CHECK(camera_tilt_init());
  ESP_ERROR_CHECK(media_sys_init());
  ESP_ERROR_CHECK(wifi_manager_init());
  ESP_ERROR_CHECK(webrtc_app_init());
  ESP_ERROR_CHECK(web_server_start());

  xTaskCreate(drive_task, "drive_task", 3072, NULL, 12, NULL);
}
