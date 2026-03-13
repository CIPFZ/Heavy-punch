#include "device_identity.h"

#include "esp_mac.h"

#include <stdio.h>
#include <string.h>

void device_identity_get_hostname(char *buf, size_t buf_len)
{
    uint8_t mac[6] = {0};

    if (!buf || buf_len == 0) {
        return;
    }

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, buf_len, "zclaw-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

void device_identity_get_setup_ssid(char *buf, size_t buf_len)
{
    uint8_t mac[6] = {0};

    if (!buf || buf_len == 0) {
        return;
    }

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, buf_len, "zclaw-setup-%02X%02X", mac[4], mac[5]);
}
