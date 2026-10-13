#include "track_battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#define VBAT_ADC_UNIT ADC_UNIT_1
#define VBAT_ADC_CHANNEL ADC_CHANNEL_3 /* GPIO4 on ESP32-S3 */
#define VBAT_ADC_ATTEN ADC_ATTEN_DB_12
#define VBAT_DIVIDER_R1_OHM 100000
#define VBAT_DIVIDER_R2_OHM 10000
#define VBAT_SAMPLES 8

static const char *TAG = "track_bat";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_ready;

esp_err_t track_battery_init(void) {
  const adc_oneshot_unit_init_cfg_t unit_cfg = {
      .unit_id = VBAT_ADC_UNIT,
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };
  esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "adc unit init failed: %s, battery sensing disabled", esp_err_to_name(err));
    return err;
  }

  const adc_oneshot_chan_cfg_t chan_cfg = {
      .atten = VBAT_ADC_ATTEN,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  err = adc_oneshot_config_channel(s_adc, VBAT_ADC_CHANNEL, &chan_cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "adc channel config failed: %s", esp_err_to_name(err));
    return err;
  }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  const adc_cali_curve_fitting_config_t cali_cfg = {
      .unit_id = VBAT_ADC_UNIT,
      .chan = VBAT_ADC_CHANNEL,
      .atten = VBAT_ADC_ATTEN,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
    s_cali = NULL;
    ESP_LOGW(TAG, "adc calibration unavailable, using linear estimate");
  }
#endif

  s_ready = true;
  ESP_LOGI(TAG, "battery sensing on ADC1 ch%d, divider %d/%d", VBAT_ADC_CHANNEL,
           VBAT_DIVIDER_R1_OHM, VBAT_DIVIDER_R2_OHM);
  return ESP_OK;
}

uint32_t track_battery_read_mv(void) {
  if (!s_ready) {
    return 0;
  }

  int32_t sum = 0;
  int count = 0;
  for (int i = 0; i < VBAT_SAMPLES; i++) {
    int raw = 0;
    if (adc_oneshot_read(s_adc, VBAT_ADC_CHANNEL, &raw) == ESP_OK) {
      sum += raw;
      count++;
    }
  }
  if (count == 0) {
    return 0;
  }
  const int raw_avg = (int)(sum / count);

  int pin_mv = 0;
  if (s_cali != NULL && adc_cali_raw_to_voltage(s_cali, raw_avg, &pin_mv) == ESP_OK) {
    /* calibrated */
  } else {
    /* 12-bit, 12 dB attenuation: roughly 0..3100 mV full scale */
    pin_mv = (raw_avg * 3100) / 4095;
  }

  const int64_t pack_mv =
      (int64_t)pin_mv * (VBAT_DIVIDER_R1_OHM + VBAT_DIVIDER_R2_OHM) / VBAT_DIVIDER_R2_OHM;
  return pack_mv < 0 ? 0 : (uint32_t)pack_mv;
}
