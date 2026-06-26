#!/usr/bin/env python3
"""Compute per-trial battery + activity ratios from a BATTAP CSV.

Usage:
  scripts/analyze-battap.py ~/battap-V4-1.csv [--meter-mah 116] [--label V4-1]

Cumulative counters (hrs/hrfast/hrslow/tx/rx/btup/tap/slp) reset on each
watchapp init. This script walks the CSV detecting restarts (where uptime
falls between rows) and sums the last cumulative value before each restart
plus the final session value to get true wear totals.

Wear duration is wall-clock from the first plg=0 row to the last row
before the next plg=1 transition (i.e. when the watch was off the puck).
"""

import argparse
import csv
import sys

COUNTERS = ['hrs', 'hrfast', 'hrslow', 'tx', 'rx', 'btup', 'tap', 'slp']


def parse_rows(path):
    rows = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append({
                'epoch':   int(r['epoch']),
                'reason':  r['reason'],
                'pct':     int(r['pct']),
                'chg':     int(r['chg']),
                'plg':     int(r['plg']),
                'uptime':  int(r['uptime']),
                'hrp':     int(r['hrp']),
                'hrs':     int(r['hrs']),
                'hrfast':  int(r['hrfast']),
                'hrslow':  int(r['hrslow']),
                'tx':      int(r['tx']),
                'rx':      int(r['rx']),
                'btup':    int(r['btup']),
                'tap':     int(r['tap']),
                'slp':     int(r['slp']),
            })
    return rows


def wear_window(rows):
    """Return (start_idx, end_idx, start_epoch, end_epoch) for the longest
    contiguous plg=0 window. Handles the typical case: a brief plg=1 block
    at the start (pre-unplug verification), then a long plg=0 wear, then
    plg=1 again at end-of-run charging."""
    best = None
    i = 0
    n = len(rows)
    while i < n:
        if rows[i]['plg'] == 0:
            j = i
            while j + 1 < n and rows[j + 1]['plg'] == 0:
                j += 1
            duration = rows[j]['epoch'] - rows[i]['epoch']
            if best is None or duration > best[1] - best[0]:
                best = (rows[i]['epoch'], rows[j]['epoch'], i, j)
            i = j + 1
        else:
            i += 1
    if best is None:
        return None
    return best


def cumulative_totals(rows, start_idx, end_idx):
    """Sum each counter across all restarts within [start_idx, end_idx]."""
    totals = {k: 0 for k in COUNTERS}
    prev = None
    restarts = 0
    for i in range(start_idx, end_idx + 1):
        row = rows[i]
        if prev is not None and row['uptime'] < prev['uptime']:
            for k in COUNTERS:
                totals[k] += prev[k]
            restarts += 1
        prev = row
    for k in COUNTERS:
        totals[k] += rows[end_idx][k]
    return totals, restarts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv', help='Path to battap-Vx-y.csv')
    ap.add_argument('--meter-mah', type=float, default=None,
                    help='KOWSI meter mAh refill (the headline)')
    ap.add_argument('--label', default=None)
    ap.add_argument('--pack-mah', type=float, default=200,
                    help='Pack capacity wall-side, default 200')
    args = ap.parse_args()

    rows = parse_rows(args.csv)
    if not rows:
        print('empty CSV', file=sys.stderr)
        sys.exit(1)

    win = wear_window(rows)
    if not win:
        print('no plg=0 wear window found', file=sys.stderr)
        sys.exit(1)
    start_epoch, end_epoch, start_idx, end_idx = win
    wear_s = end_epoch - start_epoch
    wear_h = wear_s / 3600

    start_pct = rows[start_idx]['pct']
    end_pct = rows[end_idx]['pct']
    drained_pct = start_pct - end_pct

    totals, restarts = cumulative_totals(rows, start_idx, end_idx)

    # Derived ratios
    hrfast_frac = totals['hrfast'] / wear_s if wear_s else 0
    tap_per_h = totals['tap'] / wear_h if wear_h else 0
    sleep_frac = totals['slp'] / wear_s if wear_s else 0
    bt_frac = totals['btup'] / wear_s if wear_s else 0
    wake_h = (wear_s - totals['slp']) / 3600

    # Battery
    battap_pct_per_h = drained_pct / wear_h if wear_h else 0
    battap_days = (start_pct / battap_pct_per_h / 24) if battap_pct_per_h else float('inf')

    label = args.label or args.csv.split('/')[-1]
    print(f'=== {label} ===')
    print(f'Wear window:    {wear_h:.2f} h  ({wear_s} s)')
    print(f'pct:            {start_pct} -> {end_pct}  (drained {drained_pct}%)')
    print(f'Restarts:       {restarts}')
    print()
    print(f'-- BATTAP slope (cross-check) --')
    print(f'  drain:        {battap_pct_per_h:.3f} %/h')
    print(f'  projected:    {battap_days:.2f} days full -> 0')
    if args.meter_mah is not None:
        meter_mA = args.meter_mah / wear_h
        meter_pct_h = args.meter_mah / args.pack_mah * 100 / wear_h
        meter_days = args.pack_mah / args.meter_mah * wear_h / 24
        agree = abs(meter_pct_h - battap_pct_per_h) / meter_pct_h * 100
        print(f'-- Meter ({args.meter_mah} mAh refill) --')
        print(f'  avg current:  {meter_mA:.3f} mA')
        print(f'  drain:        {meter_pct_h:.3f} %/h  ({agree:.1f}% divergence vs BATTAP)')
        print(f'  projected:    {meter_days:.2f} days full -> 0')
    print()
    # All comparisons across trials should use the normalized rates/fractions
    # below — absolute totals scale with wear length and will mislead.
    wake_frac = 1 - sleep_frac
    restart_rate = restarts / wear_h if wear_h else 0
    print(f'-- Activity proxies (confounders, normalized per unit wear) --')
    print(f'  tap_rate:     {tap_per_h:6.2f} /h    (accel-tap callbacks per hour)')
    print(f'  hrfast_frac:  {hrfast_frac:6.4f}     (fraction of wear at 1Hz HR; higher = more glances/workouts)')
    print(f'  wake_frac:    {wake_frac:6.3f}      (fraction of wear awake — 1 - sleep)')
    print(f'  sleep_frac:   {sleep_frac:6.3f}      (fraction of wear sensed as sleep)')
    print(f'  bt_uptime:    {bt_frac:6.3f}      (fraction of wear with BT to phone)')
    print(f'  restart_rate: {restart_rate:6.2f} /h    (watchapp restarts per hour)')
    print()
    print(f'-- Cumulative counters (summed across {restarts} restarts) --')
    for k in COUNTERS:
        print(f'  {k:8s}    {totals[k]}')


if __name__ == '__main__':
    main()
