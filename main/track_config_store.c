#include "track_config_store.h"

#include "esp_log.h"
#include "nvs.h"

#define NVS_NAMESPACE "track"
#define NVS_KEY "cfg"

static const char *TAG = "track_cfg";

esp_err_t track_config_load(track_config_t *cfg) {
  track_config_defaults(cfg);

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    ESP_LOGI(TAG, "no stored config (%s), using defaults", esp_err_to_name(err));
    return err;
  }

  track_config_t stored;
  size_t len = sizeof(stored);
  err = nvs_get_blob(handle, NVS_KEY, &stored, &len);
  nvs_close(handle);

  if (err != ESP_OK) {
    ESP_LOGI(TAG, "no stored config (%s), using defaults", esp_err_to_name(err));
    return err;
  }
  if (len != sizeof(stored) || !track_config_valid(&stored)) {
    ESP_LOGW(TAG, "stored config invalid or wrong version, using defaults");
    return ESP_ERR_INVALID_VERSION;
  }

  *cfg = stored;
  ESP_LOGI(TAG, "loaded config from nvs");
  return ESP_OK;
}

esp_err_t track_config_save(const track_config_t *cfg) {
  if (!track_config_valid(cfg)) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_blob(handle, NVS_KEY, cfg, sizeof(*cfg));
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "config saved");
  } else {
    ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
  }
  return err;
}
