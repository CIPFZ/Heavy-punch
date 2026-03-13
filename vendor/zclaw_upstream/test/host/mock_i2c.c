#include "driver/i2c.h"

#include <stddef.h>

static int s_param_config_calls = 0;
static int s_driver_install_calls = 0;
static int s_driver_delete_calls = 0;
static esp_err_t s_cmd_begin_result = ESP_FAIL;

void i2c_test_reset(void)
{
    s_param_config_calls = 0;
    s_driver_install_calls = 0;
    s_driver_delete_calls = 0;
    s_cmd_begin_result = ESP_FAIL;
}

void i2c_test_set_cmd_begin_result(esp_err_t result)
{
    s_cmd_begin_result = result;
}

int i2c_test_get_param_config_calls(void)
{
    return s_param_config_calls;
}

int i2c_test_get_driver_install_calls(void)
{
    return s_driver_install_calls;
}

int i2c_test_get_driver_delete_calls(void)
{
    return s_driver_delete_calls;
}

esp_err_t i2c_param_config(i2c_port_t i2c_num, const i2c_config_t *i2c_conf)
{
    (void)i2c_num;
    (void)i2c_conf;
    s_param_config_calls++;
    return ESP_OK;
}

esp_err_t i2c_driver_install(i2c_port_t i2c_num, int mode, int slv_rx_buf_len, int slv_tx_buf_len, int intr_alloc_flags)
{
    (void)i2c_num;
    (void)mode;
    (void)slv_rx_buf_len;
    (void)slv_tx_buf_len;
    (void)intr_alloc_flags;
    s_driver_install_calls++;
    return ESP_OK;
}

esp_err_t i2c_driver_delete(i2c_port_t i2c_num)
{
    (void)i2c_num;
    s_driver_delete_calls++;
    return ESP_OK;
}

i2c_cmd_handle_t i2c_cmd_link_create(void)
{
    return (i2c_cmd_handle_t)1;
}

void i2c_cmd_link_delete(i2c_cmd_handle_t cmd_handle)
{
    (void)cmd_handle;
}

esp_err_t i2c_master_start(i2c_cmd_handle_t cmd_handle)
{
    (void)cmd_handle;
    return ESP_OK;
}

esp_err_t i2c_master_write_byte(i2c_cmd_handle_t cmd_handle, uint8_t data, bool ack_en)
{
    (void)cmd_handle;
    (void)data;
    (void)ack_en;
    return ESP_OK;
}

esp_err_t i2c_master_stop(i2c_cmd_handle_t cmd_handle)
{
    (void)cmd_handle;
    return ESP_OK;
}

esp_err_t i2c_master_cmd_begin(i2c_port_t i2c_num, i2c_cmd_handle_t cmd_handle, uint32_t ticks_to_wait)
{
    (void)i2c_num;
    (void)cmd_handle;
    (void)ticks_to_wait;
    return s_cmd_begin_result;
}
