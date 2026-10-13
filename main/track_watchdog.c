#include "track_watchdog.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "track_drive.h"

static const char *TAG = "track_wdt";

static esp_timer_handle_t s_timer;
static atomic_bool s_tripped;

static void on_expire(void *arg) {
  (void)arg;
  track_drive_fault_brake();
  atomic_store(&s_tripped, true);
  ESP_LOGE(TAG, "motion watchdog expired, motors braked");
}

esp_err_t track_watchdog_init(void) {
  const esp_timer_create_args_t args = {
      .callback = on_expire,
      .arg = NULL,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "track_wdt",
      .skip_unhandled_events = true,
  };
  return esp_timer_create(&args, &s_timer);
}

void track_watchdog_feed(uint32_t timeout_ms) {
  if (s_timer == NULL) {
    return;
  }
  /* esp_timer_stop returns an error when the timer is idle; that is fine. */
  (void)esp_timer_stop(s_timer);
  (void)esp_timer_start_once(s_timer, (uint64_t)timeout_ms * 1000ULL);
}

void track_watchdog_stop(void) {
  if (s_timer == NULL) {
    return;
  }
  (void)esp_timer_stop(s_timer);
}

bool track_watchdog_take_trip(void) {
  return atomic_exchange(&s_tripped, false);
}
