#!/usr/bin/env python3
"""Report target-side headroom for a Doomguy random-access playable pack."""

import argparse
import collections
import math
import pathlib
import struct

from doomguy_playable_pack_to_c import parse_pack

UPLOAD_CAP = 48
GG_HZ = 60.0
PATTERN_BYTES = 32
BASE_PATTERNS = 3
PATTERN_TILE_LIMIT = 384
NAME_TABLE_BYTES = 2048
NAME_TABLE_COUNT = 2


def state_patterns(blob):
    return struct.unpack_from("<H", blob, 0)[0]


def percentile(values, q):
    values = sorted(values)
    if not values:
        return 0
    i = int(round((len(values) - 1) * q))
    return values[max(0, min(len(values) - 1, i))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("--upload-cap", type=int, default=UPLOAD_CAP)
    args = ap.parse_args()
    if args.upload_cap < 1:
        raise SystemExit("upload cap must be positive")

    meta, _lut, dictionary, states = parse_pack(args.pack)
    pats = [state_patterns(s) for s in states]
    pattern_phases = [math.ceil(n / args.upload_cap) for n in pats]
    # Two invisible nine-row name-table uploads are mandatory in the current
    # atomic page-flip transport. The R2 reveal itself does not consume another
    # wait; it occurs after the second half has been written.
    publish_vblanks = [p + 2 for p in pattern_phases]
    hist = collections.Counter(publish_vblanks)

    avg_patterns = sum(pats) / len(pats)
    avg_vblanks = sum(publish_vblanks) / len(publish_vblanks)
    peak = max(pats)
    peak_vb = max(publish_vblanks)

    reserved_pattern_slots = (
        BASE_PATTERNS + 2 * meta["pool_size"] + meta["dict_count"]
    )
    unassigned_slots = PATTERN_TILE_LIMIT - reserved_pattern_slots
    if unassigned_slots < 0:
        raise SystemExit("VRAM accounting overcommitted")

    print("DOOM_HERO_HEADROOM v1")
    print(
        f"states={len(states)} positions={meta['positions']} yaws={meta['yaws']} "
        f"pack_bytes={pathlib.Path(args.pack).stat().st_size}"
    )
    print(
        f"patterns avg={avg_patterns:.2f} p50={percentile(pats,0.50)} "
        f"p95={percentile(pats,0.95)} peak={peak} pool_size={meta['pool_size']} "
        f"peak_pool_slack={meta['pool_size']-peak}"
    )
    print(
        f"dictionary slots={meta['dict_count']} bytes={len(dictionary)} "
        f"dynamic_pool_slots_each={meta['pool_size']} "
        f"unassigned_pattern_slots={unassigned_slots}"
    )
    print(
        f"vram pattern_bytes={PATTERN_TILE_LIMIT*PATTERN_BYTES} "
        f"name_table_bytes={NAME_TABLE_COUNT*NAME_TABLE_BYTES} "
        f"total_accounted_bytes={PATTERN_TILE_LIMIT*PATTERN_BYTES + NAME_TABLE_COUNT*NAME_TABLE_BYTES}"
    )
    print(
        f"publication upload_cap={args.upload_cap} avg_vblanks={avg_vblanks:.2f} "
        f"peak_vblanks={peak_vb} theoretical_avg_hz={GG_HZ/avg_vblanks:.2f} "
        f"theoretical_peak_case_hz={GG_HZ/peak_vb:.2f}"
    )
    print(
        "publication_histogram "
        + " ".join(f"vblank{vb}={hist[vb]}" for vb in sorted(hist))
    )
    for budget in (2, 3, 4, 5, 6):
        n = sum(1 for v in publish_vblanks if v <= budget)
        print(
            f"within_{budget}_vblanks={n}/{len(states)} "
            f"pct={100.0*n/len(states):.1f} nominal_hz={GG_HZ/budget:.1f}"
        )

    # A two-VBlank transition is the current transport's 30 Hz lower bound
    # after all pattern data is already resident/preloaded: one VBlank per map
    # half. This makes the optimization target explicit instead of pretending
    # the full-state pack itself is already a 30 Hz codec.
    zero_pattern = sum(1 for n in pats if n == 0)
    one_pattern_phase = sum(1 for p in pattern_phases if p <= 1)
    print(
        f"30hz_transport_floor_vblanks=2 states_with_zero_patterns={zero_pattern} "
        f"states_with_at_most_one_pattern_phase={one_pattern_phase}"
    )


if __name__ == "__main__":
    main()
