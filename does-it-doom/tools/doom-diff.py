#!/usr/bin/env python3
"""Compare captured screenshots against golden PNGs and report deltas.

Usage:
  doom-diff.py --frames <captured-dir> --goldens <golden-dir> \
               [--report <out.json>] [--threshold 2.0]

Matches frame `t05s.png` to golden `t05s.png` (filename equality). Skips any
frame that has no corresponding golden. Reports per-pixel percentage delta;
fails (exit 1) if any matched frame exceeds the threshold.

Uses Pillow if available; otherwise falls back to raw-bytes comparison via
the standard library (less informative but works in clean environments).
"""
import argparse
import json
import sys
from pathlib import Path


def pixel_delta_pct(path_a: Path, path_b: Path) -> float:
    try:
        from PIL import Image
    except ImportError:
        # Best-effort: raw bytes equality. Not great, but never silently
        # claims passing when files actually differ.
        return 0.0 if path_a.read_bytes() == path_b.read_bytes() else 100.0
    a = Image.open(path_a).convert("RGBA")
    b = Image.open(path_b).convert("RGBA")
    if a.size != b.size:
        return 100.0
    ax, bx = a.tobytes(), b.tobytes()
    diffs = sum(1 for x, y in zip(ax, bx) if x != y)
    return 100.0 * diffs / max(len(ax), 1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames",  required=True, type=Path)
    ap.add_argument("--goldens", required=True, type=Path)
    ap.add_argument("--report",  type=Path)
    ap.add_argument("--threshold", type=float, default=2.0,
                    help="max %% pixel delta before failure")
    args = ap.parse_args()

    results = []
    failed = False
    for frame in sorted(args.frames.glob("*.png")):
        golden = args.goldens / frame.name
        if not golden.exists():
            results.append({"frame": frame.name, "status": "no-golden"})
            continue
        delta = pixel_delta_pct(frame, golden)
        status = "ok" if delta <= args.threshold else "fail"
        if status == "fail":
            failed = True
        results.append({
            "frame":     frame.name,
            "delta_pct": round(delta, 2),
            "threshold": args.threshold,
            "status":    status,
        })

    report = {"results": results, "passed": not failed}
    payload = json.dumps(report, indent=2)
    if args.report:
        args.report.write_text(payload)
    print(payload)
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
