#!/usr/bin/env python3
"""Adjudicate the playable PROGJOIN live A/B rung.

Reads the per-update traces produced by polar_ab_profile for the three ROM
variants built by tools/build_progjoin_live_rom.sh and applies the graduation
gate from the parity contract:

  exactness  baseline vs progjoin must agree on player state and the complete
             20x18 name-table hash on EVERY measured logical update
  control    progjoin-stats must agree with progjoin, so the counters are
             proven not to perturb what they measure
  coverage   the compiled path must actually run; a run where everything fell
             back to the legacy edge solver is not parity evidence
  cycles     whole-update cost on both ROMs, reported either way

Exactness and the control are hard failures. Coverage and cycles are reported
and compared against thresholds, but a coverage or speed shortfall is a result
about the executor, not a broken harness, so it is surfaced rather than thrown
unless --require-speedup / --min-coverage say otherwise.
"""
import argparse
import csv
import statistics
import sys
from pathlib import Path

COMPARE = ("x_q4", "y_q4", "yaw", "map_fnv64")
FALLBACKS = ("fb_depth", "fb_sel_top", "fb_sel_bot", "fb_play_top", "fb_play_bot")
MISSES = ("miss_step", "miss_desc", "miss_rank", "miss_shape")


def load(root: Path, variant: str, scenario: str):
    path = root / f"{variant}-{scenario}.csv"
    with path.open() as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        sys.exit(f"LIVE_AB_FAIL {path}: no measured updates")
    return rows


def stat(rows, name):
    key = f"g_pj_stat_{name}"
    if key not in rows[-1]:
        return None
    return int(rows[-1][key])


def compare(a, b, label, scenario, failures):
    if len(a) != len(b):
        failures.append(f"{scenario}/{label}: {len(a)} vs {len(b)} updates")
        return
    bad = [
        (i, k, a[i][k], b[i][k])
        for i in range(len(a))
        for k in COMPARE
        if a[i][k] != b[i][k]
    ]
    if bad:
        i, k, x, y = bad[0]
        failures.append(
            f"{scenario}/{label}: {len(bad)} field mismatches, first at update {i} {k} {x} != {y}"
        )
        print(f"  {label:<26} MISMATCH ({len(bad)} fields, first update {i} {k})")
    else:
        print(f"  {label:<26} EXACT over {len(a)} updates")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("--scenarios", nargs="+", default=["roomA-turn", "roomA-forward"])
    ap.add_argument("--min-coverage", type=float, default=0.0,
                    help="fail if compiled-run share of attempts falls below this (0-1)")
    ap.add_argument("--require-speedup", action="store_true",
                    help="fail if the compiled path is not faster than baseline")
    args = ap.parse_args()

    failures = []
    for scenario in args.scenarios:
        print(f"--- {scenario} ---")
        base = load(args.root, "baseline", scenario)
        prog = load(args.root, "progjoin", scenario)
        stats = load(args.root, "progjoin-stats", scenario)

        compare(base, prog, "baseline vs progjoin", scenario, failures)
        compare(prog, stats, "progjoin vs +stats", scenario, failures)

        bm = statistics.mean(int(r["loop_T"]) for r in base)
        pm = statistics.mean(int(r["loop_T"]) for r in prog)
        delta = 100.0 * (pm - bm) / bm
        print(f"  cycles/update              baseline {bm:>12,.1f}T   progjoin {pm:>12,.1f}T   {delta:+.2f}%")

        attempts = stat(stats, "attempt")
        ok = stat(stats, "ok")
        if attempts is None or ok is None:
            failures.append(f"{scenario}: progjoin-stats ROM exposes no counters")
            continue

        share = (ok / attempts) if attempts else 0.0
        print(f"  compiled run-edges         {ok}/{attempts} attempts ({100 * share:.1f}%)")
        for name in FALLBACKS:
            v = stat(stats, name)
            if v:
                print(f"    fallback {name:<12} {v}")
        for name in MISSES:
            v = stat(stats, name)
            if v:
                print(f"    dispatch {name:<12} {v}")

        # A partial write followed by a fallback would leave the legacy path to
        # redraw over cells the compiled edge had already emitted.
        for name in ("fb_play_top", "fb_play_bot"):
            v = stat(stats, name)
            if v:
                failures.append(
                    f"{scenario}: {v} playback refusals after cells were already written "
                    f"({name}) - breaks the atomic-replacement model"
                )

        control = [stat(load(args.root, "baseline", scenario), n) for n in ("attempt", "ok")]
        if any(c for c in control if c):
            failures.append(f"{scenario}: baseline ROM recorded compiled-path activity {control}")
        else:
            print("  baseline control           counters zero (no compiled playback)")

        if attempts == 0:
            failures.append(f"{scenario}: the compiled path was never attempted")
        elif ok == 0:
            failures.append(f"{scenario}: every run-edge fell back; an exact match proves nothing here")
        if share < args.min_coverage:
            failures.append(
                f"{scenario}: compiled coverage {100 * share:.1f}% below required {100 * args.min_coverage:.1f}%"
            )
        if args.require_speedup and delta >= 0.0:
            failures.append(f"{scenario}: compiled path is {delta:+.2f}% on cycles, not faster")

    print()
    if failures:
        for f in failures:
            print(f"LIVE_AB_FAIL {f}")
        return 1
    print("LIVE_AB_EXACT player state and 20x18 name-table hash agree on every measured update")
    return 0


if __name__ == "__main__":
    sys.exit(main())
