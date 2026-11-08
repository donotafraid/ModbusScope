#!/usr/bin/env python3
"""Evaluate the SLO gate for the archived benchmark medians.

Usage:
  python3 scripts/check_bench_slo.py bench-results/<stamp>_slo/median.txt \
      --profile strict
  python3 scripts/check_bench_slo.py bench-results/<stamp>_slo/median.txt \
      --profile capacity

strict   is the "linear scaling" gate: 8T p99 <= 3*1T p99 + RTT and
         8T throughput >= 1T throughput. A shared, serialized client is not
         expected to pass; the 2026-10-28 data fails it deliberately.

capacity is the deployment floor for one shared, serialized client:
         8T p99 <= 50ms, 8T calls/s >= 3000 and >= 0.45*1T. It is NOT a claim
         of linear scaling; see docs/benchmark.md.

The parser accepts both formats produced in this repository:

  === 1t: threads=1 cores=0 ops=100000 ===
  p99: median=0.326  values=...

  --- 1t (median over 5 runs) ---
  p50: 0.141
  p99: 0.262
  calls_s: 6724.2

A section label is matched greedily (`8t_ptc_r80` is not folded into `8t`).
Missing 1t/8t sections or metrics are a hard error: the gate never guesses a
default and never turns an unparsed file into PASS.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SECTION_RE = re.compile(r"^(?:===|---)\s+([0-9]+t[a-z0-9_]*)\b")
METRIC_RE = re.compile(r"^([a-z0-9_.]+):\s*(?:median=)?([0-9.]+)")


def parse(path: Path) -> dict[str, dict[str, float]]:
    sections: dict[str, dict[str, float]] = {}
    current: str | None = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        m = SECTION_RE.match(line)
        if m:
            current = m.group(1)
            sections.setdefault(current, {})
            continue
        if current is None:
            continue
        m = METRIC_RE.match(line)
        if m:
            sections[current][m.group(1)] = float(m.group(2))
    return sections


def require(sections: dict[str, dict[str, float]], label: str,
            metric: str) -> float:
    if label not in sections:
        raise SystemExit(f"missing section {label} in input")
    if metric not in sections[label]:
        raise SystemExit(f"missing {label}.{metric} in input")
    return sections[label][metric]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("--profile", choices=("strict", "capacity", "fixed-rate"),
                    default="strict",
                    help="fixed-rate gates p99 only; throughput is intentionally "
                         "capped by --rate, so the 1T throughput comparison "
                         "does not apply")
    ap.add_argument("--rtt-ms", type=float, default=1.0,
                    help="RTT allowance for the strict 8T p99 bound")
    ap.add_argument("--target", default="8t",
                    help="section to gate (default 8t; use 8t_ptc for the "
                         "per-thread-client baseline)")
    ap.add_argument("--expected-rate", type=float, default=0.0,
                    help="fixed-rate profile: expected calls/s; fail when the "
                         "archived calls_s differs by more than 5%%")
    ap.add_argument("--require-errors", action="store_true",
                    help="require an errors field in every gated section and "
                         "fail unless it is 0; run_slo_bench.sh writes this "
                         "field. Use it for new archives.")
    ap.add_argument("--ci", type=Path, default=None,
                    help="path to ci.txt (default: ci.txt next to median.txt). "
                         "Only read when --ci-upper-ms is set.")
    ap.add_argument("--ci-upper-ms", type=float, default=None,
                    help="also gate the bootstrap upper bound for 1T p99.9 "
                         "against this value; requires --ci (or ci.txt beside "
                         "the input) and a line produced by bootstrap_ci.py")
    ap.add_argument("--min-runs", type=int, default=0,
                    help="with --ci-upper-ms, require at least this many runs "
                         "in the 1T p999 CI line (prevents a 5-run archive from "
                         "being presented as a 15-run result)")
    args = ap.parse_args()

    s = parse(args.input)
    rows: list[tuple[str, str, bool]] = []

    def add(name: str, detail: str, ok: bool) -> None:
        rows.append((name, detail, ok))

    p999_1t = require(s, "1t", "p999")
    add("1T p99.9 <= 1ms", f"{p999_1t:.3f}ms", p999_1t <= 1.0)

    target = args.target

    if args.ci_upper_ms is not None:
        ci_path = args.ci if args.ci is not None else args.input.with_name("ci.txt")
        if not ci_path.is_file():
            raise SystemExit(f"missing CI file {ci_path}; pass --ci or run "
                             f"bootstrap_ci.py next to the archive")
        ci_text = ci_path.read_text(encoding="utf-8")
        ci_match = re.search(
            r"^1t p999: median=([0-9.]+) CI=\[([0-9.]+),([0-9.]+)\] n=([0-9]+)$",
            ci_text, re.MULTILINE)
        if not ci_match:
            raise SystemExit(f"missing '1t p999: median=... CI=[...,...] n=...' "
                             f"line in {ci_path}")
        ci_upper = float(ci_match.group(3))
        ci_runs = int(ci_match.group(4))
        add("1T p99.9 CI upper <= limit",
            f"{ci_upper:.3f}ms <= {args.ci_upper_ms:.3f}ms",
            ci_upper <= args.ci_upper_ms)
        if args.min_runs > 0:
            add("1T p999 sample size",
                f"n={ci_runs} >= {args.min_runs}", ci_runs >= args.min_runs)

    # A median file without an errors field cannot prove the run was clean.
    # New archives include it (run_slo_bench.sh sums errors over the raw
    # runs); --require-errors rejects old/ambiguous evidence.
    for label in ("1t", target):
        if label not in s:
            continue
        e = s[label].get("errors")
        if e is None:
            if args.require_errors:
                raise SystemExit(f"missing {label}.errors in input "
                                 f"(re-run with run_slo_bench.sh)")
            continue
        add(f"{label} errors == 0", f"{e:.0f}", e == 0)

    p99_1t = require(s, "1t", "p99")
    p99_8t = require(s, target, "p99")
    calls_1t = require(s, "1t", "calls_s")
    calls_8t = require(s, target, "calls_s")

    if args.profile == "strict":
        bound = 3.0 * p99_1t + args.rtt_ms
        add(f"{target} p99 <= 3*1T p99 + RTT",
            f"{p99_8t:.3f}ms <= {bound:.3f}ms", p99_8t <= bound)
        add(f"{target} calls/s >= 1T calls/s",
            f"{calls_8t:.1f} >= {calls_1t:.1f}", calls_8t >= calls_1t)
    elif args.profile == "fixed-rate":
        bound = 3.0 * p99_1t + args.rtt_ms
        add(f"{target} p99 <= 3*1T p99 + RTT",
            f"{p99_8t:.3f}ms <= {bound:.3f}ms", p99_8t <= bound)
        if args.expected_rate > 0:
            low = args.expected_rate * 0.95
            high = args.expected_rate * 1.05
            add(f"{target} offered rate within 5%",
                f"{calls_8t:.1f} in [{low:.1f}, {high:.1f}]",
                low <= calls_8t <= high)
    else:
        add(f"{target} p99 <= 50ms", f"{p99_8t:.3f}ms", p99_8t <= 50.0)
        add(f"{target} calls/s >= 3000", f"{calls_8t:.1f}", calls_8t >= 3000.0)
        add(f"{target} calls/s >= 0.45*1T",
            f"{calls_8t:.1f} >= {0.45 * calls_1t:.1f}",
            calls_8t >= 0.45 * calls_1t)

    width = max(len(r[0]) for r in rows)
    print(f"profile: {args.profile}")
    for name, detail, ok in rows:
        print(f"  {'PASS' if ok else 'FAIL'}  {name:<{width}}  {detail}")

    # Per-thread clients are a diagnostic baseline, not the shipped shared-
    # instance contract. Print it when archived, never gate on it.
    if "8t_ptc" in s:
        print(f"\nINFO 8t_ptc (per-thread clients): p50={s['8t_ptc'].get('p50', 0):.3f}ms "
              f"p99={s['8t_ptc'].get('p99', 0):.3f}ms "
              f"calls_s={s['8t_ptc'].get('calls_s', 0):.1f}  (not a gate)")

    failed = [r for r in rows if not r[2]]
    print(f"\n{'PASS' if not failed else 'FAIL'}: "
          f"{len(rows) - len(failed)}/{len(rows)} checks")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
