#!/usr/bin/env python3
"""Prove the accepted floor shadows need only four directed cast boundaries.

For the current static point light, each accepted hard-shadow polygon has one
new edge: caster vertex -> receiver-wall hit.  The remaining polygon edges are
existing receiver-floor boundaries.  Therefore the stronger Polar-style
representation should be:

    existing receiver floor region INTERSECT directed cast half-plane

rather than a separately projected/rasterised shadow polygon.

This probe evaluates that identity at exact per-pixel world-floor positions for
arbitrary cameras.  It uses the mature vector-light oracle only as the truth
source.  Zero mismatch means runtime lighting does not need to project the
closure edges at all: geometry already owns them.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import numpy as np

import floor_shadow_edge_field_poc as base

oracle = base.oracle

# Accepted polygon -> receiver surface membership. P0/P1 live on room-A/
# connector floor z=0; P2/P3 live on raised room-B floor z=4.
PLANE_GROUPS = {
    0.0: (0, 1),
    4.0: (2, 3),
}


def line_coeff(a: base.VRef, b: base.VRef) -> tuple[float, float, float]:
    ax, ay = a.x_q4 / 16.0, a.y_q4 / 16.0
    bx, by = b.x_q4 / 16.0, b.y_q4 / 16.0
    # A*x+B*y+C = cross((b-a),(p-a)).
    A = -(by - ay)
    B = bx - ax
    C = -A * ax - B * ay
    return A, B, C


def cast_edge(poly: tuple[base.VRef, ...]) -> tuple[base.VRef, base.VRef]:
    """The unique edge not wholly contained by an authored scene segment."""
    import shadow_receiver_chain_poc as chain
    out = []
    for i, a in enumerate(poly):
        b = poly[(i + 1) % len(poly)]
        if not chain.edge_world_sids(a, b):
            out.append((a, b))
    if len(out) != 1:
        raise RuntimeError(f"expected one cast edge, got {len(out)}")
    return out[0]


def directed_cast(poly: tuple[base.VRef, ...]):
    a, b = cast_edge(poly)
    A, B, C = line_coeff(a, b)
    # Orient the inequality using polygon centroid. The cast boundary plus the
    # existing receiver region must select the accepted polygon interior.
    cx = sum(v.x_q4 for v in poly) / (16.0 * len(poly))
    cy = sum(v.y_q4 for v in poly) / (16.0 * len(poly))
    s = A * cx + B * cy + C
    if abs(s) < 1e-12:
        raise RuntimeError("degenerate centroid on cast line")
    keep_positive = s > 0.0
    return A, B, C, keep_positive, a.name, b.name


CASTS = [directed_cast(p) for p, _ in base.POLYS]


def halfplane(wx: np.ndarray, wy: np.ndarray, cast) -> np.ndarray:
    A, B, C, keep_positive, _, _ = cast
    v = A * wx + B * wy + C
    # Boundary-inclusive, matching the oracle's floor polygon semantics.
    return v >= -1e-9 if keep_positive else v <= 1e-9


def predicted_frame(cx: float, cy: float, yaw: int):
    # Recreate the exact receiver selection from the oracle, but replace all
    # blocker/light tests with four directed cast half-planes.
    wx4, wy4, d4 = oracle.camera_floor_arrays(cx, cy, yaw, oracle.ROOM_B_FLOOR_Z)
    e4 = oracle.points_in_poly(wx4, wy4, oracle.ROOM_B) & (d4 > 0)
    wx0, wy0, d0 = oracle.camera_floor_arrays(cx, cy, yaw, 0.0)
    e0 = (oracle.points_in_poly(wx0, wy0, oracle.ROOM_A) |
          oracle.points_in_poly(wx0, wy0, oracle.CONNECTOR)) & (d0 > 0)
    use4 = e4 & (~e0 | (d4 < d0))
    use0 = e0 & ~use4

    sh0 = np.zeros_like(use0)
    for pi in PLANE_GROUPS[0.0]:
        sh0 |= halfplane(wx0, wy0, CASTS[pi])
    sh4 = np.zeros_like(use4)
    for pi in PLANE_GROUPS[4.0]:
        sh4 |= halfplane(wx4, wy4, CASTS[pi])

    eligible = np.zeros((oracle.H, oracle.W), dtype=bool)
    lit = np.zeros_like(eligible)
    eligible[73:] = use0 | use4
    shadow = (use0 & sh0) | (use4 & sh4)
    lit[73:] = (use0 | use4) & ~shadow
    return eligible, lit


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, default=Path('build/shadow-cast-halfplane'))
    ap.add_argument('--step', type=int, default=8)
    ap.add_argument('--yaw-step', type=int, default=16)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    samples = oracle.traversable_samples(args.step, True)
    rows = []
    total_mismatch = total_eligible = worst = 0
    worst_pose = None
    for cx, cy in samples:
        for yaw in range(0, 256, args.yaw_step):
            e, truth = oracle.exact_floor_frame(cx, cy, yaw)
            ep, pred = predicted_frame(cx, cy, yaw)
            if not np.array_equal(e, ep):
                raise RuntimeError(f"receiver eligibility mismatch at {(cx, cy, yaw)}")
            denom = int(e.sum())
            mismatch = int(((truth ^ pred) & e).sum())
            pct = 100.0 * mismatch / max(1, denom)
            total_mismatch += mismatch
            total_eligible += denom
            if pct > worst:
                worst = pct
                worst_pose = (cx, cy, yaw, mismatch, denom)
            rows.append((cx, cy, yaw, denom, mismatch, pct))

    with (args.out / 'pose_metrics.csv').open('w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['x', 'y', 'yaw', 'eligible_pixels', 'mismatch_pixels', 'mismatch_pct'])
        w.writerows(rows)

    summary = {
        'positions': len(samples),
        'headings_per_position': 256 // args.yaw_step,
        'poses': len(rows),
        'cast_boundaries': len(CASTS),
        'receiver_regions': 2,
        'projected_closure_edges_required': 0,
        'general_shadow_polygons_required': 0,
        'runtime_receiver_masks_required': 0,
        'total_eligible_pixels': total_eligible,
        'total_mismatch_pixels': total_mismatch,
        'mean_weighted_mismatch_pct': 100.0 * total_mismatch / max(1, total_eligible),
        'worst_pose_mismatch_pct': worst,
        'worst_pose': worst_pose,
        'casts': [
            {
                'polygon': i,
                'a': c[4],
                'b': c[5],
                'A': c[0], 'B': c[1], 'C': c[2],
                'keep_positive': c[3],
                'floor_z': float(base.POLYS[i][1]),
            }
            for i, c in enumerate(CASTS)
        ],
    }
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')

    print('=== CAST HALF-PLANE IDENTITY ===')
    print(f"positions={len(samples)} poses={len(rows)} cast_boundaries={len(CASTS)}")
    for i, c in enumerate(CASTS):
        print(f"P{i}: {c[4]}->{c[5]} line=({c[0]:g},{c[1]:g},{c[2]:g}) side={'+' if c[3] else '-'} floor_z={base.POLYS[i][1]:g}")
    print(f"mismatch={total_mismatch}/{total_eligible} = {summary['mean_weighted_mismatch_pct']:.9f}% worst={worst:.9f}%")
    if total_mismatch:
        print(f"worst_pose={worst_pose}")
        print('CAST_HALFPLANE_IDENTITY=FAIL')
        return 2
    print('PROJECTED_CLOSURE_EDGES_REQUIRED=0')
    print('GENERAL_SHADOW_POLYGONS_REQUIRED=0')
    print('CAST_HALFPLANE_IDENTITY=PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
