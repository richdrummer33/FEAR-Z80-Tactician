#!/usr/bin/env python3
"""Align a sampled GG name-table dump against the exact host-baked reference.

Each frame is the complete 20x18 Game Gear name table: 360 uint16 words =
720 bytes.  The reference may contain more frames than the sampled runtime
dump, so we search every contiguous alignment rather than guessing the
warm-up/phase offset.
"""
from __future__ import annotations

import argparse
from pathlib import Path

FRAME_BYTES = 20 * 18 * 2
WORDS = 20 * 18

def frames(path: Path) -> list[bytes]:
    data = path.read_bytes()
    if len(data) % FRAME_BYTES:
        raise SystemExit(f"{path}: {len(data)} bytes is not a multiple of {FRAME_BYTES}")
    return [data[i:i + FRAME_BYTES] for i in range(0, len(data), FRAME_BYTES)]

def word_diff(a: bytes, b: bytes) -> int:
    return sum(a[i:i+2] != b[i:i+2] for i in range(0, FRAME_BYTES, 2))

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("sample")
    ap.add_argument("--label", default="sample")
    ap.add_argument("--require-exact", action="store_true")
    args = ap.parse_args()

    ref = frames(Path(args.reference))
    got = frames(Path(args.sample))
    if not got:
        raise SystemExit("sample contains no frames")
    if len(got) > len(ref):
        raise SystemExit(f"sample has {len(got)} frames but reference only {len(ref)}")

    exact_offsets = []
    best = None
    best_detail = None
    for start in range(len(ref) - len(got) + 1):
        diffs = [word_diff(g, r) for g, r in zip(got, ref[start:start + len(got)])]
        total = sum(diffs)
        key = (total, max(diffs), start)
        if best is None or key < best:
            best = key
            best_detail = diffs
        if total == 0:
            exact_offsets.append(start)

    assert best is not None and best_detail is not None
    total, max_diff, start = best
    mean = total / len(got)
    exact_frames = sum(d == 0 for d in best_detail)
    print(
        f"{args.label}: sampled_frames={len(got)} reference_frames={len(ref)} "
        f"best_start={start} total_diff_words={total} mean={mean:.3f}/{WORDS} "
        f"max={max_diff} exact_frames={exact_frames}/{len(got)}"
    )
    if exact_offsets:
        print(f"{args.label}: EXACT contiguous alignment PASS offsets={exact_offsets}")
        return 0

    worst_i = max(range(len(best_detail)), key=best_detail.__getitem__)
    first_bad = next(i for i, d in enumerate(best_detail) if d)
    print(
        f"{args.label}: no exact alignment; first_bad_sample={first_bad} "
        f"reference={start + first_bad} diff_words={best_detail[first_bad]}; "
        f"worst_sample={worst_i} reference={start + worst_i} diff_words={best_detail[worst_i]}"
    )
    return 1 if args.require_exact else 0

if __name__ == "__main__":
    raise SystemExit(main())
