#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"

typedef struct {
  bool sta_configured;
  bool sta_connected;
  char sta_ssid[33];
  esp_netif_ip_info_t sta_ip;
} wifi_manager_status_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_set_sta(const char *ssid, const char *password);
esp_err_t wifi_manager_clear_sta(void);
void wifi_manager_get_status(wifi_manager_status_t *status);

#endif
