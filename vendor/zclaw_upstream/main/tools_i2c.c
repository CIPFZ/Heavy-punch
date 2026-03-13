#include "tools_handlers.h"
#include "config.h"
#include "gpio_policy.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I2C_SCAN_PORT                I2C_NUM_0
#define I2C_SCAN_ADDR_FIRST          0x03
#define I2C_SCAN_ADDR_LAST           0x77
#define I2C_SCAN_DEFAULT_FREQ_HZ     100000
#define I2C_SCAN_MIN_FREQ_HZ         10000
#define I2C_SCAN_MAX_FREQ_HZ         1000000
#define I2C_SCAN_ADDR_TIMEOUT_MS     25

static bool validate_scan_pin(const char *field_name, int pin, char *result, size_t result_len)
{
    if (!gpio_policy_pin_is_allowed(pin)) {
        if (gpio_policy_pin_forbidden_hint(pin, result, result_len)) {
            return false;
        }
        if (GPIO_ALLOWED_PINS_CSV[0] != '\0') {
            snprintf(result, result_len, "Error: %s pin %d is not in allowed list", field_name, pin);
        } else {
            snprintf(result, result_len, "Error: %s pin must be %d-%d", field_name, GPIO_MIN_PIN, GPIO_MAX_PIN);
        }
        return false;
    }
    return true;
}

bool tools_i2c_scan_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *sda_pin_json = cJSON_GetObjectItem(input, "sda_pin");
    cJSON *scl_pin_json = cJSON_GetObjectItem(input, "scl_pin");
    cJSON *freq_json = cJSON_GetObjectItem(input, "frequency_hz");

    if (!sda_pin_json || !cJSON_IsNumber(sda_pin_json)) {
        snprintf(result, result_len, "Error: 'sda_pin' required (number)");
        return false;
    }
    if (!scl_pin_json || !cJSON_IsNumber(scl_pin_json)) {
        snprintf(result, result_len, "Error: 'scl_pin' required (number)");
        return false;
    }

    int sda_pin = sda_pin_json->valueint;
    int scl_pin = scl_pin_json->valueint;
    int frequency_hz = I2C_SCAN_DEFAULT_FREQ_HZ;

    if (freq_json) {
        if (!cJSON_IsNumber(freq_json)) {
            snprintf(result, result_len, "Error: 'frequency_hz' must be a number");
            return false;
        }
        frequency_hz = freq_json->valueint;
    }

    if (sda_pin == scl_pin) {
        snprintf(result, result_len, "Error: SDA and SCL must be different pins");
        return false;
    }
    if (!validate_scan_pin("SDA", sda_pin, result, result_len)) {
        return false;
    }
    if (!validate_scan_pin("SCL", scl_pin, result, result_len)) {
        return false;
    }
    if (frequency_hz < I2C_SCAN_MIN_FREQ_HZ || frequency_hz > I2C_SCAN_MAX_FREQ_HZ) {
        snprintf(
            result,
            result_len,
            "Error: frequency_hz must be %d-%d",
            I2C_SCAN_MIN_FREQ_HZ,
            I2C_SCAN_MAX_FREQ_HZ
        );
        return false;
    }

    // Clear any previous configuration on this port so scans are repeatable.
    i2c_driver_delete(I2C_SCAN_PORT);

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = frequency_hz,
    };

    esp_err_t err = i2c_param_config(I2C_SCAN_PORT, &conf);
    if (err != ESP_OK) {
        snprintf(result, result_len, "Error: i2c_param_config failed (%s)", esp_err_to_name(err));
        return false;
    }

    err = i2c_driver_install(I2C_SCAN_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        snprintf(result, result_len, "Error: i2c_driver_install failed (%s)", esp_err_to_name(err));
        return false;
    }

    uint8_t found_addresses[I2C_SCAN_ADDR_LAST - I2C_SCAN_ADDR_FIRST + 1];
    int found_count = 0;

    for (int addr = I2C_SCAN_ADDR_FIRST; addr <= I2C_SCAN_ADDR_LAST; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (!cmd) {
            i2c_driver_delete(I2C_SCAN_PORT);
            snprintf(result, result_len, "Error: out of memory during I2C scan");
            return false;
        }

        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);

        err = i2c_master_cmd_begin(
            I2C_SCAN_PORT,
            cmd,
            pdMS_TO_TICKS(I2C_SCAN_ADDR_TIMEOUT_MS)
        );
        i2c_cmd_link_delete(cmd);

        if (err == ESP_OK && found_count < (int)(sizeof(found_addresses))) {
            found_addresses[found_count++] = (uint8_t)addr;
        }
    }

    i2c_driver_delete(I2C_SCAN_PORT);

    if (found_count == 0) {
        snprintf(
            result,
            result_len,
            "No I2C devices found on SDA=%d SCL=%d @ %d Hz",
            sda_pin,
            scl_pin,
            frequency_hz
        );
        return true;
    }

    size_t offset = 0;
    int written = snprintf(
        result,
        result_len,
        "Found %d I2C device(s) on SDA=%d SCL=%d @ %d Hz: ",
        found_count,
        sda_pin,
        scl_pin,
        frequency_hz
    );
    if (written < 0) {
        snprintf(result, result_len, "Found %d I2C device(s)", found_count);
        return true;
    }
    offset = (size_t)written < result_len ? (size_t)written : result_len - 1;

    int listed = 0;
    for (int i = 0; i < found_count; i++) {
        if (offset >= result_len) {
            break;
        }

        written = snprintf(
            result + offset,
            result_len - offset,
            listed == 0 ? "0x%02X" : ", 0x%02X",
            found_addresses[i]
        );
        if (written < 0 || (size_t)written >= result_len - offset) {
            break;
        }
        offset += (size_t)written;
        listed++;
    }

    if (listed < found_count && offset < result_len) {
        snprintf(result + offset, result_len - offset, " ... (+%d more)", found_count - listed);
    }

    return true;
}

#ifdef TEST_BUILD
bool tools_i2c_test_pin_is_allowed_for_esp32_target(int pin, const char *csv, int min_pin, int max_pin)
{
    return gpio_policy_test_pin_is_allowed(pin, csv, min_pin, max_pin, true, true);
}
#endif
