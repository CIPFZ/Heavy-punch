#include "oled_display.h"

#include "config.h"
#include "device_identity.h"
#include "memory.h"
#include "nvs_keys.h"

#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define OLED_I2C_PORT I2C_NUM_0
#define OLED_I2C_SDA 21
#define OLED_I2C_SCL 22
#define OLED_I2C_FREQ_HZ 400000
#define OLED_ADDR_PRIMARY 0x3C
#define OLED_ADDR_SECONDARY 0x3D
#define OLED_WIDTH 128
#define OLED_HEIGHT 32
#define OLED_PAGE_COUNT (OLED_HEIGHT / 8)
#define OLED_BUF_SIZE (OLED_WIDTH * OLED_PAGE_COUNT)
#define OLED_TASK_STACK_SIZE 4096
#define OLED_TASK_PERIOD_MS 1500
#define OLED_LINE_MAX_CHARS 21

static const char *TAG = "oled_display";

static TaskHandle_t s_task = NULL;
static bool s_available = false;
static bool s_driver_ready = false;
static uint8_t s_addr = 0;
static oled_display_mode_t s_mode = OLED_DISPLAY_MODE_BOOT;
static uint8_t s_framebuffer[OLED_BUF_SIZE];
static SemaphoreHandle_t s_state_mutex = NULL;
static TickType_t s_message_until_ticks = 0;
static char s_message_lines[OLED_PAGE_COUNT][OLED_LINE_MAX_CHARS + 1];

static const uint8_t k_font_digits[10][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
};

static const uint8_t k_font_upper[26][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

static const uint8_t *oled_font_for_char(char c)
{
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const uint8_t hyphen[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};

    if (c >= '0' && c <= '9') {
        return k_font_digits[c - '0'];
    }
    if (c >= 'a' && c <= 'z') {
        c = (char)toupper((unsigned char)c);
    }
    if (c >= 'A' && c <= 'Z') {
        return k_font_upper[c - 'A'];
    }
    switch (c) {
    case '-':
        return hyphen;
    case '.':
        return dot;
    case ':':
        return colon;
    case '/':
        return slash;
    default:
        return blank;
    }
}

static esp_err_t oled_write_bytes(bool is_data, const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd;
    esp_err_t err;
    uint8_t control = is_data ? 0x40 : 0x00;

    cmd = i2c_cmd_link_create();
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, control, true);
    i2c_master_write(cmd, (uint8_t *)data, len, true);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(OLED_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t oled_write_cmd(uint8_t cmd)
{
    return oled_write_bytes(false, &cmd, 1);
}

static bool oled_probe_address(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    esp_err_t err;
    if (!cmd) {
        return false;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(OLED_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static esp_err_t oled_detect_address(void)
{
    if (oled_probe_address(OLED_ADDR_PRIMARY)) {
        s_addr = OLED_ADDR_PRIMARY;
        return ESP_OK;
    }
    if (oled_probe_address(OLED_ADDR_SECONDARY)) {
        s_addr = OLED_ADDR_SECONDARY;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t oled_init_bus(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = OLED_I2C_SDA,
        .scl_io_num = OLED_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = OLED_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    i2c_driver_delete(OLED_I2C_PORT);
    ESP_RETURN_ON_ERROR(i2c_param_config(OLED_I2C_PORT, &conf), TAG, "i2c_param_config");
    ESP_RETURN_ON_ERROR(i2c_driver_install(OLED_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0), TAG,
                        "i2c_driver_install");
    s_driver_ready = true;
    return ESP_OK;
}

static esp_err_t oled_init_panel(void)
{
    static const uint8_t init_seq[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x1F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
        0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x02, 0x81, 0x8F, 0xD9, 0xF1,
        0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };
    size_t i;
    for (i = 0; i < sizeof(init_seq); i++) {
        ESP_RETURN_ON_ERROR(oled_write_cmd(init_seq[i]), TAG, "oled init cmd");
    }
    return ESP_OK;
}

static void oled_clear_buffer(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

static void oled_draw_text_line_centered(int page, const char *text)
{
    int len;
    int col;
    int x;

    if (!text) {
        return;
    }

    len = (int)strlen(text);
    if (len <= 0) {
        return;
    }
    if (len > OLED_LINE_MAX_CHARS) {
        len = OLED_LINE_MAX_CHARS;
    }

    col = len * 6 - 1;
    if (col < 0) {
        col = 0;
    }
    x = (OLED_WIDTH - col) / 2;
    if (x < 0) {
        x = 0;
    }

    while (*text && len-- > 0 && x <= OLED_WIDTH - 6) {
        const uint8_t *glyph = oled_font_for_char(*text++);
        int i;
        for (i = 0; i < 5; i++) {
            s_framebuffer[page * OLED_WIDTH + x + i] = glyph[i];
        }
        if (x + 5 < OLED_WIDTH) {
            s_framebuffer[page * OLED_WIDTH + x + 5] = 0x00;
        }
        x += 6;
    }
}

static esp_err_t oled_flush(void)
{
    int page;
    for (page = 0; page < OLED_PAGE_COUNT; page++) {
        uint8_t page_cmds[] = {
            (uint8_t)(0xB0 + page),
            0x00,
            0x10,
        };
        ESP_RETURN_ON_ERROR(oled_write_bytes(false, page_cmds, sizeof(page_cmds)), TAG, "set page");
        ESP_RETURN_ON_ERROR(oled_write_bytes(true, &s_framebuffer[page * OLED_WIDTH], OLED_WIDTH), TAG,
                            "write page");
    }
    return ESP_OK;
}

static void oled_get_ip(char *buf, size_t len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;

    if (!buf || len == 0) {
        return;
    }
    buf[0] = '\0';

    if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        snprintf(buf, len, "NO LINK");
        return;
    }

    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
}

static bool oled_has_api_key(void)
{
    char api_key[LLM_API_KEY_BUF_SIZE] = {0};
    return memory_get(NVS_KEY_API_KEY, api_key, sizeof(api_key)) && api_key[0] != '\0';
}

static void oled_compose_lines(char lines[OLED_PAGE_COUNT][OLED_LINE_MAX_CHARS + 1])
{
    char ip[20] = {0};
    char ssid[64] = {0};
    char setup_ssid[32] = {0};
    TickType_t now = xTaskGetTickCount();
    int page_index = ((int)(now / pdMS_TO_TICKS(5000))) & 1;

    memset(lines, 0, sizeof(char) * OLED_PAGE_COUNT * (OLED_LINE_MAX_CHARS + 1));

    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (s_message_until_ticks != 0 && now < s_message_until_ticks) {
            memcpy(lines, s_message_lines, sizeof(s_message_lines));
            xSemaphoreGive(s_state_mutex);
            return;
        }
        xSemaphoreGive(s_state_mutex);
    }

    oled_get_ip(ip, sizeof(ip));
    memory_get(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid));
    device_identity_get_setup_ssid(setup_ssid, sizeof(setup_ssid));

    switch (s_mode) {
    case OLED_DISPLAY_MODE_SETUP: {
        if (page_index == 0) {
            snprintf(lines[0], sizeof(lines[0]), "SETUP AP");
            snprintf(lines[1], sizeof(lines[1]), "%.21s", setup_ssid);
            snprintf(lines[2], sizeof(lines[2]), "OPEN");
            snprintf(lines[3], sizeof(lines[3]), "192.168.4.1");
        } else {
            snprintf(lines[0], sizeof(lines[0]), "STEP1 JOIN AP");
            snprintf(lines[1], sizeof(lines[1]), "STEP2 OPEN URL");
            snprintf(lines[2], sizeof(lines[2]), "STEP3 SAVE WIFI");
            snprintf(lines[3], sizeof(lines[3]), "API %s", oled_has_api_key() ? "OK" : "MISS");
        }
        break;
    }
    case OLED_DISPLAY_MODE_SAFE:
        if (page_index == 0) {
            snprintf(lines[0], sizeof(lines[0]), "SAFE MODE");
            snprintf(lines[1], sizeof(lines[1]), "OPEN SETUP AP");
            snprintf(lines[2], sizeof(lines[2]), "192.168.4.1");
            snprintf(lines[3], sizeof(lines[3]), "BOOT FAILURES");
        } else {
            snprintf(lines[0], sizeof(lines[0]), "RECOVERY");
            snprintf(lines[1], sizeof(lines[1]), "HOLD BOOT 3S");
            snprintf(lines[2], sizeof(lines[2]), "FOR SETUP AP");
            snprintf(lines[3], sizeof(lines[3]), "%.21s", setup_ssid);
        }
        break;
    case OLED_DISPLAY_MODE_CHAT:
        if (page_index == 0) {
            snprintf(lines[0], sizeof(lines[0]), "PHONE URL");
            snprintf(lines[1], sizeof(lines[1]), "%.21s", ip);
            snprintf(lines[2], sizeof(lines[2]), "WIFI %s", strcmp(ip, "NO LINK") == 0 ? "WAIT" : "OK");
            snprintf(lines[3], sizeof(lines[3]), "API %s", oled_has_api_key() ? "OK" : "MISS");
        } else {
            snprintf(lines[0], sizeof(lines[0]), "WIFI");
            snprintf(lines[1], sizeof(lines[1]), "%.21s", ssid[0] ? ssid : "NOT SET");
            snprintf(lines[2], sizeof(lines[2]), "BOOT 3S SETUP");
            snprintf(lines[3], sizeof(lines[3]), "%.21s", ip);
        }
        break;
    case OLED_DISPLAY_MODE_BOOT:
    default:
        snprintf(lines[0], sizeof(lines[0]), "ZCLAW");
        snprintf(lines[1], sizeof(lines[1]), "STARTING");
        snprintf(lines[2], sizeof(lines[2]), "OLED + WIFI");
        snprintf(lines[3], sizeof(lines[3]), "WAIT...");
        break;
    }
}

static void oled_task(void *arg)
{
    (void)arg;
    while (1) {
        char lines[OLED_PAGE_COUNT][OLED_LINE_MAX_CHARS + 1];
        oled_compose_lines(lines);
        oled_clear_buffer();
        oled_draw_text_line_centered(0, lines[0]);
        oled_draw_text_line_centered(1, lines[1]);
        oled_draw_text_line_centered(2, lines[2]);
        oled_draw_text_line_centered(3, lines[3]);
        if (oled_flush() != ESP_OK) {
            ESP_LOGW(TAG, "OLED flush failed");
        }
        vTaskDelay(pdMS_TO_TICKS(OLED_TASK_PERIOD_MS));
    }
}

esp_err_t oled_display_start(oled_display_mode_t mode)
{
    esp_err_t err;

    s_mode = mode;
    if (!s_state_mutex) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (!s_state_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_task) {
        return ESP_OK;
    }

    err = oled_init_bus();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED I2C init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = oled_detect_address();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED not detected on SDA=%d SCL=%d", OLED_I2C_SDA, OLED_I2C_SCL);
        return err;
    }

    err = oled_init_panel();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_available = true;
    ESP_LOGI(TAG, "OLED detected at 0x%02X on SDA=%d SCL=%d", s_addr, OLED_I2C_SDA, OLED_I2C_SCL);

    if (xTaskCreate(oled_task, "oled_task", OLED_TASK_STACK_SIZE, NULL, 1, &s_task) != pdPASS) {
        s_available = false;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void oled_display_set_mode(oled_display_mode_t mode)
{
    s_mode = mode;
}

void oled_display_show_message(const char *line1,
                               const char *line2,
                               const char *line3,
                               const char *line4,
                               uint32_t duration_ms)
{
    const char *src[OLED_PAGE_COUNT] = {line1, line2, line3, line4};
    int i;

    if (!s_state_mutex) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (i = 0; i < OLED_PAGE_COUNT; i++) {
        snprintf(s_message_lines[i], sizeof(s_message_lines[i]), "%.21s", src[i] ? src[i] : "");
    }
    s_message_until_ticks = duration_ms > 0
        ? (xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms))
        : 0;
    xSemaphoreGive(s_state_mutex);
}

bool oled_display_is_available(void)
{
    return s_available && s_driver_ready;
}
