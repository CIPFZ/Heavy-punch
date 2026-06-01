#include "wifi_manager.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"

#define AP_SSID "HeavyPunch-Track"
#define AP_PASSWORD "12345678"
#define AP_CHANNEL 6
#define AP_MAX_CONNECTIONS 5
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_STA_SSID "sta_ssid"
#define WIFI_NVS_STA_PASS "sta_pass"

static const char *TAG = "wifi_manager";
static bool sta_configured;
static bool sta_connected;
static char sta_ssid[33];
static char sta_password[65];
static esp_netif_ip_info_t sta_ip;

static esp_err_t load_sta_config(void) {
  nvs_handle_t nvs = 0;
  esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    sta_configured = false;
    return ESP_OK;
  }
  ESP_RETURN_ON_ERROR(err, TAG, "open wifi nvs failed");

  size_t ssid_len = sizeof(sta_ssid);
  size_t pass_len = sizeof(sta_password);
  err = nvs_get_str(nvs, WIFI_NVS_STA_SSID, sta_ssid, &ssid_len);
  if (err == ESP_OK) {
    err = nvs_get_str(nvs, WIFI_NVS_STA_PASS, sta_password, &pass_len);
  }
  nvs_close(nvs);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    sta_configured = false;
    sta_ssid[0] = '\0';
    sta_password[0] = '\0';
    return ESP_OK;
  }
  ESP_RETURN_ON_ERROR(err, TAG, "read wifi nvs failed");
  sta_configured = sta_ssid[0] != '\0';
  return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {
  (void)arg;
  if (event_base == WIFI_EVENT) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
      const wifi_event_ap_staconnected_t *event = (const wifi_event_ap_staconnected_t *)event_data;
      ESP_LOGI(TAG, "AP station connected: " MACSTR " aid=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
      const wifi_event_ap_stadisconnected_t *event =
          (const wifi_event_ap_stadisconnected_t *)event_data;
      ESP_LOGI(TAG, "AP station disconnected: " MACSTR " aid=%d", MAC2STR(event->mac),
               event->aid);
    } else if (event_id == WIFI_EVENT_STA_START && sta_configured) {
      ESP_LOGI(TAG, "connecting STA to ssid=%s", sta_ssid);
      esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
      if (sta_connected) {
        ESP_LOGW(TAG, "STA disconnected");
      }
      sta_connected = false;
      memset(&sta_ip, 0, sizeof(sta_ip));
      if (sta_configured) {
        esp_wifi_connect();
      }
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    sta_connected = true;
    sta_ip = event->ip_info;
    ESP_LOGI(TAG, "STA connected: ssid=%s url=http://" IPSTR, sta_ssid, IP2STR(&sta_ip.ip));
  }
}

static esp_err_t configure_ap(void) {
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

  wifi_config_t ap_config = {
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
  return esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

static esp_err_t configure_sta(void) {
  if (!sta_configured) {
    return ESP_OK;
  }
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  if (sta_netif == NULL) {
    return ESP_FAIL;
  }

  wifi_config_t sta_config = {
      .sta = {
          .scan_method = WIFI_ALL_CHANNEL_SCAN,
          .failure_retry_cnt = 3,
          .threshold.authmode = WIFI_AUTH_WPA2_PSK,
          .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
      },
  };
  strlcpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid));
  strlcpy((char *)sta_config.sta.password, sta_password, sizeof(sta_config.sta.password));
  return esp_wifi_set_config(WIFI_IF_STA, &sta_config);
}

esp_err_t wifi_manager_init(void) {
  ESP_RETURN_ON_ERROR(load_sta_config(), TAG, "load STA config failed");
  ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
  ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          wifi_event_handler, NULL, NULL),
                      TAG, "wifi event handler failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                          wifi_event_handler, NULL, NULL),
                      TAG, "ip event handler failed");

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(sta_configured ? WIFI_MODE_APSTA : WIFI_MODE_AP), TAG,
                      "wifi mode failed");
  ESP_RETURN_ON_ERROR(configure_ap(), TAG, "AP config failed");
  ESP_RETURN_ON_ERROR(configure_sta(), TAG, "STA config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

  ESP_LOGI(TAG, "AP started: ssid=%s password=%s url=http://192.168.4.1", AP_SSID,
           AP_PASSWORD);
  if (!sta_configured) {
    ESP_LOGI(TAG, "STA not configured; AP-only mode");
  }
  return ESP_OK;
}

esp_err_t wifi_manager_set_sta(const char *ssid, const char *password) {
  if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32 || password == NULL ||
      strlen(password) > 64) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs = 0;
  ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG,
                      "open wifi nvs failed");
  esp_err_t err = nvs_set_str(nvs, WIFI_NVS_STA_SSID, ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, WIFI_NVS_STA_PASS, password);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  if (err != ESP_OK) {
    return err;
  }

  strlcpy(sta_ssid, ssid, sizeof(sta_ssid));
  strlcpy(sta_password, password, sizeof(sta_password));
  sta_configured = true;
  ESP_LOGI(TAG, "saved STA credentials for ssid=%s; reboot to connect in AP+STA mode", sta_ssid);
  return ESP_OK;
}

esp_err_t wifi_manager_clear_sta(void) {
  nvs_handle_t nvs = 0;
  esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  ESP_RETURN_ON_ERROR(err, TAG, "open wifi nvs failed");
  nvs_erase_key(nvs, WIFI_NVS_STA_SSID);
  nvs_erase_key(nvs, WIFI_NVS_STA_PASS);
  err = nvs_commit(nvs);
  nvs_close(nvs);
  sta_configured = false;
  sta_connected = false;
  sta_ssid[0] = '\0';
  sta_password[0] = '\0';
  memset(&sta_ip, 0, sizeof(sta_ip));
  return err;
}

void wifi_manager_get_status(wifi_manager_status_t *status) {
  if (status == NULL) {
    return;
  }
  memset(status, 0, sizeof(*status));
  status->sta_configured = sta_configured;
  status->sta_connected = sta_connected;
  strlcpy(status->sta_ssid, sta_ssid, sizeof(status->sta_ssid));
  status->sta_ip = sta_ip;
}
