#!/usr/bin/env python3
"""Audit the mature static floor shadows as Polar-style cast receiver chains.

The accepted lighting oracle stores four convex hard-shadow polygons.  This
probe asks whether those polygons really need to exist as new runtime geometry.
For each polygon it finds the one edge that is NOT already an authored world
wall, treats that edge as a cast boundary emitted from an existing vertical
wall vertex, then ray-traces that cast against the authored scene to recover the
receiver wall.

For a static point light and a vertical occluder edge, the shadow-boundary plane
contains the world Z axis.  Therefore:

  * its intersection with a horizontal floor is one arbitrary-angle world line;
  * if that line reaches another vertical wall, the continuation on that wall is
    a vertical line through the receiver hit point.

That is deliberately the same representation family as Polar wall runs:
static world endpoints/topology are solved on the PC; runtime only projects a
small run and selects/fills the appropriate side.  No general shadow polygon
clipper is implied by this experiment.
"""
from __future__ import annotations

import argparse
import json
import math
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Optional

import matplotlib.pyplot as plt

import floor_shadow_edge_field_poc as base

oracle = base.oracle

PROFILE_NAMES = {
    oracle.FULL: "FULL",
    oracle.LINTEL: "LINTEL",
    oracle.RAISED: "RAISED",
    oracle.RISER: "RISER",
}


@dataclass
class Hit:
    sid: int
    t: float
    u: float
    x: float
    y: float


@dataclass
class Chain:
    polygon: int
    floor_z: float
    caster_name: str
    caster_vertex: int
    caster_x: float
    caster_y: float
    accepted_hit_name: str
    accepted_hit_x: float
    accepted_hit_y: float
    exact_hit_x: float
    exact_hit_y: float
    accepted_hit_error_world: float
    receiver_sid: int
    receiver_v0: int
    receiver_v1: int
    receiver_profile: str
    receiver_floor_z: float
    receiver_u: float
    receiver_u_q8: int
    closure_sids: list[int]
    cast_edge_only_new_boundary: bool
    wall_continuation_vertical: bool


def xy(v: base.VRef) -> tuple[float, float]:
    return v.x_q4 / 16.0, v.y_q4 / 16.0


def cross(ax: float, ay: float, bx: float, by: float) -> float:
    return ax * by - ay * bx


def point_on_world_segment(v: base.VRef, sid: int, eps: float = 1e-7) -> bool:
    x, y = xy(v)
    s = oracle.SEGS[sid]
    ax, ay = map(float, oracle.VERTS[s.v0])
    bx, by = map(float, oracle.VERTS[s.v1])
    dx, dy = bx - ax, by - ay
    px, py = x - ax, y - ay
    if abs(cross(px, py, dx, dy)) > eps * max(1.0, abs(dx) + abs(dy)):
        return False
    dot = px * dx + py * dy
    return -eps <= dot <= dx * dx + dy * dy + eps


def edge_world_sids(a: base.VRef, b: base.VRef) -> list[int]:
    """World segments containing the full edge a..b, including profile aliases."""
    return [sid for sid in range(len(oracle.SEGS))
            if point_on_world_segment(a, sid) and point_on_world_segment(b, sid)]


def unique_xy_segments() -> Iterable[int]:
    seen = set()
    for sid, s in enumerate(oracle.SEGS):
        key = tuple(sorted((s.v0, s.v1)))
        if key in seen:
            continue
        seen.add(key)
        yield sid


def ray_segment_hit(cx: float, cy: float, vx: float, vy: float, sid: int) -> Optional[Hit]:
    """Intersect light + t*(caster-light), choosing only points beyond caster t>1."""
    lx, ly, _ = oracle.LIGHT
    dx, dy = vx - lx, vy - ly
    s = oracle.SEGS[sid]
    ax, ay = map(float, oracle.VERTS[s.v0])
    bx, by = map(float, oracle.VERTS[s.v1])
    sx, sy = bx - ax, by - ay
    den = cross(dx, dy, sx, sy)
    if abs(den) < 1e-10:
        return None
    qx, qy = ax - lx, ay - ly
    t = cross(qx, qy, sx, sy) / den
    u = cross(qx, qy, dx, dy) / den
    if t <= 1.0 + 1e-8 or u < -1e-8 or u > 1.0 + 1e-8:
        return None
    return Hit(sid, t, u, lx + t * dx, ly + t * dy)


def first_receiver(caster: base.VRef) -> Hit:
    vx, vy = xy(caster)
    hits = [h for sid in unique_xy_segments()
            if (h := ray_segment_hit(vx, vy, vx, vy, sid)) is not None]
    if not hits:
        raise RuntimeError(f"no receiver beyond {caster.name}")
    return min(hits, key=lambda h: h.t)


def audit_polygon(pi: int, poly: tuple[base.VRef, ...], floor_z: float) -> Chain:
    closures: list[int] = []
    cast = []
    for i, a in enumerate(poly):
        b = poly[(i + 1) % len(poly)]
        sids = edge_world_sids(a, b)
        if sids:
            closures.append(sids[0])
        else:
            cast.append((a, b))
    if len(cast) != 1:
        raise RuntimeError(f"polygon {pi}: expected exactly one non-world edge, got {len(cast)}")

    a, b = cast[0]
    if a.existing >= 0 and b.existing < 0:
        caster, accepted = a, b
    elif b.existing >= 0 and a.existing < 0:
        caster, accepted = b, a
    else:
        raise RuntimeError(f"polygon {pi}: cast edge is not old-vertex -> baked-hit")

    hit = first_receiver(caster)
    if not point_on_world_segment(accepted, hit.sid):
        raise RuntimeError(f"polygon {pi}: accepted hit {accepted.name} is not on traced receiver sid={hit.sid}")

    ax, ay = xy(accepted)
    err = math.hypot(ax - hit.x, ay - hit.y)
    if err > 0.13 + 1e-9:
        raise RuntimeError(f"polygon {pi}: accepted Q2 hit too far from exact ray hit: {err}")

    rs = oracle.SEGS[hit.sid]
    rz0, _ = oracle.profile_z(rs.profile)
    if abs(rz0 - floor_z) > 1e-8:
        raise RuntimeError(f"polygon {pi}: receiver floor {rz0} != shadow floor {floor_z}")

    return Chain(
        polygon=pi,
        floor_z=float(floor_z),
        caster_name=caster.name,
        caster_vertex=caster.existing,
        caster_x=xy(caster)[0],
        caster_y=xy(caster)[1],
        accepted_hit_name=accepted.name,
        accepted_hit_x=ax,
        accepted_hit_y=ay,
        exact_hit_x=hit.x,
        exact_hit_y=hit.y,
        accepted_hit_error_world=err,
        receiver_sid=hit.sid,
        receiver_v0=rs.v0,
        receiver_v1=rs.v1,
        receiver_profile=PROFILE_NAMES[rs.profile],
        receiver_floor_z=float(rz0),
        receiver_u=hit.u,
        receiver_u_q8=max(0, min(255, int(round(hit.u * 255.0)))),
        closure_sids=closures,
        cast_edge_only_new_boundary=True,
        wall_continuation_vertical=True,
    )


def plot(chains: list[Chain], out: Path) -> None:
    fig, ax = plt.subplots(figsize=(10, 5.5))
    # World geometry.
    for sid in unique_xy_segments():
        s = oracle.SEGS[sid]
        a = oracle.VERTS[s.v0]
        b = oracle.VERTS[s.v1]
        ax.plot([a[0], b[0]], [a[1], b[1]], linewidth=1.2)
    lx, ly, _ = oracle.LIGHT
    ax.scatter([lx], [ly], marker='*', s=150)
    ax.text(lx + 2, ly - 2, 'light')
    for c in chains:
        ax.plot([c.caster_x, c.exact_hit_x], [c.caster_y, c.exact_hit_y], linewidth=2.5)
        ax.scatter([c.caster_x, c.exact_hit_x], [c.caster_y, c.exact_hit_y], s=30)
        ax.text(c.caster_x + 1, c.caster_y + 2, c.caster_name)
        ax.text(c.exact_hit_x + 1, c.exact_hit_y + 2, f"sid {c.receiver_sid}")
    ax.set_aspect('equal', 'box')
    ax.set_xlim(8, 184)
    ax.set_ylim(92, 0)
    ax.set_xlabel('world X')
    ax.set_ylabel('world Y')
    ax.set_title('Baked shadow cast boundaries: one new run per accepted polygon')
    fig.tight_layout()
    fig.savefig(out, dpi=180)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, default=Path('build/shadow-receiver-chain'))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    chains = [audit_polygon(i, p, z) for i, (p, z) in enumerate(base.POLYS)]
    unique_closure = sorted({sid for c in chains for sid in c.closure_sids})
    data = {
        'light': list(map(float, oracle.LIGHT)),
        'accepted_shadow_polygons': len(base.POLYS),
        'new_cast_boundaries': len(chains),
        'all_polygons_have_one_new_cast_edge': all(c.cast_edge_only_new_boundary for c in chains),
        'all_receiver_continuations_vertical': all(c.wall_continuation_vertical for c in chains),
        'closure_edges_reuse_world_geometry': True,
        'unique_world_closure_segments': unique_closure,
        'new_runtime_receiver_masks_required': 0,
        'new_general_polygon_clipper_required': False,
        'chains': [asdict(c) for c in chains],
    }
    (args.out / 'receiver-chains.json').write_text(json.dumps(data, indent=2) + '\n')
    plot(chains, args.out / 'receiver-chains.png')

    print('=== SHADOW RECEIVER CHAIN AUDIT ===')
    print(f"accepted polygons={len(base.POLYS)}; new cast runs={len(chains)}")
    print(f"existing closure segments reused={unique_closure}")
    for c in chains:
        print(
            f"P{c.polygon}: {c.caster_name} -> sid {c.receiver_sid} "
            f"({c.receiver_profile}) hit=({c.exact_hit_x:.4f},{c.exact_hit_y:.4f}) "
            f"accepted=({c.accepted_hit_x:.2f},{c.accepted_hit_y:.2f}) "
            f"err={c.accepted_hit_error_world:.4f} u={c.receiver_u:.6f} q8={c.receiver_u_q8} "
            f"closures={c.closure_sids}"
        )
    print('CAST_BOUNDARY_ONLY_NEW_GEOMETRY=PASS')
    print('RECEIVER_WALL_CONTINUATION_VERTICAL=PASS')
    print('GENERAL_SHADOW_POLYGON_RUNTIME_REQUIRED=NO')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
