#pragma once
#include <pebble.h>

// Lightweight battery-drain telemetry.
//
// Subscribes to BatteryStateService + ConnectionService, accumulates a few
// in-RAM counters (HR samples, AppMessage tx/rx, taps, BT-up seconds, time
// spent at fast/slow HR period), and emits a single positional APP_LOG line:
//
//   BATTAP <reason> <epoch> <pct> <chg> <plg> <up> <hrp> <hrs> <hrfast>
//          <hrslow> <tx> <rx> <btup> <tap>
//
// Fields are positional (not key=value) because Pebble's APP_LOG truncates
// around ~86 chars of message body; key=value names blow that budget once
// uptime+samples get into the thousands. Columns, in order:
//   reason : "init" | "deinit" | "batt" | "tick"
//   epoch  : unix seconds at emit
//   pct    : BatteryChargeState.charge_percent (0..100, 10%-quantized)
//   chg    : is_charging (0|1)
//   plg    : is_plugged  (0|1)
//   up     : seconds since this app start
//   hrp    : current HR sample period in seconds (0=off, 1=live, 60=bg)
//   hrs    : cumulative HealthEventHeartRateUpdate count
//   hrfast : cumulative seconds spent at hrp<=2 ("fast" / 1Hz burst)
//   hrslow : cumulative seconds spent at hrp>2  ("slow" / background)
//   tx     : cumulative AppMessage outbox_sent count
//   rx     : cumulative AppMessage inbox_received count
//   btup   : cumulative seconds the Pebble-app BT connection was up
//   tap    : cumulative accel_tap_service callbacks
//
// Emitted on every battery percent transition and every 5 minutes. The
// 10%-quantized BatteryChargeState means transitions are sparse; the 5-min
// pulse fills in the gaps so you can correlate counters with charge state
// over a 12h+ window.
//
// All counters are RAM-only — they reset on app restart. Run captures with
// scripts/pebble-logs-loop so the lines land in /tmp/pebble.log.
void battery_tap_init(void);
void battery_tap_deinit(void);

// Callsite hooks — keep these tight; called on the hot path.
void battery_tap_record_hr_sample(void);
void battery_tap_record_am_tx(void);
void battery_tap_record_am_rx(void);
void battery_tap_record_tap(void);

// Caller notifies the module whenever the HR sample period changes (1Hz
// "live" vs the slower background). We use this to attribute "fast HR time"
// vs "slow HR time" between samples.
void battery_tap_set_hr_period(int period_s);
