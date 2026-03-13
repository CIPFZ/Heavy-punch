#include "dht11_sensor.h"

#include "config.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

#include <stdio.h>
#include <string.h>

static int s_last_temperature_c = 0;
static int s_last_humidity_pct = 0;
static bool s_last_valid = false;
static int64_t s_last_read_us = 0;
static char s_last_result[128] = {0};
static portMUX_TYPE s_dht11_lock = portMUX_INITIALIZER_UNLOCKED;

#define DHT11_TIMER_INTERVAL_US 2
#define DHT11_DATA_BITS         40
#define DHT11_DATA_BYTES        (DHT11_DATA_BITS / 8)

static esp_err_t dht11_await_pin_state(gpio_num_t pin,
                                       uint32_t timeout_us,
                                       int expected_pin_state,
                                       uint32_t *duration_us)
{
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    for (uint32_t i = 0; i < timeout_us; i += DHT11_TIMER_INTERVAL_US) {
        ets_delay_us(DHT11_TIMER_INTERVAL_US);
        if (gpio_get_level(pin) == expected_pin_state) {
            if (duration_us) {
                *duration_us = i;
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t dht11_fetch_data(uint8_t data[DHT11_DATA_BYTES], char *error, size_t error_len)
{
    uint32_t low_duration_us = 0;
    uint32_t high_duration_us = 0;

    gpio_set_direction((gpio_num_t)DHT11_DATA_PIN, GPIO_MODE_OUTPUT_OD);
    gpio_set_level((gpio_num_t)DHT11_DATA_PIN, 0);
    ets_delay_us(20000);
    gpio_set_level((gpio_num_t)DHT11_DATA_PIN, 1);

    if (dht11_await_pin_state((gpio_num_t)DHT11_DATA_PIN, 40, 0, NULL) != ESP_OK) {
        snprintf(error, error_len, "phase_b_timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (dht11_await_pin_state((gpio_num_t)DHT11_DATA_PIN, 88, 1, NULL) != ESP_OK) {
        snprintf(error, error_len, "phase_c_timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (dht11_await_pin_state((gpio_num_t)DHT11_DATA_PIN, 88, 0, NULL) != ESP_OK) {
        snprintf(error, error_len, "phase_d_timeout");
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < DHT11_DATA_BITS; i++) {
        if (dht11_await_pin_state((gpio_num_t)DHT11_DATA_PIN, 65, 1, &low_duration_us) != ESP_OK) {
            snprintf(error, error_len, "low_bit_timeout");
            return ESP_ERR_TIMEOUT;
        }
        if (dht11_await_pin_state((gpio_num_t)DHT11_DATA_PIN, 75, 0, &high_duration_us) != ESP_OK) {
            snprintf(error, error_len, "high_bit_timeout");
            return ESP_ERR_TIMEOUT;
        }

        uint8_t byte_index = (uint8_t)(i / 8);
        uint8_t bit_index = (uint8_t)(i % 8);
        if (bit_index == 0) {
            data[byte_index] = 0;
        }
        data[byte_index] |= (uint8_t)((high_duration_us > low_duration_us) << (7 - bit_index));
    }

    return ESP_OK;
}

static bool dht11_read_raw(int *temperature_c, int *humidity_pct, char *error, size_t error_len)
{
    uint8_t data[DHT11_DATA_BYTES] = {0};
    esp_err_t result;

    if (!temperature_c || !humidity_pct) {
        if (error && error_len > 0) {
            snprintf(error, error_len, "invalid_args");
        }
        return false;
    }

    gpio_set_pull_mode((gpio_num_t)DHT11_DATA_PIN, GPIO_PULLUP_ONLY);
    gpio_set_direction((gpio_num_t)DHT11_DATA_PIN, GPIO_MODE_OUTPUT_OD);
    gpio_set_level((gpio_num_t)DHT11_DATA_PIN, 1);

    taskENTER_CRITICAL(&s_dht11_lock);
    result = dht11_fetch_data(data, error, error_len);
    taskEXIT_CRITICAL(&s_dht11_lock);

    gpio_set_direction((gpio_num_t)DHT11_DATA_PIN, GPIO_MODE_OUTPUT_OD);
    gpio_set_level((gpio_num_t)DHT11_DATA_PIN, 1);

    if (result != ESP_OK) {
        return false;
    }

    if (data[4] != ((uint8_t)(data[0] + data[1] + data[2] + data[3]))) {
        snprintf(error, error_len, "checksum_failed");
        return false;
    }

    *humidity_pct = (int)data[0];
    *temperature_c = (int)data[2];
    return true;
}

bool dht11_read(char *result, size_t result_len)
{
    int64_t now_us = esp_timer_get_time();
    int temperature_c = 0;
    int humidity_pct = 0;
    char error[64] = {0};

    if (!result || result_len == 0) {
        return false;
    }

    if (s_last_valid && s_last_read_us > 0 &&
        ((now_us - s_last_read_us) / 1000) < DHT11_CACHE_MS) {
        snprintf(result, result_len, "%s | Cache: hit", s_last_result);
        return true;
    }

    if (!dht11_read_raw(&temperature_c, &humidity_pct, error, sizeof(error))) {
        snprintf(result, result_len,
                 "Error: DHT11 read failed on GPIO%d (%s)",
                 DHT11_DATA_PIN,
                 error[0] ? error : "unknown");
        s_last_valid = false;
        s_last_read_us = now_us;
        return false;
    }

    s_last_temperature_c = temperature_c;
    s_last_humidity_pct = humidity_pct;
    s_last_valid = true;
    s_last_read_us = now_us;
    snprintf(s_last_result, sizeof(s_last_result),
             "DHT11: %d C | %d%% RH | Pin: GPIO%d",
             s_last_temperature_c,
             s_last_humidity_pct,
             DHT11_DATA_PIN);
    snprintf(result, result_len, "%s | Cache: fresh", s_last_result);
    return true;
}
