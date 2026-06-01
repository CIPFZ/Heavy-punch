#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"
#include "nvs_flash.h"

#include "camera_tilt.h"
#include "media_sys.h"
#include "track_drive.h"
#include "webrtc_app.h"
#include "web_server.h"
#include "wifi_manager.h"

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
  ESP_ERROR_CHECK(track_drive_init());
  ESP_ERROR_CHECK(camera_tilt_init());
  ESP_ERROR_CHECK(media_sys_init());
  ESP_ERROR_CHECK(wifi_manager_init());
  ESP_ERROR_CHECK(webrtc_app_init());
  ESP_ERROR_CHECK(web_server_start());

  xTaskCreate(drive_task, "drive_task", 3072, NULL, 12, NULL);
}
