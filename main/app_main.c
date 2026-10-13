#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

#include "track_battery.h"
#include "track_control.h"
#include "track_drive.h"
#include "track_watchdog.h"
#include "web_server.h"

#define AP_SSID "HeavyPunch-Track"
#define AP_PASSWORD "12345678"
#define AP_CHANNEL 6
#define AP_MAX_CONNECTIONS 2

static const char *TAG = "heavy_punch";

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

  wifi_config_t wifi_config = {0};
  strlcpy((char *)wifi_config.ap.ssid, AP_SSID, sizeof(wifi_config.ap.ssid));
  strlcpy((char *)wifi_config.ap.password, AP_PASSWORD, sizeof(wifi_config.ap.password));
  wifi_config.ap.ssid_len = strlen(AP_SSID);
  wifi_config.ap.channel = AP_CHANNEL;
  wifi_config.ap.max_connection = AP_MAX_CONNECTIONS;
  wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  wifi_config.ap.pmf_cfg.required = false;

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

  /* Motors are braked before anything else can run. */
  ESP_ERROR_CHECK(track_drive_init());
  ESP_ERROR_CHECK(track_watchdog_init());
  /* Battery sensing is optional: a failure only disables the reading. */
  (void)track_battery_init();

  ESP_ERROR_CHECK(track_control_start());
  ESP_ERROR_CHECK(wifi_init_ap());
  ESP_ERROR_CHECK(web_server_start());
}
