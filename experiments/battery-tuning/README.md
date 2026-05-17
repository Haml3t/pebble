# Battery tuning

Goal: get Pebble Time 2 battery life to ≥1 week (ideally 2) while keeping the
Glance watchface useful. The dominant lever is HR sample period (was 1 Hz
always; now 60 s background with a 1 Hz burst on accel-tap), but we won't
know without measuring.

This dir is the **measurement rig + procedure** for that work. The
instrumentation itself lives in [`src/c/battery_tap.{h,c}`](../../src/c) and
is wired into Glance's `init`/`deinit` and AppMessage/Health/Tap hooks.

## What the BATTAP log lines mean

The watchface emits a positional, space-separated line on three triggers:
- `init` / `deinit` — once per app lifecycle
- `batt` — every BatteryStateService callback (10%-quantized percent
  transitions and charging-state changes)
- `tick` — every 5 minutes, regardless of state

Format (also documented in [`battery_tap.h`](../../src/c/battery_tap.h)):

```
BATTAP <reason> <epoch> <pct> <chg> <plg> <up> <hrp> <hrs> <hrfast> <hrslow> <tx> <rx> <btup> <tap> <slp>
```

| Field    | Meaning                                                |
|----------|--------------------------------------------------------|
| reason   | `init` \| `deinit` \| `batt` \| `tick`                 |
| epoch    | `time(NULL)` at emit                                   |
| pct      | `BatteryChargeState.charge_percent` (0..100, 10%-step) |
| chg      | `is_charging` (0/1)                                    |
| plg      | `is_plugged`  (0/1)                                    |
| up       | seconds since this app start                           |
| hrp      | current HR sample period in seconds                    |
| hrs      | cumulative `HealthEventHeartRateUpdate` count          |
| hrfast   | cumulative seconds spent at hrp ≤ 2 (1 Hz burst)       |
| hrslow   | cumulative seconds spent at hrp > 2 (background)       |
| tx       | cumulative AppMessage outbox-sent count                |
| rx       | cumulative AppMessage inbox-received count             |
| btup     | cumulative seconds Pebble-app BT connection was up     |
| tap      | cumulative `accel_tap_service` callbacks               |
| slp      | `health_service_sum(SleepSeconds, boot, now)` (or -1)  |

All counters are RAM-only and reset on app restart. Don't compare absolute
counter values across `init` lines; only deltas within a single run mean
anything.

Capture is via `scripts/pebble-logs-loop`, which retries reconnection so a
12-hour run survives sleeping the phone screen.

## USB-PD inline meter procedure

The 10% quantization on `BatteryChargeState.charge_percent` is too coarse to
A/B variants quickly. A USB-PD inline meter on the magnetic charge cable
bypasses it entirely — you measure mAh refilled, which is direct ground truth.

### (A) Pack capacity — one-time calibration

Goal: know the watch's actual pack size so future mAh-refill numbers convert
cleanly to %.

1. Drain the watch to ~5% (will take 1–2 days; longer on the new default).
2. Plug meter → magnetic Pebble cable → watch.
3. **Reset meter accumulator to 0 mAh.**
4. Charge to 100%. Don't touch it.
5. When current drops to ~0 mA at 100%, **read the mAh accumulated**.
6. Record this as `PACK_MAH`. Likely 150–200 mAh on a Pebble Time 2.

### (B) Per-build drain rate — repeat per variant

For each build variant you want to compare:

1. **Charge to 100%** with the meter inline. Confirm final mAh is close to
   `PACK_MAH` (if you started near empty). Unplug.
2. **Reset meter to 0** (or note the displayed value).
3. **Start log capture** on the laptop, into a per-variant file:
   ```sh
   ./scripts/pebble-logs-loop > /tmp/battery-run-${VARIANT}.log 2>&1 &
   ```
   The BT dev-connection adds a constant drain contribution that cancels out
   in A/B comparisons. Don't sweat it.
4. **Wear the watch normally for 12+ hours.** Note start timestamp.
5. **End of run**: stop log capture, note end timestamp, put the watch on the
   charger with the meter inline, **reset meter to 0**, charge to 100%.
6. **Read the meter**: mAh consumed during wear ÷ hours wearing =
   **average mA draw**. ×24 = mAh/day. ÷`PACK_MAH` = fraction of pack/day.
   Inverse = days of battery life.

### Worked example

Given:
- `PACK_MAH` = 180 mAh
- Wore 12.0 hours, refill meter read 60 mAh
- → 5.0 mA average draw
- → 120 mAh/day = 67% of pack per day
- → **~1.5 days battery life**

That headline number is what you compare across variants.

### Cross-checking against BATTAP

The log file gives you a second, independent slope estimate from the same run:

```sh
grep "BATTAP batt" /tmp/battery-run-A.log | awk '{print $3, $4}' > pct-timeline.csv
# fit slope manually or with: pct_per_hour = Δpct / (Δepoch / 3600)
```

`BATTAP tick` lines fill the gaps between 10% transitions. The two estimates
(meter mA / `PACK_MAH` and BATTAP slope) should agree within ~20% — wider
divergence usually means the test was contaminated (manually charged
mid-run, watch unpaired and reconnected several times, etc.).

The BATTAP fields are what give you *attribution*: `hrfast`/`hrslow` time
breakdown, `tap` count, `tx`/`rx` totals, `btup` time. Those are the
covariates for "% per hour attributable to subsystem X".

## Variants to run

Working list, run in order:

| # | Name              | Change                                            | Expected |
|---|-------------------|---------------------------------------------------|----------|
| 1 | `bg60`            | New default: 60 s background + 30 s 1 Hz burst    | Baseline |
| 2 | `always-1hz`      | Clay toggle "Live" = On — sensor pinned at 1 Hz   | Worse    |
| 3 | `bg300`           | Background period 300 s instead of 60 s           | Better   |
| 4 | `no-art`          | Comment out album-art chunk handling in PKJS+Android | TBD   |
| 5 | `cal-300s`        | Calendar/weather poll every 5 min instead of 1 min | TBD     |

(1) and (2) confirm the HR-cost hypothesis. (3) tests whether 60 s is the
right floor or if we can go slower without UX cost. (4) and (5) only worth
running if (1) doesn't already get us to ≥7 days projected.

## Tips that bite

- **Magnet orientation**: if the meter reads 0 mA when plugged in, flip the
  charging puck.
- **Don't let the laptop sleep** during the run — BT may drop. `tick` lines
  recover on reconnect; `batt` transitions during the gap are lost.
- **First 15 minutes are noisy** — initial weather/calendar fetches, art
  transfer, BT handshakes. Slope stabilizes after ~30 min.
- **Same wrist activity matters**: HR sensor cost depends on contact
  quality. Same wrist, same band tightness across variants.
- **Same charge endpoints**: drain to a similar level before each refill, or
  always charge to 100% from a known-equal level. Otherwise the meter mAh
  totals aren't directly comparable across variants.

## Known caveat

The Pebble firmware can deliver `HealthEventHeartRateUpdate` callbacks
faster than the configured sample period when fresh data is available
(observed during verification: ~14 samples in 105 s at a 60 s period). So
treat `hrs` (sample count) as informational, not as the truth of how often
the sensor was *active*. `hrfast` / `hrslow` (time spent at each period
config) is the variable that drives the underlying sensor power state and
the one to correlate with drain.
