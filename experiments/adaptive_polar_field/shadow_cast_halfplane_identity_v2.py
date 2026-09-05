#!/usr/bin/env python3
"""Receiver-owned version of the cast-halfplane identity probe.

The first probe intentionally exposed an important topology rule: coplanar
surfaces are not interchangeable receivers. Room A and the connector both live
at z=0, but the upper/lower window-edge shadows belong to Room A only.  A cast
run therefore carries an authored receiver-surface owner, exactly as a Polar
wall run carries its segment/surface owner.

This wrapper keeps the original dense sweep and cast-line derivation, replacing
only receiver selection. No new per-pixel receiver mask is proposed for the GG;
the existing Polar geometry/recipe ownership is the runtime source of truth.
"""
from __future__ import annotations

import numpy as np

import shadow_cast_halfplane_identity as probe

oracle = probe.oracle


def receiver_owned_frame(cx: float, cy: float, yaw: int):
    wx4, wy4, d4 = oracle.camera_floor_arrays(cx, cy, yaw, oracle.ROOM_B_FLOOR_Z)
    room_b = oracle.points_in_poly(wx4, wy4, oracle.ROOM_B)
    e4 = room_b & (d4 > 0)

    wx0, wy0, d0 = oracle.camera_floor_arrays(cx, cy, yaw, 0.0)
    room_a = oracle.points_in_poly(wx0, wy0, oracle.ROOM_A)
    connector = oracle.points_in_poly(wx0, wy0, oracle.CONNECTOR)
    e0 = (room_a | connector) & (d0 > 0)

    use4 = e4 & (~e0 | (d4 < d0))
    use0 = e0 & ~use4

    # P0/P1 are explicitly owned by ROOM_A.  The connector is coplanar but is
    # not their receiver and must remain untouched by those cast half-planes.
    sh0 = np.zeros_like(use0)
    for pi in probe.PLANE_GROUPS[0.0]:
        sh0 |= probe.halfplane(wx0, wy0, probe.CASTS[pi])
    sh0 &= room_a

    # P2/P3 are owned by the raised ROOM_B floor.
    sh4 = np.zeros_like(use4)
    for pi in probe.PLANE_GROUPS[4.0]:
        sh4 |= probe.halfplane(wx4, wy4, probe.CASTS[pi])
    sh4 &= room_b

    eligible = np.zeros((oracle.H, oracle.W), dtype=bool)
    lit = np.zeros_like(eligible)
    eligible[73:] = use0 | use4
    shadow = (use0 & sh0) | (use4 & sh4)
    lit[73:] = (use0 | use4) & ~shadow
    return eligible, lit


probe.predicted_frame = receiver_owned_frame

if __name__ == '__main__':
    raise SystemExit(probe.main())
