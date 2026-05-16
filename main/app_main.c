#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

#include "camera_stream.h"
#include "track_drive.h"
#include "web_server.h"

#define AP_SSID "HeavyPunch-Track"
#define AP_PASSWORD "12345678"
#define AP_CHANNEL 6
#define AP_MAX_CONNECTIONS 5

static const char *TAG = "heavy_punch";

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {
  (void)arg;
  if (event_base != WIFI_EVENT) {
    return;
  }

  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    const wifi_event_ap_staconnected_t *event = (const wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "station connected: " MACSTR " aid=%d", MAC2STR(event->mac), event->aid);
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    const wifi_event_ap_stadisconnected_t *event =
        (const wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "station disconnected: " MACSTR " aid=%d", MAC2STR(event->mac), event->aid);
  }
}

static void drive_task(void *arg) {
  (void)arg;
  while (true) {
    track_drive_update();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static esp_err_t wifi_init_ap(void) {
  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
  ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
  esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
  if (ap_netif == NULL) {
    return ESP_FAIL;
  }

  esp_netif_ip_info_t ip_info = {0};
  IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
  IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
  IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
  ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(ap_netif), TAG, "dhcps stop failed");
  ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ip_info), TAG, "set ap ip failed");
  ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif), TAG, "dhcps start failed");

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          wifi_event_handler, NULL, NULL),
                      TAG, "wifi event handler failed");

  wifi_config_t wifi_config = {
      .ap = {
          .ssid = AP_SSID,
          .ssid_len = sizeof(AP_SSID) - 1,
          .channel = AP_CHANNEL,
          .password = AP_PASSWORD,
          .max_connection = AP_MAX_CONNECTIONS,
          .authmode = WIFI_AUTH_WPA_WPA2_PSK,
          .pmf_cfg = {.required = false},
      },
  };

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "wifi mode failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "wifi config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
  ESP_LOGI(TAG, "AP started: ssid=%s password=%s url=http://192.168.4.1", AP_SSID, AP_PASSWORD);
  return ESP_OK;
}

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(track_drive_init());
  ESP_ERROR_CHECK(camera_stream_init());
  ESP_ERROR_CHECK(wifi_init_ap());
  ESP_ERROR_CHECK(web_server_start());
  ESP_ERROR_CHECK(camera_stream_start());

  xTaskCreate(drive_task, "drive_task", 3072, NULL, 12, NULL);
}
