#!/usr/bin/env python3
"""Fail CI when the documented GG parity rungs are not present in generated source.

This check deliberately inspects the *post-transform* materializer rather than
assuming the checked-in baseline assembly is the ROM implementation.

It is not a correctness oracle; exact-ROM hash/profiler tests remain responsible
for semantic and cycle validation. This script catches integration bookkeeping
failures: skipped patch steps, stale workflow composition, or a claim that a
feature is integrated when its defining source markers never reach the assembler.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys


REQUIRED = {
    "run-carry": (
        "CARRY_EDGE_A",
        "Only the right endpoint is new.  It becomes next column's left.",
    ),
    "border-hoist": (
        "BORDERHOIST",
        "Interior columns carry no border.",
    ),
    "edge-local-dda": (
        "DDA_LOCAL",
        "caller carries left_y-row*8 down the edge rows",
    ),
    "interior-claim-walk": (
        "CLAIMWALK_A",
        "r_fill_claim_mask$",
        "r_fill_claim_byte$",
    ),
}

BASELINE_ANTI_MARKERS = {
    "old-run-rederives-left-endpoint": (
        "; Left profile endpoints from half=invl>>1.",
        "; Right profile endpoints from half=invr>>1.",
        "run_geom_loop$:",
    ),
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "path",
        nargs="?",
        default="src/tilesector_polar_materialize_gg.s",
        help="post-transform assembly source",
    )
    ap.add_argument(
        "--allow-baseline-run-loop",
        action="store_true",
        help="do not reject the original per-column run loop markers",
    )
    ns = ap.parse_args()

    p = Path(ns.path)
    if not p.is_file():
        print(f"PARITY_FAIL missing generated materializer: {p}", file=sys.stderr)
        return 2

    text = p.read_text()
    bad = False

    print(f"PARITY_SOURCE={p}")
    for feature, markers in REQUIRED.items():
        missing = [m for m in markers if m not in text]
        if missing:
            bad = True
            print(
                f"PARITY_MISSING feature={feature} markers={missing!r}",
                file=sys.stderr,
            )
        else:
            print(f"PARITY_PRESENT feature={feature}")

    if not ns.allow_baseline_run_loop:
        for name, markers in BASELINE_ANTI_MARKERS.items():
            if all(m in text for m in markers):
                bad = True
                print(
                    f"PARITY_STALE feature={name}: original two-endpoint per-column "
                    "run walker still appears intact",
                    file=sys.stderr,
                )

    if bad:
        print("PARITY_RESULT=FAIL", file=sys.stderr)
        return 1

    print("PARITY_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
