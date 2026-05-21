#!/usr/bin/env python3
"""Parse PERF lines from pebble app logs into JSON.

Reads a log file produced by `pebble logs`, extracts every
`PERF fps=N heap=M state=S ay=A btn=0xH` line, and emits a JSON
document with the raw samples plus a summary block the dev-loop
scripts can assert against (median/min/max fps, heap delta).

Usage:
  doom-parse-perf.py <logfile>
"""
import json
import re
import statistics
import sys
from pathlib import Path

# Captures lines like:
#   ... PERF fps=18 heap=92480 state=TITLE ay=12 btn=0x00
PERF_RE = re.compile(
    r"PERF\s+"
    r"fps=(?P<fps>\d+)\s+"
    r"heap=(?P<heap>\d+)\s+"
    r"state=(?P<state>\S+)\s+"
    r"ay=(?P<ay>-?\d+)\s+"
    r"btn=0x(?P<btn>[0-9a-fA-F]+)"
)

INIT_RE = re.compile(r"INIT_OK\s+heap=(?P<heap>\d+)")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: doom-parse-perf.py <logfile>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    if not path.exists():
        print(json.dumps({"error": f"no such file: {path}", "samples": [], "summary": {}}))
        return 1

    samples = []
    init_heap = None
    for line in path.read_text(errors="replace").splitlines():
        m = INIT_RE.search(line)
        if m and init_heap is None:
            init_heap = int(m.group("heap"))
        m = PERF_RE.search(line)
        if not m:
            continue
        samples.append({
            "fps":   int(m.group("fps")),
            "heap":  int(m.group("heap")),
            "state": m.group("state"),
            "ay":    int(m.group("ay")),
            "btn":   int(m.group("btn"), 16),
        })

    summary = {"sample_count": len(samples)}
    if samples:
        fps_values = [s["fps"] for s in samples]
        heap_values = [s["heap"] for s in samples]
        summary.update({
            "fps_median": statistics.median(fps_values),
            "fps_min":    min(fps_values),
            "fps_max":    max(fps_values),
            "heap_first": heap_values[0],
            "heap_last":  heap_values[-1],
            "heap_delta": heap_values[0] - heap_values[-1],  # +ve == leaked
            "states_seen": sorted({s["state"] for s in samples}),
        })
    if init_heap is not None:
        summary["init_heap"] = init_heap

    json.dump({"samples": samples, "summary": summary}, sys.stdout, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
