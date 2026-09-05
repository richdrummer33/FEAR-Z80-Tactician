#!/usr/bin/env python3
"""Compare ROM layouts for the new Polar cast-light endpoint field.

No candidate culling is accepted for runtime here; this only quantifies where
bytes are going so the first functional GG integration does not accidentally
turn a four-run light into a ~40 KiB duplicated point field.
"""
from __future__ import annotations

from collections import Counter

import cast_light_field as cl
import local_projection_field_poc as lp

ROOM_A_SIDS = frozenset((0, 1, 2, 3, 4, 14))
ROOM_B_SIDS = frozenset((7, 8, 9, 10, 11, 12, 13, 15, 16))


def receiver_relevant_mask(d: lp.D, gx: int, gy: int) -> int:
    corners, segs = lp.relevant(d, gx, gy)
    if not corners:
        return 0
    cs, ss = set(corners), set(segs)
    mask = 0
    for i, c in enumerate(cl.CASTS):
        room_sids = ROOM_A_SIDS if c.receiver_surface == cl.SURFACE_ROOM_A else ROOM_B_SIDS
        # Conservative topology hint only: retain if the caster is part of the
        # cell recipe, the receiver wall itself is part of it, or any boundary
        # belonging to the receiver surface is potentially visible.
        if c.caster_vid in cs or c.receiver_sid in ss or bool(room_sids & ss):
            mask |= 1 << i
    return mask


def main() -> None:
    d0 = lp.load(); casts = cl.derive_casts(d0); d, endpoint_ids = cl.extended_data(d0, casts)
    threshold = 4.0; min_q4 = 8
    cache: dict[tuple[int, int, int], int] = {}

    def endpoint_bytes(vid: int, gx: int, gy: int) -> int:
        k = (vid, gx, gy)
        if k not in cache:
            cache[k] = len(cl.serialize_endpoint(d, vid, gx, gy, threshold, min_q4))
        return cache[k]

    totals = Counter(); mask_hist = Counter(); active_casts = Counter()
    for gy in range(lp.GRID_H):
        for gx in range(lp.GRID_W):
            nonempty = d0.grid[gy * lp.GRID_W + gx] != 255
            allmask = 0x0F if nonempty else 0
            relmask = receiver_relevant_mask(d0, gx, gy)
            mask_hist[relmask] += 1
            totals['cells'] += 1
            # Every serialized cell needs one mask byte.
            for mode in ('all_both', 'all_hit', 'relevant_both', 'relevant_hit'):
                totals[mode] += 1
            for i, (caster, hit) in enumerate(endpoint_ids):
                cb = endpoint_bytes(caster, gx, gy)
                hb = endpoint_bytes(hit, gx, gy)
                if allmask & (1 << i):
                    totals['all_both'] += cb + hb
                    totals['all_hit'] += hb
                    active_casts['all'] += 1
                if relmask & (1 << i):
                    totals['relevant_both'] += cb + hb
                    totals['relevant_hit'] += hb
                    active_casts['relevant'] += 1

    print('=== POLAR CAST-LIGHT PACK COMPARISON ===')
    print(f"cells={totals['cells']} thresholdQ12={threshold:g} minLeafWorld={min_q4/16:g}")
    print(f"all-casts + duplicated caster/hit endpoints = {totals['all_both']} bytes")
    print(f"all-casts + hit endpoints only              = {totals['all_hit']} bytes")
    print(f"receiver-relevant + duplicated endpoints    = {totals['relevant_both']} bytes")
    print(f"receiver-relevant + hit endpoints only      = {totals['relevant_hit']} bytes")
    print(f"active cast refs: all={active_casts['all']} receiver-relevant={active_casts['relevant']}")
    print('receiver-relevant mask histogram:')
    for mask, n in sorted(mask_hist.items()):
        print(f"  0x{mask:02X}: {n} cells")
    print('IMPORTANT: receiver-relevant culling is a ROM-size candidate, not accepted until dense camera/yaw oracle proves no visible cast is omitted.')


if __name__ == '__main__':
    main()
