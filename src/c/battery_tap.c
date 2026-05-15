#include "battery_tap.h"

// Cadence for the periodic snapshot. 5 minutes is short enough that an 8-hour
// run produces ~100 lines (good for slope fitting) but long enough that the
// AppTimer itself contributes negligible drain.
#define BATTERY_TAP_PERIOD_MS (5 * 60 * 1000)

// "Fast" HR threshold: anything <=2s is treated as the 1Hz burst regime;
// anything else as background. 0 means "off / system default."
#define HR_FAST_THRESHOLD_S 2

static uint32_t s_boot_epoch;

static uint32_t s_hr_samples_total;
static uint32_t s_am_tx_count;
static uint32_t s_am_rx_count;
static uint32_t s_tap_count;

static int      s_current_hr_period;
static uint32_t s_hr_period_set_at;
static uint32_t s_hr_fast_secs;
static uint32_t s_hr_slow_secs;

static bool     s_bt_up;
static uint32_t s_bt_up_since;
static uint32_t s_bt_up_secs;

static AppTimer *s_periodic_timer;

// Roll forward whichever HR-period bucket has been accumulating since the
// last flush. Called before every emit and on every period change so the
// accounting is monotonic from the caller's perspective.
static void flush_hr_period_accounting(uint32_t now) {
  if (s_hr_period_set_at == 0) {
    s_hr_period_set_at = now;
    return;
  }
  uint32_t delta = now - s_hr_period_set_at;
  if (s_current_hr_period > 0 && s_current_hr_period <= HR_FAST_THRESHOLD_S) {
    s_hr_fast_secs += delta;
  } else if (s_current_hr_period > HR_FAST_THRESHOLD_S) {
    s_hr_slow_secs += delta;
  }
  // period==0 means "system default / unsubscribed" — don't charge either
  // bucket; the gap is implicit.
  s_hr_period_set_at = now;
}

static void flush_bt_accounting(uint32_t now) {
  if (s_bt_up && s_bt_up_since > 0) {
    s_bt_up_secs += (now - s_bt_up_since);
    s_bt_up_since = now;
  }
}

static void emit_sample(const char *reason, BatteryChargeState st) {
  uint32_t now = (uint32_t)time(NULL);
  flush_hr_period_accounting(now);
  flush_bt_accounting(now);
  // Positional, space-separated — see battery_tap.h for column layout.
  // APP_LOG truncates the message body around 86 chars, so we trade keys
  // for brevity. A typical 12-hour run produces values that fit:
  //   "BATTAP tick 1715792345 87 0 0 43200 60 4321 30 43170 540 240 43200 12"
  APP_LOG(APP_LOG_LEVEL_INFO,
          "BATTAP %s %lu %d %d %d %lu %d %lu %lu %lu %lu %lu %lu %lu",
          reason,
          (unsigned long)now,
          (int)st.charge_percent,
          st.is_charging ? 1 : 0,
          st.is_plugged ? 1 : 0,
          (unsigned long)(now - s_boot_epoch),
          s_current_hr_period,
          (unsigned long)s_hr_samples_total,
          (unsigned long)s_hr_fast_secs,
          (unsigned long)s_hr_slow_secs,
          (unsigned long)s_am_tx_count,
          (unsigned long)s_am_rx_count,
          (unsigned long)s_bt_up_secs,
          (unsigned long)s_tap_count);
}

static void battery_state_handler(BatteryChargeState st) {
  emit_sample("batt", st);
}

static void connection_handler(bool connected) {
  uint32_t now = (uint32_t)time(NULL);
  flush_bt_accounting(now);
  s_bt_up = connected;
  s_bt_up_since = connected ? now : 0;
}

static void periodic_tick(void *ctx);
static void schedule_periodic(void) {
  s_periodic_timer = app_timer_register(BATTERY_TAP_PERIOD_MS,
                                        periodic_tick, NULL);
}
static void periodic_tick(void *ctx) {
  s_periodic_timer = NULL;
  emit_sample("tick", battery_state_service_peek());
  schedule_periodic();
}

void battery_tap_init(void) {
  s_boot_epoch = (uint32_t)time(NULL);
  s_hr_period_set_at = s_boot_epoch;

  // Seed BT state from the current connection so the first chunk of
  // up-time isn't lost waiting for an edge.
  s_bt_up = connection_service_peek_pebble_app_connection();
  s_bt_up_since = s_bt_up ? s_boot_epoch : 0;

  battery_state_service_subscribe(battery_state_handler);
  connection_service_subscribe((ConnectionHandlers){
      .pebble_app_connection_handler = connection_handler,
  });
  schedule_periodic();
  emit_sample("init", battery_state_service_peek());
}

void battery_tap_deinit(void) {
  if (s_periodic_timer) {
    app_timer_cancel(s_periodic_timer);
    s_periodic_timer = NULL;
  }
  emit_sample("deinit", battery_state_service_peek());
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
}

void battery_tap_record_hr_sample(void) { s_hr_samples_total++; }
void battery_tap_record_am_tx(void)     { s_am_tx_count++; }
void battery_tap_record_am_rx(void)     { s_am_rx_count++; }
void battery_tap_record_tap(void)       { s_tap_count++; }

void battery_tap_set_hr_period(int period_s) {
  flush_hr_period_accounting((uint32_t)time(NULL));
  s_current_hr_period = period_s;
}
