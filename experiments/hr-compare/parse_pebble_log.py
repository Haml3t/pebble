#!/usr/bin/env python3
"""Extract HRCMP lines from pebble logs into a CSV of (unix_ts, bpm).

Reads a file like /tmp/pebble.log produced by `pebble logs` (or the
pebble-logs-loop wrapper). Each watchapp HR sample is logged as a line of
the form:

    [HH:MM:SS] main.c:LINE> HRCMP <unix_ts> <bpm>

We don't need the bracketed time; the unix_ts in the payload is authoritative.
"""

import argparse
import csv
import re
import sys
from pathlib import Path

# HRCMP line carries the raw BPM and (optionally) a filtered BPM. The
# filtered value was added later — old log lines have just two numbers, new
# ones have three. The third group is optional so both formats parse.
LINE_RE = re.compile(r"\bHRCMP\s+(\d+)\s+(\d+)(?:\s+(\d+))?\b")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--log", default="/tmp/pebble.log",
                    help="path to pebble logs file (default /tmp/pebble.log)")
    ap.add_argument("--out", default="pebble.csv",
                    help="output CSV path (default pebble.csv)")
    args = ap.parse_args()

    log_path = Path(args.log)
    if not log_path.exists():
        print(f"error: {log_path} does not exist", file=sys.stderr)
        return 1

    # (unix_ts, raw_bpm, filtered_bpm_or_None)
    rows: list[tuple[int, int, int | None]] = []
    with log_path.open() as f:
        for line in f:
            m = LINE_RE.search(line)
            if not m:
                continue
            ts = int(m.group(1))
            raw = int(m.group(2))
            filt_raw = m.group(3)
            # Treat both "no third value" (old format) and 0 (firmware
            # hasn't produced a filtered value yet) as missing.
            filt: int | None = None
            if filt_raw is not None:
                fv = int(filt_raw)
                filt = fv if fv > 0 else None
            rows.append((ts, raw, filt))

    # Dedup consecutive duplicates at the same timestamp.
    deduped: list[tuple[int, int, int | None]] = []
    for r in rows:
        if deduped and deduped[-1] == r:
            continue
        deduped.append(r)

    with Path(args.out).open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["unix_ts", "bpm_raw", "bpm_filtered"])
        for ts, raw, filt in deduped:
            w.writerow([ts, raw, "" if filt is None else filt])

    print(f"wrote {len(deduped)} rows to {args.out} "
          f"(from {len(rows)} raw HRCMP lines)")
    if deduped:
        first = deduped[0][0]
        last = deduped[-1][0]
        with_filt = sum(1 for _, _, f in deduped if f is not None)
        print(f"span: {first}..{last} ({last - first}s); "
              f"filtered values present in {with_filt}/{len(deduped)} rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
