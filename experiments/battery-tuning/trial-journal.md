# Battery trial wear journal

Per-trial freeform notes that aren't easy to recover from the CSV. Use this
when comparing trials with surprisingly different drain — workout days
should show up in `tap_rate` and `hrfast_frac` from `scripts/analyze-battap.py`,
and this journal is the explanatory variable.

Two lines per day is plenty. Capture:
- Exercise (type, duration, intensity)
- Long stretches off the wrist (charging another device, sleep)
- Anomalies (firmware updates, watchapp restarts you noticed, BT
  dropouts, being far from phone for hours)

## Trials

### V3-1 (2026-05-22 12:26 EDT → 2026-05-24 16:35 EDT) — 52 mAh / 8.4 days projected
- Wear: 52.15 h. Pre-dates this journal — no detailed daily notes.
- Known: ~22 watchapp restarts during wear (watchface switches).

### V4-1 (2026-05-24 17:30 EDT → 2026-05-28 12:01 EDT) — 116 mAh / 6.5 days projected
- Wear: 90.5 h.
- BATTAP slope (0.62 %/h) is **worse** than V3-1 (0.50 %/h). Cause unclear — possible activity confounder. Tap rate during the last 79 min of wear was ~17 /h, suspiciously high.
- (No daily notes — backfill if you remember any specific workouts during this window.)

### V3-2 (start: 2026-05-28 13:05 EDT → end: 2026-06-01 13:54 EDT) — 96.87 h wear
- Meter reset to 0 before start: **uncertain** (probably not — see headline)
- Day 1 (2026-05-28): no workout
- Day 2 (2026-05-29): no workout
- Day 3 (2026-05-30): no workout
- Day 4 (2026-05-31): no workout
- Day 5 (2026-06-01): no workout (partial day, until end-of-run)
- Notes: zero workouts during this trial. Some metronome use across the period. Should be a "low-activity" V3 trial, but ended up draining FASTER than V3-1's higher-activity trial. Meter at end of charge read 245 mAh — implies 377 mAh pack which is physically impossible, so the meter probably wasn't reset before unplug (carryover from V4-1's 116 mAh + V3-2's true 129 mAh). Carryover-corrected: 129 mAh / 1.33 mA / 6.2 days projected. Meter ↔ BATTAP agreement is 0.8% with the corrected number, confirming the inference.

**Lesson: always note meter-reset confirmation in the start-of-trial row.** Template updated below.

### V4-2 (start: 2026-06-01 15:23 EDT → end: 2026-06-04 ~19:13 EDT) — 75.79 h wear, 94 mAh, 1.24 mA, 6.7 days projected
- Meter reset to 0 before start: [x] yes (confirmed display at 0 mAh)
- Day 1 (2026-06-01): no workout noted
- Day 2 (2026-06-02): possible ~30 min workout in the evening (Tuesday — uncertain whether tracked)
- Day 3 (2026-06-03): ~30 min walk, likely picked up by health service as a workout
- Day 4 (2026-06-04): no workout noted (partial day, until end-of-run)
- Notes: tap_rate 3.27/h is essentially the same as V3-2's 3.32/h — first apples-to-apples activity-matched comparison. V4-2 drained 7% less than V3-2 (1.24 vs 1.33 mA). First credible config-effect signal.

### V3-3 (start: ___ EDT → end: ___ EDT)
- Meter reset to 0 before start: [ ] yes  (confirm by watching display flash to 0 mAh before unplug)
- Day 1 (date): 
- Day 2 (date): 
- Day 3 (date): 
- Day 4 (date): 
- Notes:

### V4-3 (start: ___ EDT → end: ___ EDT)
- Meter reset to 0 before start: [ ] yes  (confirm by watching display flash to 0 mAh before unplug)
- Day 1 (date): 
- Day 2 (date): 
- Day 3 (date): 
- Day 4 (date): 
- Notes:
