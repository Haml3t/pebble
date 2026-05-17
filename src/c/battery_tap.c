#include "battery_tap.h"

// Cadence for the periodic snapshot. 5 minutes is short enough that an 8-hour
// run produces ~100 lines (good for slope fitting) but long enough that the
// AppTimer itself contributes negligible drain.
#define BATTERY_TAP_PERIOD_MS (5 * 60 * 1000)

// "Fast" HR threshold: anything <=2s is treated as the 1Hz burst regime;
// anything else as background. 0 means "off / system default."
#define HR_FAST_THRESHOLD_S 2

// DataLogging session tag. Arbitrary uint32 — the Android receiver
// (BattapDataLogReceiver.java) matches on this exact value. Changing it
// breaks consumption of old buffered rows still on the watch, so don't.
#define BATTAP_DATALOG_TAG 0xBA77AB01

typedef enum {
  BATTAP_REASON_INIT   = 0,
  BATTAP_REASON_DEINIT = 1,
  BATTAP_REASON_BATT   = 2,
  BATTAP_REASON_TICK   = 3,
} BattapReason;

// Wire format for each row. Packed and fixed-size so the Android consumer
// can byte-offset-parse without worrying about ARM struct alignment. Total
// is 46 bytes (verify with sizeof() in init); the Pebble DataLogging API
// transfers in fixed-size items and the receiver parses by offset.
//
// Field order matches the APP_LOG column order in battery_tap.h.
typedef struct __attribute__((packed)) {
  uint32_t epoch;
  uint8_t  reason;        // BattapReason cast to u8
  uint8_t  pct;
  uint8_t  is_charging;
  uint8_t  is_plugged;
  uint32_t uptime_secs;
  uint16_t hr_period_s;
  uint32_t hr_samples;
  uint32_t hr_fast_secs;
  uint32_t hr_slow_secs;
  uint32_t am_tx_count;
  uint32_t am_rx_count;
  uint32_t bt_up_secs;
  uint32_t tap_count;
  int32_t  sleep_secs;
} BattapRow;

static const char *reason_str(BattapReason r) {
  switch (r) {
    case BATTAP_REASON_INIT:   return "init";
    case BATTAP_REASON_DEINIT: return "deinit";
    case BATTAP_REASON_BATT:   return "batt";
    case BATTAP_REASON_TICK:   return "tick";
  }
  return "?";
}

static DataLoggingSessionRef s_log;

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

// Cumulative sleep seconds in the run window. Negative if PBL_HEALTH is
// off or the firmware can't fulfill the query (e.g. no historical data
// yet at first boot). We mirror -1 into the log so post-hoc parsing can
// distinguish "definitely zero sleep" from "no data".
static long sleep_secs_since_boot(void) {
#if defined(PBL_HEALTH)
  time_t now = time(NULL);
  HealthServiceAccessibilityMask m = health_service_metric_accessible(
      HealthMetricSleepSeconds, (time_t)s_boot_epoch, now);
  if (m & HealthServiceAccessibilityMaskAvailable) {
    return (long)health_service_sum(HealthMetricSleepSeconds,
                                    (time_t)s_boot_epoch, now);
  }
#endif
  return -1;
}

static void emit_sample(BattapReason reason, BatteryChargeState st) {
  uint32_t now = (uint32_t)time(NULL);
  flush_hr_period_accounting(now);
  flush_bt_accounting(now);
  long slp = sleep_secs_since_boot();
  uint32_t uptime = now - s_boot_epoch;

  // Positional, space-separated — see battery_tap.h for column layout.
  // APP_LOG truncates the message body around 86 chars, so we trade keys
  // for brevity. A typical 12-hour run produces values that fit:
  //   "BATTAP tick 1715792345 87 0 0 43200 60 4321 30 43170 540 240 43200 12 28800"
  APP_LOG(APP_LOG_LEVEL_INFO,
          "BATTAP %s %lu %d %d %d %lu %d %lu %lu %lu %lu %lu %lu %lu %ld",
          reason_str(reason),
          (unsigned long)now,
          (int)st.charge_percent,
          st.is_charging ? 1 : 0,
          st.is_plugged ? 1 : 0,
          (unsigned long)uptime,
          s_current_hr_period,
          (unsigned long)s_hr_samples_total,
          (unsigned long)s_hr_fast_secs,
          (unsigned long)s_hr_slow_secs,
          (unsigned long)s_am_tx_count,
          (unsigned long)s_am_rx_count,
          (unsigned long)s_bt_up_secs,
          (unsigned long)s_tap_count,
          slp);

  // Same data, byte-encoded, into the DataLogging queue — survives BT
  // dropouts (640 KB on-watch buffer) and gets drained to the Android
  // companion whenever it's reachable. APP_LOG above is the live tail;
  // this is the durable record.
  if (s_log) {
    BattapRow row = {
      .epoch        = now,
      .reason       = (uint8_t)reason,
      .pct          = (uint8_t)st.charge_percent,
      .is_charging  = st.is_charging ? 1 : 0,
      .is_plugged   = st.is_plugged ? 1 : 0,
      .uptime_secs  = uptime,
      .hr_period_s  = (uint16_t)s_current_hr_period,
      .hr_samples   = s_hr_samples_total,
      .hr_fast_secs = s_hr_fast_secs,
      .hr_slow_secs = s_hr_slow_secs,
      .am_tx_count  = s_am_tx_count,
      .am_rx_count  = s_am_rx_count,
      .bt_up_secs   = s_bt_up_secs,
      .tap_count    = s_tap_count,
      .sleep_secs   = (int32_t)slp,
    };
    data_logging_log(s_log, &row, 1);
  }
}

static void battery_state_handler(BatteryChargeState st) {
  emit_sample(BATTAP_REASON_BATT, st);
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
  emit_sample(BATTAP_REASON_TICK, battery_state_service_peek());
  schedule_periodic();
}

void battery_tap_init(void) {
  s_boot_epoch = (uint32_t)time(NULL);
  s_hr_period_set_at = s_boot_epoch;

  // Seed BT state from the current connection so the first chunk of
  // up-time isn't lost waiting for an edge.
  s_bt_up = connection_service_peek_pebble_app_connection();
  s_bt_up_since = s_bt_up ? s_boot_epoch : 0;

  // Open the DataLogging session before any emit, so the first "init"
  // row lands in the queue too. resume=false starts a fresh session on
  // every app launch — we don't want stale rows from a prior install
  // (where counters were RAM-only and just got reset to zero) blending
  // with the current run's monotonic counters.
  s_log = data_logging_create(BATTAP_DATALOG_TAG,
                              DATA_LOGGING_BYTE_ARRAY,
                              sizeof(BattapRow),
                              false);

  battery_state_service_subscribe(battery_state_handler);
  connection_service_subscribe((ConnectionHandlers){
      .pebble_app_connection_handler = connection_handler,
  });
  schedule_periodic();
  emit_sample(BATTAP_REASON_INIT, battery_state_service_peek());
}

void battery_tap_deinit(void) {
  if (s_periodic_timer) {
    app_timer_cancel(s_periodic_timer);
    s_periodic_timer = NULL;
  }
  emit_sample(BATTAP_REASON_DEINIT, battery_state_service_peek());
  if (s_log) {
    data_logging_finish(s_log);
    s_log = NULL;
  }
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
