#include "track_control.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "track_battery.h"
#include "track_config.h"
#include "track_config_store.h"
#include "track_drive.h"
#include "track_safety.h"
#include "track_watchdog.h"
#include "web_server.h"

#define CONTROL_TASK_STACK 4096
#define CONTROL_TASK_PRIORITY 12
#define CONTROL_QUEUE_LEN 16
#define CONTROL_IDLE_WAIT_MS 5

#define STATUS_INTERVAL_MS 100
#define BATTERY_INTERVAL_MS 500
#define BATTERY_FAULT_SAMPLES 4

/* Hardware watchdog runs at a multiple of the software timeout so it only
 * trips when the control task itself has stopped processing. */
#define WATCHDOG_MULTIPLIER 3
#define WATCHDOG_MIN_MS 600

static const char *TAG = "track_ctl";

typedef enum {
  MSG_COMMAND = 0,
  MSG_CONTROLLER_LOST,
} msg_kind_t;

typedef struct {
  msg_kind_t kind;
  int fd;
  track_command_t cmd;
} ctrl_msg_t;

static QueueHandle_t s_queue;
static track_config_t s_cfg;
static track_safety_t s_safety;
static int64_t s_last_frame_us;
static int64_t s_last_update_us;
static int64_t s_last_status_us;
static int64_t s_last_battery_us;
static uint32_t s_battery_mv;
static int s_battery_low_count;
static const char *s_fault_reason = "";

static uint32_t watchdog_period_ms(void) {
  const uint32_t ms = (uint32_t)s_cfg.timeout_ms * WATCHDOG_MULTIPLIER;
  return ms < WATCHDOG_MIN_MS ? WATCHDOG_MIN_MS : ms;
}

static void apply_action(track_action_t action, int16_t left_pct, int16_t right_pct) {
  switch (action) {
    case TRACK_ACTION_BRAKE:
      track_drive_brake();
      break;
    case TRACK_ACTION_DRIVE:
      track_drive_set_target(track_percent_to_pwm(left_pct, &s_cfg),
                             track_percent_to_pwm(right_pct, &s_cfg));
      break;
    case TRACK_ACTION_NONE:
    case TRACK_ACTION_REJECT:
      break;
  }

  if (track_safety_is_armed(&s_safety)) {
    track_watchdog_feed(watchdog_period_ms());
  } else {
    track_watchdog_stop();
  }
}

static void raise_fault(const char *reason) {
  s_fault_reason = reason;
  ESP_LOGE(TAG, "FAULT: %s", reason);
  apply_action(track_safety_step(&s_safety, TRACK_EVT_FAULT, false), 0, 0);
  track_drive_fault_brake();
}

static void handle_event(track_event_t evt, int16_t left_pct, int16_t right_pct) {
  const track_state_t before = s_safety.state;
  const track_action_t action =
      track_safety_step(&s_safety, evt, left_pct != 0 || right_pct != 0);
  apply_action(action, left_pct, right_pct);

  if (evt == TRACK_EVT_STOP && before == TRACK_STATE_FAULT) {
    /* Operator acknowledged: release the hardware latch too. */
    track_drive_clear_fault();
    s_fault_reason = "";
  }
  if (before != s_safety.state) {
    ESP_LOGI(TAG, "%s -> %s", track_state_name(before), track_state_name(s_safety.state));
  }
}

static void reply_cfg(int fd) {
  char buf[128];
  memcpy(buf, "cfg:", 4);
  if (track_config_format(&s_cfg, buf + 4, sizeof(buf) - 4) > 0) {
    web_server_send(fd, buf);
  }
}

static void handle_command(int fd, const track_command_t *cmd) {
  switch (cmd->type) {
    case TRACK_CMD_ARM:
      handle_event(TRACK_EVT_ARM, 0, 0);
      s_last_frame_us = esp_timer_get_time();
      if (s_safety.state == TRACK_STATE_FAULT) {
        web_server_send(fd, "err:fault, send stop to clear");
      }
      break;

    case TRACK_CMD_STOP:
      handle_event(TRACK_EVT_STOP, 0, 0);
      break;

    case TRACK_CMD_CONTROL:
      if (!track_safety_is_armed(&s_safety)) {
        /* Not an error worth a reply: the UI shows the state. */
        break;
      }
      if (!track_safety_accept_seq(&s_safety, cmd->seq)) {
        break;
      }
      s_last_frame_us = esp_timer_get_time();
      handle_event(TRACK_EVT_CONTROL, cmd->left_pct, cmd->right_pct);
      break;

    case TRACK_CMD_CFG_GET:
      reply_cfg(fd);
      break;

    case TRACK_CMD_CFG_SET:
      if (track_safety_is_armed(&s_safety)) {
        web_server_send(fd, "err:disarm before changing config");
        break;
      }
      if (!track_config_set(&s_cfg, cmd->key, cmd->value)) {
        web_server_send(fd, "err:bad config key or value");
        break;
      }
      track_drive_set_invert(s_cfg.invert_left, s_cfg.invert_right);
      reply_cfg(fd);
      break;

    case TRACK_CMD_CFG_SAVE:
      if (track_config_save(&s_cfg) == ESP_OK) {
        web_server_send(fd, "ok:saved");
      } else {
        web_server_send(fd, "err:save failed");
      }
      break;

    case TRACK_CMD_INVALID:
      break;
  }
}

static void publish_status(void) {
  int16_t left = 0;
  int16_t right = 0;
  track_drive_get_actual(&left, &right);
  char buf[96];
  snprintf(buf, sizeof(buf), "s:%s:%d:%d:%lu:%s", track_state_name(s_safety.state), left, right,
           (unsigned long)s_battery_mv, s_fault_reason);
  web_server_broadcast_status(buf);
}

static void periodic(int64_t now) {
  if (track_watchdog_take_trip()) {
    raise_fault("watchdog");
  }

  if (track_safety_is_armed(&s_safety) &&
      now - s_last_frame_us > (int64_t)s_cfg.timeout_ms * 1000LL) {
    ESP_LOGW(TAG, "control frame timeout (%u ms)", s_cfg.timeout_ms);
    handle_event(TRACK_EVT_TIMEOUT, 0, 0);
  }

  if (now - s_last_battery_us >= BATTERY_INTERVAL_MS * 1000LL) {
    s_last_battery_us = now;
    s_battery_mv = track_battery_read_mv();
    if (s_cfg.lowbat_mv > 0 && s_battery_mv > 0 && s_battery_mv < s_cfg.lowbat_mv) {
      if (++s_battery_low_count >= BATTERY_FAULT_SAMPLES && s_safety.state != TRACK_STATE_FAULT) {
        raise_fault("lowbat");
      }
    } else {
      s_battery_low_count = 0;
    }
  }

  if (now - s_last_update_us >= TRACK_DRIVE_UPDATE_INTERVAL_MS * 1000LL) {
    s_last_update_us = now;
    track_drive_update(s_cfg.slew_step);
  }

  if (now - s_last_status_us >= STATUS_INTERVAL_MS * 1000LL) {
    s_last_status_us = now;
    publish_status();
  }
}

static void control_task(void *arg) {
  (void)arg;
  ctrl_msg_t msg;
  while (true) {
    if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(CONTROL_IDLE_WAIT_MS)) == pdTRUE) {
      if (msg.kind == MSG_COMMAND) {
        handle_command(msg.fd, &msg.cmd);
      } else {
        ESP_LOGW(TAG, "controller disconnected");
        handle_event(TRACK_EVT_CONTROLLER_LOST, 0, 0);
      }
    }
    periodic(esp_timer_get_time());
  }
}

esp_err_t track_control_start(void) {
  track_config_load(&s_cfg);
  track_safety_init(&s_safety);
  track_drive_set_invert(s_cfg.invert_left, s_cfg.invert_right);
  track_drive_brake();

  s_queue = xQueueCreate(CONTROL_QUEUE_LEN, sizeof(ctrl_msg_t));
  if (s_queue == NULL) {
    return ESP_ERR_NO_MEM;
  }

  const int64_t now = esp_timer_get_time();
  s_last_frame_us = now;
  s_last_update_us = now;
  s_last_status_us = now;
  s_last_battery_us = now - BATTERY_INTERVAL_MS * 1000LL;

  if (xTaskCreate(control_task, "track_ctl", CONTROL_TASK_STACK, NULL, CONTROL_TASK_PRIORITY,
                  NULL) != pdPASS) {
    return ESP_FAIL;
  }

  char cfg_text[96];
  track_config_format(&s_cfg, cfg_text, sizeof(cfg_text));
  ESP_LOGI(TAG, "started, %s", cfg_text);
  return ESP_OK;
}

bool track_control_post(int fd, const track_command_t *cmd) {
  if (s_queue == NULL || cmd == NULL) {
    return false;
  }
  const ctrl_msg_t msg = {.kind = MSG_COMMAND, .fd = fd, .cmd = *cmd};
  return xQueueSend(s_queue, &msg, 0) == pdTRUE;
}

void track_control_post_controller_lost(void) {
  if (s_queue == NULL) {
    return;
  }
  const ctrl_msg_t msg = {.kind = MSG_CONTROLLER_LOST, .fd = -1};
  /* Must not be dropped: wait briefly if the queue is momentarily full. */
  if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(50)) != pdTRUE) {
    ESP_LOGE(TAG, "queue full, braking directly");
    track_drive_brake();
  }
}
