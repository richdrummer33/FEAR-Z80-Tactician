#!/usr/bin/env python3
"""Size the missing cell-local Polar RANGE bake, the sibling of the bearing bake.

`local_projection_field_poc.py` bakes, per 4-world-unit camera cell, each
relevant corner's BEARING as `base + dx*local_x + dy*local_y` with adaptive
quadtree refinement.  That answers "which screen column is this corner in".

It does not answer "how tall is the wall there".  That is still solved at
runtime by wall_d_q4 -> inv_for_dq4 -> inv_at_invd.  This experiment bakes the
other half in exactly the same shape, so a span interpreter can read a cell
block and emit vertical edges without any projection math.

WHAT IS BAKED
-------------
Per (cell, relevant corner): the 8-bit inverse radial distance

    inv(P) = 2550 / |V - P|        clamped to the runtime's [10,127] domain

`inv` and not `distance`, deliberately, for two reasons:

  1. It is what the renderer consumes.  Runtime half-height is `h = inv >> 1`
     (TSPF_HORIZON +/- h), so an error in `inv` is an error in pixels with a
     fixed factor of 1/2 -- no reciprocal at runtime, and the error metric is
     directly meaningful.

  2. It allocates storage precision the way perspective needs it.  A far wall
     moves very little on screen per unit of camera travel, and `inv` is
     small and slowly varying there; a near wall is the opposite.  Baking
     distance would spend equal bits on both and then destroy the near
     precision in the reciprocal.

`inv` is yaw-free: it depends only on camera position.  The secant/FOV term
that makes wall height vary across the screen is a function of `rel` (bearing
minus yaw), i.e. of the screen COLUMN, and therefore stays a tiny screen-space
LUT rather than baked per pose.  This is why the yaw axis never enters storage.

RECORD FORMAT
-------------
    leaf = base:uint8 + dx:int8 + dy:int8                    (3 bytes)
    reconstruction = base + (dx*lx >> log2 span) + (dy*ly >> log2 span)

One byte narrower than the bearing leaf (which needs a uint16 Q12 base), and
every runtime operation is 8-bit.

RESULT - THIS DECOMPOSITION IS REJECTED
---------------------------------------
Measured on the current two-room topology (466 cells, 4433 corner records):

    threshold   pack        leaves   unfit/saturated corners
    2.0 px      22.3 KiB     5050    115
    1.0 px     106.0 KiB    33622    328
    0.5 px     271.9 KiB    90241   1264

Compare the BEARING bake at a comparable ~0.98 px: 46.1 KiB, 9610 leaves, 7
fallbacks.  Per-corner range is ~2.3x more expensive at 1 px and explodes at
0.5 px, because 1/r has 1/r^3 curvature where bearing has 1/r^2 - the cost is
concentrated exactly at near corners, which is also where it matters most.

The reason is that this bakes the wrong quantity.  RADIAL distance to a corner
is nonlinear in camera position.  PERPENDICULAR distance to a wall plane is
not:

    d_perp(P) = n . (V - P) = C_sid - (nx*Px + ny*Py)

with n constant per segment, so it is EXACTLY affine in camera position -
verified here to 0.000000000 Q4 error against a single global model, at every
sampled cell and sub-cell position.  It therefore needs no cell bake, no
refinement and no per-cell storage at all: one constant per segment.

    17 segments x (int16 C + int8 nx + int8 ny) = 68 bytes, total
    14 of 17 have cardinal normals -> one subtraction, no multiply

which is what src/tilesector_polar_renderer.c:wall_d_q4() already computes,
including the cardinal shortcuts.  The radial/secant term that turns
perpendicular distance into on-screen wall height is a function of `rel`
(bearing minus yaw) i.e. of the screen COLUMN, and stays in the existing
k_tspf_sec_q7 screen-space LUT.

CONCLUSION: the range half of the structural bake is already done and already
costs ~nothing.  Keep this file as the negative result - it is cheap to
re-derive the wrong answer and expensive to re-learn it.
"""
from __future__ import annotations
import argparse, math, pathlib, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from local_projection_field_poc import (  # noqa: E402
    load, relevant, CELL_Q4, GRID_W, GRID_H, shr0,
)

NEAR_D = 10.0            # TSPF_NEAR_Z_Q4 >> 4 : inv saturates at 255
FAR_D = 127.0            # TSPF_FAR_Z_Q4  >> 4
INV_NUM = 2550.0         # inv(10) == 255
PX_PER_INV = 0.5         # h = inv >> 1


def inv_at(vx, vy, xq, yq):
    """Runtime-domain 8-bit inverse radial distance from camera Q4 to vertex."""
    dx = vx * 16.0 - xq
    dy = vy * 16.0 - yq
    d = math.hypot(dx, dy) / 16.0
    if d <= NEAR_D:
        return 255.0, True          # saturated: wall overfills the viewport
    if d >= FAR_D:
        d = FAR_D
    return INV_NUM / d, False


def fit(d, v, gx, gy, x0, x1, y0, y1):
    """Least-error affine fit at the leaf centre, analytic gradient of 1/r."""
    cx = (x0 + x1 - 1) * .5
    cy = (y0 + y1 - 1) * .5
    X = d.vx[v] * 16.0 - (gx * CELL_Q4 + cx)
    Y = d.vy[v] * 16.0 - (gy * CELL_Q4 + cy)
    r2 = X * X + Y * Y
    if r2 < 0.25:
        return None
    base, sat = inv_at(d.vx[v], d.vy[v], gx * CELL_Q4 + cx, gy * CELL_Q4 + cy)
    if sat:
        return None
    # d(inv)/dP = d(2550*16/|V-P|)/dP = 2550*16 * (P-V)/|V-P|^3, in Q4 units.
    r = math.sqrt(r2)
    k = INV_NUM * 16.0 / (r2 * r)
    return base, k * X, k * Y


def quant_leaf(d, v, gx, gy, x0, x1, y0, y1):
    """Quantize to base:uint8 + dx:int8 + dy:int8 over the leaf span."""
    f = fit(d, v, gx, gy, x0, x1, y0, y1)
    if f is None:
        return None
    base, gx_, gy_ = f
    span = x1 - x0
    shift = int(round(math.log2(span)))
    b = int(round(base))
    if b < 0 or b > 255:
        return None
    # dx is the total change across the span; runtime shifts it back down.
    qx = int(round(gx_ * span))
    qy = int(round(gy_ * span))
    if not (-128 <= qx <= 127 and -128 <= qy <= 127):
        return None
    return b, qx, qy, shift


def leaf_error(d, v, gx, gy, x0, x1, y0, y1, exhaustive):
    """Worst |Δ half-height| in screen pixels over the leaf."""
    q = quant_leaf(d, v, gx, gy, x0, x1, y0, y1)
    if q is None:
        return float("inf"), True
    b, qx, qy, shift = q
    cx = (x0 + x1 - 1) * .5
    cy = (y0 + y1 - 1) * .5
    if exhaustive:
        xs = range(x0, x1); ys = range(y0, y1)
    else:
        z = x1 - 1; w = y1 - 1
        xs = sorted({x0, z, (x0 + z) // 2}); ys = sorted({y0, w, (y0 + w) // 2})
    worst = 0.0
    for x in xs:
        for y in ys:
            e, sat = inv_at(d.vx[v], d.vy[v], gx * CELL_Q4 + x, gy * CELL_Q4 + y)
            if sat:
                return float("inf"), True
            p = b + shr0(qx * int(round(x - cx)), shift) + shr0(qy * int(round(y - cy)), shift)
            worst = max(worst, abs(e - p) * PX_PER_INV)
    return worst, False


def corner_depth(d, v, gx, gy, thr_px, minq):
    """Smallest regular 1x1/2x2/4x4/8x8 grid meeting the pixel threshold."""
    maxdepth = 0
    size = CELL_Q4
    while size > minq:
        maxdepth += 1
        size //= 2
    for dep in range(maxdepth + 1):
        n = 1 << dep
        step = CELL_Q4 // n
        ok = True
        worst = 0.0
        for yy in range(n):
            for xx in range(n):
                e, bad = leaf_error(d, v, gx, gy, xx * step, (xx + 1) * step,
                                    yy * step, (yy + 1) * step, exhaustive=True)
                worst = max(worst, e)
                if bad or e > thr_px + 1e-9:
                    ok = False
                    break
            if not ok:
                break
        if ok:
            return dep, worst
    return None, float("inf")


def analyse(d, thr_px, minq):
    cells = []
    for gy in range(GRID_H):
        for gx in range(GRID_W):
            c, _ = relevant(d, gx, gy)
            if c:
                cells.append((gx, gy, c))
    hist = {}
    fallback = 0
    leaves = 0
    corner_records = 0
    worst = 0.0
    for gx, gy, corners in cells:
        for v in corners:
            corner_records += 1
            dep, e = corner_depth(d, v, gx, gy, thr_px, minq)
            if dep is None:
                fallback += 1
                continue
            hist[dep] = hist.get(dep, 0) + 1
            leaves += 4 ** dep
            worst = max(worst, e)
    # Same concrete shape as the bearing pack: full cell-offset table +
    # per-cell corner presence mask + one mode byte per corner + 3-byte leaves.
    pack = (GRID_W * GRID_H * 2) + (len(cells) * 2) + corner_records + leaves * 3
    return cells, corner_records, fallback, leaves, hist, worst, pack


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--threshold-px", type=float, default=1.0,
                    help="worst-case half-height error budget, screen pixels")
    ap.add_argument("--min-q4", type=int, default=8)
    args = ap.parse_args()

    d = load()
    print("=== POLAR CELL-LOCAL RANGE FIELD (sibling of the bearing bake) ===")
    print("baked quantity = 8-bit inverse radial distance; h = inv >> 1")
    print("leaf = base:uint8 + dx:int8 + dy:int8 (3 bytes); all-8-bit reconstruction")
    print()
    for thr in (args.threshold_px, 0.5, 2.0):
        cells, recs, fb, leaves, hist, worst, pack = analyse(d, thr, args.min_q4)
        print(f"threshold={thr:g}px  cells={len(cells)} corner-records={recs} "
              f"fallback(near-saturated)={fb}")
        print(f"  depth histogram={dict(sorted(hist.items()))}  affine leaves={leaves}")
        print(f"  worst accepted error={worst:.3f}px   CONCRETE PACK={pack} bytes "
              f"({pack/1024:.1f} KiB)")
        nonzero = [c for c in cells]
        print(f"  avg leaves/corner={leaves/max(1,recs-fb):.2f}")
        print()


if __name__ == "__main__":
    main()
