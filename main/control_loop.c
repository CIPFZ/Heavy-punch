#include "control_loop.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "camera_tilt.h"
#include "track_drive.h"

#define CONTROL_LOOP_INTERVAL_MS 5
#define CONTROL_STOP_BIT BIT0

static const char *TAG = "control_loop";
static QueueHandle_t tracks_queue;
static QueueHandle_t tilt_queue;
static EventGroupHandle_t control_events;
static TaskHandle_t control_task_handle;
static bool initialized;

static void control_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "control_task started on core %d", xPortGetCoreID());

  while (true) {
    const EventBits_t bits = xEventGroupGetBits(control_events);
    if ((bits & CONTROL_STOP_BIT) != 0) {
      xQueueReset(tracks_queue);
      track_drive_stop();
      xEventGroupClearBits(control_events, CONTROL_STOP_BIT);
    }

    control_tracks_t tracks = {0};
    while (xQueueReceive(tracks_queue, &tracks, 0) == pdTRUE) {
      track_drive_set_percent(tracks.left, tracks.right);
    }

    int16_t tilt = 0;
    while (xQueueReceive(tilt_queue, &tilt, 0) == pdTRUE) {
      camera_tilt_set_percent(tilt);
    }

    track_drive_update();
    vTaskDelay(pdMS_TO_TICKS(CONTROL_LOOP_INTERVAL_MS));
  }
}

esp_err_t control_loop_init(void) {
  if (initialized) {
    return ESP_OK;
  }

  tracks_queue = xQueueCreate(1, sizeof(control_tracks_t));
  tilt_queue = xQueueCreate(1, sizeof(int16_t));
  control_events = xEventGroupCreate();
  if (tracks_queue == NULL || tilt_queue == NULL || control_events == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ESP_RETURN_ON_ERROR(track_drive_init(), TAG, "track drive init failed");
  ESP_RETURN_ON_ERROR(camera_tilt_init(), TAG, "camera tilt init failed");

  initialized = true;
  return ESP_OK;
}

esp_err_t control_loop_start(void) {
  if (!initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (control_task_handle != NULL) {
    return ESP_OK;
  }

  BaseType_t ok = xTaskCreatePinnedToCore(control_task, "control_task", 4096, NULL, 12,
                                         &control_task_handle, 1);
  if (ok != pdPASS) {
    control_task_handle = NULL;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

bool control_loop_submit_tracks(int16_t left, int16_t right) {
  if (tracks_queue == NULL) {
    return false;
  }

  const control_tracks_t tracks = {
      .left = left,
      .right = right,
  };
  return xQueueOverwrite(tracks_queue, &tracks) == pdTRUE;
}

bool control_loop_submit_tilt(int16_t percent) {
  if (tilt_queue == NULL) {
    return false;
  }

  return xQueueOverwrite(tilt_queue, &percent) == pdTRUE;
}

void control_loop_submit_stop(void) {
  if (control_events != NULL) {
    xEventGroupSetBits(control_events, CONTROL_STOP_BIT);
  }
}
