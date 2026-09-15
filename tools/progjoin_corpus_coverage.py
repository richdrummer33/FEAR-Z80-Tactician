#!/usr/bin/env python3
"""Measure what a PROGJOIN pose oracle covers, and whether it can be made denser.

Answers three questions that gate any attempt to raise compiled-path coverage
by baking more poses:

  vocabulary  how many distinct steps and (step, want) keys does this oracle
              produce, and how does that compare to the whole pose space
  reachable   do the poses a live trace actually visits appear in the oracle's
              sampled position grid at all
  headroom    would the resulting corpus still fit the sparse dispatch format,
              whose descriptor stores record-list offsets as a uint16

The third is the binding constraint and is cheap to evaluate, so check it before
spending a census run on a denser oracle. Validated against real packs: with the
matching --entries-per-key it predicts 18,107 B for the shipped corpus (actual
18,976) and 258,408 B for a 28,070-pose one (actual 262,315, which the packer
rejects outright).

  usage: progjoin_corpus_coverage.py <oracle.txt> [--traces CSV [CSV ...]]
"""
import argparse
import sys
from pathlib import Path

CHUNK_COLS = 6
PROFILE_FULL = 0

# coverage_potential_probe.c: positions are gx*CELL_Q4 + 32 plus one of four
# fixed sub-cell offsets, over a GRID_W x GRID_H walkable grid.
CELL_Q4 = 64
GRID_W, GRID_H = 48, 24
SUB_OFFSETS = ((0, 0), (7, 3), (3, 7), (11, 5))

# gg_progjoin_sparse_direct.py refuses above 0xfffe; records hold 4 bytes per
# semantic entry plus one 0xFF terminator per descriptor.
RECORD_CAP = 0xFFFE


def scan(path: Path):
    steps, keys = set(), set()
    runs = poses = 0
    with path.open() as fh:
        for line in fh:
            if not line.strip():
                continue
            f = line.split()
            nd = int(f[0])
            poses += 1
            for i in range(nd):
                b = 1 + i * 8
                if int(f[b + 4]) != PROFILE_FULL:
                    continue
                step, c0, c1 = int(f[b + 1]), int(f[b + 2]), int(f[b + 3])
                runs += 1
                steps.add(step)
                left = c1 - c0 + 1
                while left > 0:
                    want = CHUNK_COLS if left > CHUNK_COLS else left
                    keys.add((step, want))
                    left -= want
    return poses, runs, steps, keys


def on_sampled_grid(x: int, y: int) -> bool:
    for ox, oy in SUB_OFFSETS:
        dx, dy = x - 32 - ox, y - 32 - oy
        if dx % CELL_Q4 == 0 and dy % CELL_Q4 == 0 and 0 <= dx // CELL_Q4 < GRID_W and 0 <= dy // CELL_Q4 < GRID_H:
            return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("oracle", type=Path)
    ap.add_argument("--traces", nargs="*", type=Path, default=[],
                    help="polar_ab_profile CSVs whose visited positions to test")
    ap.add_argument("--entries-per-key", type=float, default=1.0,
                    help="semantic (base,rank) variants per (step,want). 1.0 is the "
                         "absolute floor; measured corpora ran 4.5 (shipped 2,486-pose "
                         "oracle) and 10.2 (28,070-pose oracle across all headings), so "
                         "the multiplier grows with density and the floor is optimistic")
    args = ap.parse_args()

    poses, runs, steps, keys = scan(args.oracle)
    print(f"oracle                 {args.oracle}")
    print(f"  poses dumped         {poses:,}")
    print(f"  FULL run-edges       {runs:,}")
    print(f"  distinct steps       {len(steps):,}")
    print(f"  distinct (step,want) {len(keys):,}")

    # Records hold 4 bytes per semantic entry plus one terminator per descriptor.
    entries = len(keys) * args.entries_per_key
    need = entries * 4 + len(keys)
    print()
    print("sparse dispatch format headroom")
    print(f"  uint16 record cap    {RECORD_CAP:,} B")
    print(f"  entries/key assumed  {args.entries_per_key:g}")
    print(f"  records needed       {need:>10,.0f} B  ({100.0 * need / RECORD_CAP:.0f}% of cap)")
    if need > RECORD_CAP:
        print(f"  VERDICT              {need / RECORD_CAP:.1f}x over cap; the dispatch format must be")
        print("                       widened before a corpus this dense can be baked at all")
    elif args.entries_per_key <= 1.0:
        print("  VERDICT              fits only at the 1-entry-per-key floor, which no real")
        print("                       corpus achieves; re-run with --entries-per-key")
    else:
        print("  VERDICT              representable")

    if args.traces:
        print()
        print("live trace positions against the oracle's sampled grid")
        import csv
        total = hit = 0
        for t in args.traces:
            with t.open() as fh:
                rows = list(csv.DictReader(fh))
            pts = {(int(r["x_q4"]), int(r["y_q4"])) for r in rows}
            h = sum(1 for p in pts if on_sampled_grid(*p))
            total += len(pts)
            hit += h
            print(f"  {t.name:<40} {h}/{len(pts)} positions sampled")
        print(f"  {'TOTAL':<40} {hit}/{total}")
        if total and hit == 0:
            print("  VERDICT              no visited position is in the oracle; every compiled")
            print("                       hit is an incidental step collision, not coverage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
