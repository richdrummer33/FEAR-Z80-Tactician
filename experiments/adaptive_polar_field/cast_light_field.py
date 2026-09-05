#!/usr/bin/env python3
"""Bake receiver-owned static-light cast runs onto the Adaptive Polar camera grid.

This is the missing bridge between the mature Polar vertex field and the old
runtime shadow-polygon experiment.  A static light and a vertical occluder
vertex define a vertical cast plane.  Its intersection with a horizontal floor
is one straight run; if that run reaches a wall, the same plane continues up
that wall as a vertical light/shadow split.

The Game Gear must not ray-cast or polygon-clip these facts.  This tool therefore
bakes, per 4x4-world-unit camera cell, the two screen-bearing endpoints required
for each cast run using the SAME local affine Q12 bearing model used by the
mature Polar vertex projection field.  Topology is stored once as four tiny
receiver-owned cast records; each camera cell stores a four-bit active mask and
its affine endpoint records.

Current cell payload (experimental, deliberately self-contained):
    uint8 cast_mask;
    for each active cast, in cast order:
        caster endpoint: uint8 depth + 4-byte affine leaves
        receiver-hit endpoint: uint8 depth + 4-byte affine leaves

A leaf is exactly the mature Polar shape:
    base_q12:uint16, span_dx:int8, span_dy:int8
and runtime reconstruction is:
    base + (dx*local_x >> log2(span)) + (dy*local_y >> log2(span))

The caster endpoint is temporarily duplicated in this companion field so this
first integration is self-contained and cannot depend on whether that authored
wall vertex happened to be relevant to the ordinary wall recipe in a cell.
Once the cast renderer is proven, duplicate caster records can be removed where
safe and the existing 14-vertex field reused directly.
"""
from __future__ import annotations

import argparse
import math
import pathlib
from dataclasses import dataclass
from typing import Sequence

import local_projection_field_poc as lp

ROOT = pathlib.Path(__file__).resolve().parents[2]
GRID_W, GRID_H = lp.GRID_W, lp.GRID_H
CELL_Q4 = lp.CELL_Q4

LIGHT_XY = (92.0, 50.0)

# Must remain in lock-step with the authored two-room scene/oracle.
SEG_VERTS = (
    (0, 1), (1, 2), (3, 4), (4, 5), (5, 0), (2, 6), (7, 3),
    (8, 6), (7, 9), (8, 10), (10, 11), (11, 12), (12, 13), (13, 9),
    (2, 3), (6, 7), (6, 7),
)

SURFACE_ROOM_A = 0
SURFACE_ROOM_B = 1
SIDE_RIGHT = 0
SIDE_LEFT = 1


@dataclass(frozen=True)
class CastDef:
    caster_vid: int
    receiver_sid: int
    receiver_surface: int
    floor_z: int
    shadow_side: int
    expected_hit_q2: tuple[int, int]


# These are the four genuinely new boundaries in the accepted physical oracle.
# Every other shadow-patch boundary is an existing authored world wall.
CASTS: tuple[CastDef, ...] = (
    CastDef(2, 0, SURFACE_ROOM_A, 0, SIDE_LEFT,  (251, 64)),
    CastDef(3, 3, SURFACE_ROOM_A, 0, SIDE_RIGHT, (265, 320)),
    CastDef(6, 10, SURFACE_ROOM_B, 4, SIDE_RIGHT, (580, 52)),
    CastDef(7, 13, SURFACE_ROOM_B, 4, SIDE_LEFT,  (562, 336)),
)


@dataclass(frozen=True)
class BakedCast:
    caster_vid: int
    receiver_sid: int
    receiver_surface: int
    floor_z: int
    shadow_side: int
    split_u_q8: int
    nx_q5: int
    ny_q5: int
    hit_q2: tuple[int, int]
    hit_world: tuple[float, float]


def segment_intersection(p: tuple[float, float], d: tuple[float, float],
                         a: tuple[float, float], b: tuple[float, float]) -> tuple[float, float, tuple[float, float]]:
    """Solve p+t*d = a+u*(b-a) without numpy; t>1 means beyond caster."""
    ex, ey = b[0] - a[0], b[1] - a[1]
    det = d[1] * ex - d[0] * ey
    if abs(det) < 1e-12:
        raise ValueError("cast ray parallel to receiver segment")
    rx, ry = a[0] - p[0], a[1] - p[1]
    t = (-rx * ey + ry * ex) / det
    u = (d[0] * ry - d[1] * rx) / det
    return t, u, (p[0] + t * d[0], p[1] + t * d[1])


def q5_normal(dx: float, dy: float) -> tuple[int, int]:
    # Perpendicular to cast direction. Same signed Q5 normal convention as walls.
    nx, ny = -dy, dx
    mag = math.hypot(nx, ny)
    if mag < 1e-12:
        raise ValueError("degenerate cast direction")
    qx = max(-32, min(32, int(round(nx * 32.0 / mag))))
    qy = max(-32, min(32, int(round(ny * 32.0 / mag))))
    return qx, qy


def derive_casts(d: lp.D) -> tuple[BakedCast, ...]:
    verts = list(zip(d.vx, d.vy))
    out: list[BakedCast] = []
    for i, c in enumerate(CASTS):
        cx, cy = map(float, verts[c.caster_vid])
        sv0, sv1 = SEG_VERTS[c.receiver_sid]
        a = tuple(map(float, verts[sv0])); b = tuple(map(float, verts[sv1]))
        direction = (cx - LIGHT_XY[0], cy - LIGHT_XY[1])
        t, u, hit = segment_intersection(LIGHT_XY, direction, a, b)
        if t <= 1.0 + 1e-9:
            raise SystemExit(f"cast {i}: receiver is not beyond caster (t={t})")
        if u < -1e-9 or u > 1.0 + 1e-9:
            raise SystemExit(f"cast {i}: receiver hit falls off wall segment (u={u})")
        hit_q2 = (int(round(hit[0] * 4.0)), int(round(hit[1] * 4.0)))
        if hit_q2 != c.expected_hit_q2:
            raise SystemExit(f"cast {i}: Q2 receiver hit changed: got={hit_q2} expected={c.expected_hit_q2}")
        # Q8 means denominator 256 here. All four accepted Q2 hits are preserved.
        uq = max(0, min(255, int(round(u * 256.0))))
        nx, ny = q5_normal(*direction)
        out.append(BakedCast(c.caster_vid, c.receiver_sid, c.receiver_surface,
                             c.floor_z, c.shadow_side, uq, nx, ny, hit_q2, hit))
    return tuple(out)


def extended_data(d: lp.D, casts: Sequence[BakedCast]) -> tuple[lp.D, list[tuple[int, int]]]:
    """Append four accepted Q2 wall-hit points to the mature vertex coordinate arrays."""
    vx = list(d.vx); vy = list(d.vy); endpoint_ids: list[tuple[int, int]] = []
    for c in casts:
        hit_vid = len(vx)
        # lp.fit() multiplies these coordinates by 16. Quarter-world coordinates
        # are intentionally kept as floats so the accepted Q2 hit is exact in Q4.
        vx.append(c.hit_q2[0] / 4.0)
        vy.append(c.hit_q2[1] / 4.0)
        endpoint_ids.append((c.caster_vid, hit_vid))
    return lp.D(d.keys, d.ro, d.rs, d.bo, d.bs, d.grid, vx, vy), endpoint_ids


def active_mask(d: lp.D, gx: int, gy: int) -> int:
    # Correctness-first first integration: any Polar cell with a recipe may need
    # any of the four casts at some yaw. This costs only one mask byte/cell.
    # A later bake can cull by receiver visibility/topology after the runtime
    # result is proven against the dense physical oracle.
    return 0x0F if d.grid[gy * GRID_W + gx] != 255 else 0x00


def serialize_endpoint(d: lp.D, vid: int, gx: int, gy: int, threshold: float, min_q4: int) -> bytes:
    dep, _ = lp.corner_quant_depth(d, vid, gx, gy, threshold, min_q4)
    if dep is None:
        return bytes((0xFF,))
    out = bytearray((dep,))
    n = 1 << dep; step = CELL_Q4 // n
    for yy in range(n):
        for xx in range(n):
            rec = lp.quant_leaf_record(d, vid, gx, gy,
                                       xx * step, (xx + 1) * step,
                                       yy * step, (yy + 1) * step)
            if rec is None:
                raise RuntimeError(f"cast endpoint coefficient overflow cell={gx},{gy} vid={vid} depth={dep}")
            base, sx, sy = rec
            out.extend((base & 255, (base >> 8) & 15, sx & 255, sy & 255))
    return bytes(out)


def serialize_cell(d: lp.D, endpoint_ids: Sequence[tuple[int, int]], gx: int, gy: int,
                   threshold: float, min_q4: int) -> bytes:
    mask = active_mask(d, gx, gy)
    out = bytearray((mask,))
    for i, (caster, hit) in enumerate(endpoint_ids):
        if mask & (1 << i):
            out.extend(serialize_endpoint(d, caster, gx, gy, threshold, min_q4))
            out.extend(serialize_endpoint(d, hit, gx, gy, threshold, min_q4))
    return bytes(out)


def c_u8(name: str, data: Sequence[int], cols: int = 16) -> str:
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), cols):
        lines.append("    " + ", ".join(str(int(v) & 255) for v in data[i:i + cols]) + ",")
    lines.append("};")
    return "\n".join(lines)


def c_i8(name: str, data: Sequence[int], cols: int = 16) -> str:
    lines = [f"static const int8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), cols):
        lines.append("    " + ", ".join(str(int(v)) for v in data[i:i + cols]) + ",")
    lines.append("};")
    return "\n".join(lines)


def c_u16(name: str, data: Sequence[int], cols: int = 12) -> str:
    lines = [f"static const uint16_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), cols):
        lines.append("    " + ", ".join(str(int(v)) for v in data[i:i + cols]) + ",")
    lines.append("};")
    return "\n".join(lines)


def shr0(v: int, n: int) -> int:
    return v >> n if v >= 0 else -((-v) >> n)


def round_div_signed(n: int, d: int) -> int:
    return (n + d // 2) // d if n >= 0 else -(((-n) + d // 2) // d)


def emit(outdir: pathlib.Path, threshold: float, min_q4: int, rows_per_bank: int) -> None:
    d0 = lp.load(); casts = derive_casts(d0); d, endpoint_ids = extended_data(d0, casts)
    outdir.mkdir(parents=True, exist_ok=True)
    bank_count = (GRID_H + rows_per_bank - 1) // rows_per_bank
    max_cell = 0; total_payload = 0; fallback_endpoints = 0

    for bank in range(bank_count):
        y0 = bank * rows_per_bank; y1 = min(GRID_H, y0 + rows_per_bank)
        offsets = [0]; payload = bytearray()
        for gy in range(y0, y1):
            for gx in range(GRID_W):
                block = serialize_cell(d, endpoint_ids, gx, gy, threshold, min_q4)
                fallback_endpoints += block.count(0xFF)  # diagnostic upper bound; payload bytes can also be 0xff
                max_cell = max(max_cell, len(block)); payload.extend(block); offsets.append(len(payload))
        total_payload += len(payload)
        src = f'''/* GENERATED by cast_light_field.py. */
#include <stdint.h>
#include <string.h>
#include <gbdk/platform.h>
#pragma bank 255
BANKREF(tilesector_polar_castproj_bank{bank})

{c_u16("k_cast_off", offsets)}

{c_u8("k_cast_data", payload)}

void tsp_polar_castproj_load_bank{bank}(uint16_t cell, uint8_t *dst) BANKED {{
    uint16_t a=k_cast_off[cell], b=k_cast_off[cell+1u];
    if(b>a) memcpy(dst,&k_cast_data[a],(uint16_t)(b-a));
}}
'''
        (outdir / f"tilesector_polar_castproj_bank{bank}.c").write_text(src)
        print(f"CAST_BANK bank={bank} rows={y0}-{y1-1} payload={len(payload)} offsets={len(offsets)*2}")

    # Same depth-plane trick as ordinary straight walls, but only four cast normals.
    text = "\n".join(p.read_text() for p in sorted((ROOT / "src/generated").glob("tilesector_polar_data_part*.inc")))
    sin = lp.arr(text, "k_tspf_sin_q7")
    nf_rows: list[list[int]] = []; sf_rows: list[list[int]] = []
    for c in casts:
        nf: list[int] = []; sf: list[int] = []
        for yaw in range(256):
            sn = sin[yaw]; cs = sin[(yaw + 64) & 255]
            f = shr0(c.nx_q5 * cs + c.ny_q5 * sn, 5)
            side = shr0(-c.nx_q5 * sn + c.ny_q5 * cs, 5)
            nf.append(max(-127, min(127, f)))
            sf.append(max(-127, min(127, round_div_signed(side * 4, 5))))
        nf_rows.append(nf); sf_rows.append(sf)

    def i8_rows(name: str, rows: Sequence[Sequence[int]]) -> str:
        out = [f"static const int8_t {name}[{len(rows)}][256] = {{"]
        for row in rows:
            out.append("    {")
            for i in range(0, 256, 16): out.append("        " + ", ".join(str(v) for v in row[i:i+16]) + ",")
            out.append("    },")
        out.append("};")
        return "\n".join(out)

    hdr = [
        "/* GENERATED receiver-owned Polar cast-light metadata. */",
        "#ifndef TILESECTOR_POLAR_CAST_LIGHT_META_H",
        "#define TILESECTOR_POLAR_CAST_LIGHT_META_H",
        "#include <stdint.h>",
        "#include <gbdk/platform.h>",
        f"#define TSPF_CAST_COUNT {len(casts)}u",
        f"#define TSPF_CAST_BANK_COUNT {bank_count}u",
        f"#define TSPF_CAST_ROWS_PER_BANK {rows_per_bank}u",
        f"#define TSPF_CAST_MAX_CELL_BYTES {max_cell}u",
        f"#define TSPF_CAST_SURFACE_ROOM_A {SURFACE_ROOM_A}u",
        f"#define TSPF_CAST_SURFACE_ROOM_B {SURFACE_ROOM_B}u",
        f"#define TSPF_CAST_SIDE_RIGHT {SIDE_RIGHT}u",
        f"#define TSPF_CAST_SIDE_LEFT {SIDE_LEFT}u",
        c_u8("k_tspf_cast_caster", [c.caster_vid for c in casts]),
        c_u8("k_tspf_cast_receiver_sid", [c.receiver_sid for c in casts]),
        c_u8("k_tspf_cast_receiver_surface", [c.receiver_surface for c in casts]),
        c_u8("k_tspf_cast_floor_z", [c.floor_z for c in casts]),
        c_u8("k_tspf_cast_shadow_side", [c.shadow_side for c in casts]),
        c_u8("k_tspf_cast_split_u_q8", [c.split_u_q8 for c in casts]),
        c_i8("k_tspf_cast_nx_q5", [c.nx_q5 for c in casts]),
        c_i8("k_tspf_cast_ny_q5", [c.ny_q5 for c in casts]),
    ]
    for bank in range(bank_count):
        hdr.append(f"void tsp_polar_castproj_load_bank{bank}(uint16_t cell, uint8_t *dst) BANKED;")
    hdr.append("void tsp_polar_castplane_load(uint8_t yaw, int8_t *nf, int8_t *sf) BANKED;")
    hdr.append("#endif")
    (outdir / "tilesector_polar_cast_light_meta.h").write_text("\n\n".join(hdr) + "\n")

    plane_src = f'''/* GENERATED by cast_light_field.py. */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar_cast_light_meta.h"
#pragma bank 255
BANKREF(tilesector_polar_castplane)

{i8_rows("k_cast_nf_q7", nf_rows)}

{i8_rows("k_cast_stepfac_q4", sf_rows)}

void tsp_polar_castplane_load(uint8_t yaw, int8_t *nf, int8_t *sf) BANKED {{
    uint8_t c;
    for(c=0u;c<TSPF_CAST_COUNT;++c){{nf[c]=k_cast_nf_q7[c][yaw];sf[c]=k_cast_stepfac_q4[c][yaw];}}
}}
'''
    (outdir / "tilesector_polar_castplane.c").write_text(plane_src)

    print("CAST_CHAIN_AUDIT=PASS")
    for i, c in enumerate(casts):
        print(f"CAST {i}: caster=v{c.caster_vid} receiver_sid={c.receiver_sid} surface={c.receiver_surface} "
              f"floor_z={c.floor_z} hit_q2={c.hit_q2[0]},{c.hit_q2[1]} split_u_q8={c.split_u_q8} "
              f"normal_q5={c.nx_q5},{c.ny_q5} shadow_side={'LEFT' if c.shadow_side else 'RIGHT'}")
    print(f"CAST_GRID_BAKE=PASS cells={GRID_W*GRID_H} banks={bank_count} total_payload={total_payload} max_cell={max_cell}")
    print(f"CAST_ENDPOINT_MODEL=POLAR_Q12_AFFINE threshold={threshold:g} min_leaf_world={min_q4/16:g}")
    print("NOTE: fallback byte count above is intentionally not reported because 0xff may occur inside coefficient payload; runtime parser validation is the authoritative fallback count.")


def audit() -> None:
    d = lp.load(); casts = derive_casts(d)
    print("CAST_CHAIN_AUDIT=PASS")
    for i, c in enumerate(casts):
        print(f"CAST {i}: v{c.caster_vid} -> wall {c.receiver_sid}; hit={c.hit_world[0]:.6f},{c.hit_world[1]:.6f}; "
              f"Q2={c.hit_q2}; uQ8={c.split_u_q8}; nQ5=({c.nx_q5},{c.ny_q5})")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit-dir", default="")
    ap.add_argument("--emit-threshold", type=float, default=4.0)
    ap.add_argument("--min-q4", type=int, default=8)
    ap.add_argument("--rows-per-bank", type=int, default=4)
    a = ap.parse_args()
    if a.emit_dir:
        emit(pathlib.Path(a.emit_dir), a.emit_threshold, a.min_q4, a.rows_per_bank)
    else:
        audit()


if __name__ == "__main__":
    main()
