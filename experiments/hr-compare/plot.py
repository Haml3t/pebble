#!/usr/bin/env python3
"""Overlay Pebble and Fitbit HR traces on a shared time axis.

Pebble CSV has columns (unix_ts, bpm_raw, bpm_filtered) — bpm_filtered may
be empty for samples where the firmware hadn't produced a filtered value yet.
Fitbit CSV has (unix_ts, bpm). Output is a PNG.
"""

import argparse
import csv
import datetime as dt
import sys
from pathlib import Path

import matplotlib.dates as mdates
import matplotlib.pyplot as plt


def load_pebble(path: Path) -> tuple[
        list[dt.datetime],
        list[int],
        list[dt.datetime],
        list[int]]:
    raw_t, raw_y, filt_t, filt_y = [], [], [], []
    with path.open() as f:
        r = csv.reader(f)
        header = next(r)
        # Old single-bpm CSVs had columns "unix_ts,bpm"; current ones have
        # "unix_ts,bpm_raw,bpm_filtered". Sniff which we're reading.
        has_filtered = "bpm_filtered" in header
        for row in r:
            if not row:
                continue
            t = dt.datetime.fromtimestamp(int(row[0]))
            raw_t.append(t)
            raw_y.append(int(row[1]))
            if has_filtered and len(row) >= 3 and row[2] != "":
                filt_t.append(t)
                filt_y.append(int(row[2]))
    return raw_t, raw_y, filt_t, filt_y


def load_fitbit(path: Path) -> tuple[list[dt.datetime], list[int]]:
    times, bpms = [], []
    with path.open() as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            if not row:
                continue
            times.append(dt.datetime.fromtimestamp(int(row[0])))
            bpms.append(int(row[1]))
    return times, bpms


def parse_hhmm(date_ref: dt.datetime, s: str) -> dt.datetime:
    h, m = (int(x) for x in s.split(":"))
    return date_ref.replace(hour=h, minute=m, second=0, microsecond=0)


def clip(times, vals, t_min, t_max):
    if not times:
        return [], []
    out_t, out_v = [], []
    for t, v in zip(times, vals):
        if t_min <= t <= t_max:
            out_t.append(t)
            out_v.append(v)
    return out_t, out_v


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pebble", default="pebble.csv")
    ap.add_argument("--fitbit", default="fitbit.csv")
    ap.add_argument("--out", default="compare.png")
    ap.add_argument("--start", help="clip start, HH:MM local")
    ap.add_argument("--end", help="clip end, HH:MM local")
    args = ap.parse_args()

    p_raw_t, p_raw_y, p_filt_t, p_filt_y = load_pebble(Path(args.pebble))
    f_t, f_y = load_fitbit(Path(args.fitbit))

    if not p_raw_t and not f_t:
        sys.exit("both inputs are empty")

    ref = (p_raw_t or f_t)[0]
    if args.start:
        t_min = parse_hhmm(ref, args.start)
    else:
        t_min = min((p_raw_t or f_t)[0], (f_t or p_raw_t)[0])
    if args.end:
        t_max = parse_hhmm(ref, args.end)
    else:
        t_max = max((p_raw_t or f_t)[-1], (f_t or p_raw_t)[-1])

    p_raw_t, p_raw_y   = clip(p_raw_t, p_raw_y, t_min, t_max)
    p_filt_t, p_filt_y = clip(p_filt_t, p_filt_y, t_min, t_max)
    f_t, f_y           = clip(f_t, f_y, t_min, t_max)

    fig, ax = plt.subplots(figsize=(12, 5))
    if p_raw_t:
        ax.plot(p_raw_t, p_raw_y,
                label=f"Pebble raw      ({len(p_raw_t)} pts)",
                linewidth=1.0, alpha=0.85)
    if p_filt_t:
        ax.plot(p_filt_t, p_filt_y,
                label=f"Pebble filtered ({len(p_filt_t)} pts)",
                linewidth=1.4, alpha=0.9)
    if f_t:
        ax.plot(f_t, f_y,
                label=f"Fitbit          ({len(f_t)} pts)",
                linewidth=1.2, alpha=0.85)

    ax.set_xlabel("time (local)")
    ax.set_ylabel("BPM")
    ax.set_title("HR comparison — Pebble (raw + filtered) vs Fitbit")
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper right")
    fig.autofmt_xdate()
    fig.tight_layout()

    out_path = Path(args.out)
    fig.savefig(out_path, dpi=130)
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
