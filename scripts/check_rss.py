#!/usr/bin/env python3
"""scripts/check_rss.py -- acceptance gate for a bench_rss CSV.

Turns "look at the RSS curve" into a reproducible pass/fail check:

  * every data row must report errors == 0;
  * the run must cover at least --min-hours;
  * total RSS growth (last - first) must stay under --max-delta-kb;
  * the least-squares slope of the second half of the run (the steady state)
    must stay under --max-slope-kb-per-hour.

The slope, not the absolute value, is what distinguishes an allocator warm-up
from a leak: a stable curve has a slope near zero even if RSS is a few MiB.
Exit codes: 0 pass, 1 acceptance failure, 2 unreadable/insufficient input.

Usage:
  python3 scripts/check_rss.py bench-results/rss_curve.csv --min-hours 23.5
"""
import argparse
import re
import sys

ROW = re.compile(r"^\s*([0-9]+(?:\.[0-9]+)?),\s*(-?[0-9]+),\s*([0-9]+),\s*([0-9]+)\s*$")


def load(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = ROW.match(line)
            if m:
                rows.append(
                    (float(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))
                )
    return rows


def slope_per_hour(points):
    n = len(points)
    if n < 2:
        return 0.0
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    den = sum((p[0] - mx) ** 2 for p in points)
    if den == 0.0:
        return 0.0
    num = sum((p[0] - mx) * (p[1] - my) for p in points)
    return (num / den) * 3600.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--min-hours", type=float, default=0.0)
    ap.add_argument("--max-delta-kb", type=int, default=2048)
    ap.add_argument("--max-slope-kb-per-hour", type=float, default=512.0)
    args = ap.parse_args()

    try:
        rows = load(args.csv)
    except OSError as exc:
        print(f"FAIL: cannot read {args.csv}: {exc}")
        return 2

    if len(rows) < 2:
        print(f"FAIL: {args.csv} has {len(rows)} data rows; need >= 2")
        return 2

    first_t, first_rss = rows[0][0], rows[0][1]
    last_t, last_rss = rows[-1][0], rows[-1][1]
    peak = max(r[1] for r in rows)
    hours = (last_t - first_t) / 3600.0
    delta = last_rss - first_rss
    errors = rows[-1][3]
    half = rows[len(rows) // 2:]
    slope = slope_per_hour([(r[0], r[1]) for r in half])

    print(
        f"RSS check: rows={len(rows)} hours={hours:.2f} "
        f"first={first_rss}KiB last={last_rss}KiB delta={delta}KiB "
        f"peak={peak}KiB slope_h2={slope:.1f}KiB/h errors={errors}"
    )

    problems = []
    if errors != 0:
        problems.append(f"errors={errors} (must be 0)")
    if hours < args.min_hours:
        problems.append(f"hours={hours:.2f} < min-hours={args.min_hours}")
    if delta > args.max_delta_kb:
        problems.append(f"delta={delta}KiB > max-delta-kb={args.max_delta_kb}")
    if slope > args.max_slope_kb_per_hour:
        problems.append(
            f"steady slope={slope:.1f}KiB/h > "
            f"max-slope-kb-per-hour={args.max_slope_kb_per_hour}"
        )

    if problems:
        print("FAIL: " + "; ".join(problems))
        return 1
    print("PASS: RSS is flat within the configured thresholds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
