#!/usr/bin/env python3
"""Audit whether live GG ownership tags are invariant under PROGJOIN body reuse.

The sparse pack deduplicates selected-count bodies by their existing payload:
    count, [word16, nt_delta16] * count

For live rendering we want to augment each cell with a compact ownership tag.
Before changing the body format, prove whether one tagged body can still be
shared everywhere its untagged payload is reused.

The proposed tag is:
    bit7     visible (row 0..17, col 0..19)
    bits6..4 row & 7
    bits1..0 coverage-coordinate delta from the previous cell (0..3)
    bits3..2 spare

The first cell of each selected body uses delta=0. Its absolute coverage seed is
runtime state derived from the current destination cursor and therefore is not
part of the deduplicated body. Consecutive chunks carry that runtime cursor.

This audit groups all observed uses by the exact tuned sparse-body payload and
counts distinct tag byte strings. Zero conflicts means adding tags does not
force any extra body variants over the observed FULL corpus.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
from pathlib import Path

C = 6
FULL = {0, 2}
COLS = 20
ROW_BYTES = COLS * 2


def u16(b: bytes, o: int) -> int:
    return b[o] | (b[o + 1] << 8)


def s16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def iter_cases(path: Path):
    for ln in path.read_text().splitlines():
        if not ln.strip():
            continue
        f = [int(x) for x in ln.split()]
        i = 0
        fam, step, iq0, c0, ncol, shade = f[i:i + 6]
        i += 6
        first = f[i]
        i += 1
        ncs = f[i]
        i += 1
        wants = f[i:i + ncs]
        i += ncs
        nc = f[i]
        i += 1
        # Skip renderer expected pairs; this audit only needs dispatch + cursor.
        yield fam, step, iq0, c0, ncol, first, wants


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("windows", type=Path)
    ap.add_argument("out", type=Path)
    a = ap.parse_args()

    signatures: dict[bytes, set[bytes]] = collections.defaultdict(set)
    uses = collections.Counter()
    first_seed_min = 999
    first_seed_max = -999
    cell_count = 0
    offscreen = 0
    bad_delta = 0

    for wd in sorted(p for p in a.windows.iterdir() if p.is_dir() and p.name.startswith("w")):
        sm = (wd / "progjoin_stepmap.bin").read_bytes()
        ds = (wd / "progjoin_desc.bin").read_bytes()
        th = (wd / "progjoin_thresh.bin").read_bytes()
        bl = (wd / "progjoin_blocks.bin").read_bytes()
        bo = (wd / "progjoin_bodies.bin").read_bytes()

        for fam, step, iq0, c0, ncol, first, wants in iter_cases(wd / "progjoin_cases.txt"):
            if fam not in FULL:
                continue
            slot = sm[step + 2048]
            chunk_start = 0
            cursor = first

            for want in wants:
                iq = s16(iq0 + s16(chunk_start * step))
                acc = iq + 32
                base = (acc >> 7) & 7
                u = acc & 127
                rank = sum(1 for t in th[slot * 8:slot * 8 + C + 1] if u >= t)
                di = ((slot * 32) + fam * C + want - 1) * 2
                block = u16(ds, di)
                body = u16(bl, block + (base * 8 + rank) * 2)
                count = bo[body + want]
                p = body + C + 1

                payload = bytes([count]) + bo[p:p + count * 4]
                tags = bytearray()
                prev_cov = None

                for _ in range(count):
                    # Destination cursor is a byte offset in the unclipped 20-column
                    # row-major name-table program coordinate system.
                    row = cursor // ROW_BYTES
                    rem = cursor - row * ROW_BYTES
                    if rem & 1:
                        raise SystemExit(f"odd destination byte offset {cursor}")
                    col = rem // 2
                    cov = col * 3 + (row // 8)
                    visible = 0 <= row < 18 and 0 <= col < 20
                    if not visible:
                        offscreen += 1
                    rb = row & 7
                    if prev_cov is None:
                        dd = 0
                        first_seed_min = min(first_seed_min, cov)
                        first_seed_max = max(first_seed_max, cov)
                    else:
                        dd = cov - prev_cov
                        if dd < 0 or dd > 3:
                            bad_delta += 1
                    tag = (0x80 if visible else 0) | ((rb & 7) << 4) | (dd & 3)
                    tags.append(tag)
                    prev_cov = cov
                    delta = s16(u16(bo, p + 2))
                    cursor += 1 + delta
                    p += 4
                    cell_count += 1

                signatures[payload].add(bytes(tags))
                uses[payload] += 1
                chunk_start += want

    conflict_bodies = {k: v for k, v in signatures.items() if len(v) != 1}
    conflict_uses = sum(uses[k] for k in conflict_bodies)
    unique_untagged_bytes = sum(len(k) for k in signatures)
    unique_tagged_bytes_no_padding = sum(len(k) + len(next(iter(v))) for k, v in signatures.items() if len(v) == 1)

    examples = []
    for payload, variants in sorted(conflict_bodies.items(), key=lambda kv: (-len(kv[1]), hashlib.sha1(kv[0]).hexdigest()))[:12]:
        examples.append({
            "body_sha1": hashlib.sha1(payload).hexdigest(),
            "uses": uses[payload],
            "tag_variants": len(variants),
            "cell_count": payload[0],
            "variant_hex": [x.hex() for x in list(variants)[:4]],
        })

    report = {
        "observed_tuned_bodies": len(signatures),
        "observed_body_uses": sum(uses.values()),
        "cells": cell_count,
        "offscreen_cells": offscreen,
        "bad_cov_delta_count": bad_delta,
        "first_body_cell_cov_seed_min": first_seed_min,
        "first_body_cell_cov_seed_max": first_seed_max,
        "conflicting_untagged_bodies": len(conflict_bodies),
        "uses_of_conflicting_bodies": conflict_uses,
        "tag_invariant_under_body_dedup": not conflict_bodies and bad_delta == 0,
        "unique_untagged_payload_bytes": unique_untagged_bytes,
        "unique_tagged_payload_bytes_if_invariant": unique_tagged_bytes_no_padding if not conflict_bodies else None,
        "tag_growth_bytes_if_invariant": (unique_tagged_bytes_no_padding - unique_untagged_bytes) if not conflict_bodies else None,
        "tag_format": "bit7 visible; bits6..4 row_bit; bits1..0 cov_delta; bits3..2 spare; first-cell delta=0",
        "seed_policy": "derive absolute coverage seed from each selected body's current destination cursor at runtime",
        "conflict_examples": examples,
    }
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, indent=2) + "\n")

    print("=== GG PROGJOIN OWNERSHIP-TAG DEDUP AUDIT ===")
    print(f"tuned bodies / uses       {len(signatures):,} / {sum(uses.values()):,}")
    print(f"cells / offscreen         {cell_count:,} / {offscreen:,}")
    print(f"body seed range           {first_seed_min} .. {first_seed_max}")
    print(f"bad coverage deltas       {bad_delta}")
    print(f"conflicting bodies        {len(conflict_bodies):,}")
    print(f"conflicting body uses     {conflict_uses:,}")
    if not conflict_bodies and bad_delta == 0:
        print(f"unique payload growth     +{unique_tagged_bytes_no_padding - unique_untagged_bytes:,} bytes")
        print("GATE_INVARIANCE_PASS")
        return 0
    print("GATE_INVARIANCE_FAIL")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
