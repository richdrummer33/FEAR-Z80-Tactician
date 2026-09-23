#!/usr/bin/env python3
"""Census exact-X boundary tiles for the FULL-wall front-envelope renderer.

This is deliberately an independent host-side oracle.  It ray/segment
intersects the authored wall graph at each of the 160 screen-pixel centres,
projects FULL-wall height from forward depth, and rasterizes the top half of
the Game Gear image at 1px X precision.

The question is narrow: if a hardware 8px tile contains a true visibility
handoff between adjacent front-envelope owners, how many *new physical 8x8
patterns* are required beyond the ordinary one-owner edge/full vocabulary?

FULL walls are vertically symmetric at the benchmark eye height, so only rows
0..8 are counted physically; the floor half can reuse the same pattern via
VFLIP + the existing floor palette.  Patterns are also canonicalized under
HFLIP because the VDP can reflect them for free.
"""
from __future__ import annotations

import argparse
import csv
import math
import pathlib
import re
import statistics
from collections import Counter, defaultdict

COLS = 20
ROWS_TOP = 9
W = 160
FOCAL = 80.0
RECLAIMABLE_PATTERN_SLOTS = 36


def arr(text: str, name: str):
    m = re.search(
        r"static\s+const\s+[^;=]+?\b"
        + re.escape(name)
        + r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};",
        text,
        re.S,
    )
    if not m:
        raise SystemExit("missing generated array " + name)
    return [int(x, 0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+", m.group(1))]


def pct(v, p):
    if not v:
        return float("nan")
    q = sorted(v)
    return q[int((len(q) - 1) * p)]


def cross(ax, ay, bx, by):
    return ax * by - ay * bx


def derive_segments(keys, vx, vy):
    bysid = defaultdict(set)
    for w in keys:
        sid = w & 31
        v0 = (w >> 5) & 31
        v1 = (w >> 10) & 31
        if v0 >= len(vx) or v1 >= len(vx) or v0 == v1:
            continue
        bysid[sid].add(tuple(sorted((v0, v1))))
    conflicts = {sid: p for sid, p in bysid.items() if len(p) != 1}
    if conflicts:
        raise SystemExit("surface endpoint ambiguity: " + repr(conflicts))
    seg = []
    for sid in sorted(bysid):
        v0, v1 = next(iter(bysid[sid]))
        seg.append(
            (
                sid,
                float(vx[v0]),
                float(vy[v0]),
                float(vx[v1]),
                float(vy[v1]),
            )
        )
    return seg


def ray_hit(camx, camy, fx, fy, rx, ry, screen_x, segs):
    # Direction is deliberately not normalized. dot(dir,forward)==1, therefore
    # the ray parameter t is the camera-forward depth consumed by projection.
    u = (screen_x - 80.0) / FOCAL
    dx = fx + u * rx
    dy = fy + u * ry
    best_t = 1e30
    best_sid = None
    for sid, ax, ay, bx, by in segs:
        sx = bx - ax
        sy = by - ay
        den = cross(dx, dy, sx, sy)
        if abs(den) < 1e-10:
            continue
        qx = ax - camx
        qy = ay - camy
        t = cross(qx, qy, sx, sy) / den
        uu = cross(qx, qy, dx, dy) / den
        if t > 1e-7 and -1e-9 <= uu <= 1.0 + 1e-9 and t < best_t:
            best_t = t
            best_sid = sid
    if best_sid is None:
        return None, None
    return best_sid, best_t


def full_top_bottom(depth):
    # Mirror the runtime's integer FULL convention:
    # inverse depth ~= round(2560/depth), half = inv>>1,
    # top=71-half, bottom=72+half.
    inv = max(0, min(255, int(2560.0 / depth + 0.5)))
    half = inv >> 1
    return 71 - half, 72 + half


def frame_pixel_columns(frame, segs):
    camx = int(frame["x_q4"]) / 16.0
    camy = int(frame["y_q4"]) / 16.0
    yaw = int(frame["yaw"]) * 2.0 * math.pi / 256.0
    fx, fy = math.cos(yaw), math.sin(yaw)
    rx, ry = -fy, fx

    cols = []
    for x in range(W):
        sid, depth = ray_hit(camx, camy, fx, fy, rx, ry, x + 0.5, segs)
        if sid is None:
            cols.append((None, None, None))
        else:
            top, bot = full_top_bottom(depth)
            cols.append((sid, top, bot))
    return cols


def cell_pattern(pxcols, tile_col, tile_row):
    # 0=ceiling/background, 1=wall fill, 2=black wall edge.
    # Only top-half rows 0..8 are materialized here.  Bottom uses VFLIP+palette.
    out = []
    x0 = tile_col * 8
    y0 = tile_row * 8
    owners = []
    for lx in range(8):
        sid, top, bot = pxcols[x0 + lx]
        owners.append(sid)
        for ly in range(8):
            y = y0 + ly
            if sid is None:
                c = 0
            elif y == top:
                c = 2
            elif top < y < bot:
                c = 1
            else:
                c = 0
            out.append(c)
    return tuple(out), tuple(owners)


def hflip(pattern):
    # Pattern is stored x-major (8 groups of 8 y pixels).
    out = []
    for lx in range(7, -1, -1):
        base = lx * 8
        out.extend(pattern[base : base + 8])
    return tuple(out)


def canon_h(pattern):
    rev = hflip(pattern)
    return min(pattern, rev)


def transitions(owners):
    return sum(owners[i] != owners[i - 1] for i in range(1, 8))


def encode_pattern(pattern):
    # Human-readable compact 2bpp semantic signature, four pixels per byte.
    b = []
    for i in range(0, len(pattern), 4):
        q = 0
        for j in range(4):
            q |= (pattern[i + j] & 3) << (j * 2)
        b.append(q)
    return "".join(f"{x:02x}" for x in b)


def slots_for_coverage(counter, fraction):
    """Minimum most-common novel patterns needed to cover fraction of uses."""
    if not counter:
        return 0
    total = sum(counter.values())
    target = total * fraction
    accum = 0
    for slots, (_, count) in enumerate(counter.most_common(), 1):
        accum += count
        if accum >= target:
            return slots
    return len(counter)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map-inc", required=True)
    ap.add_argument("--frames", nargs="+", required=True)
    ap.add_argument("--label", default="combined")
    args = ap.parse_args()

    text = pathlib.Path(args.map_inc).read_text()
    vx = arr(text, "k_tspf_vx")
    vy = arr(text, "k_tspf_vy")
    keys = arr(text, "k_tspf_keys")
    segs = derive_segments(keys, vx, vy)

    normal_patterns = set()
    boundary_patterns = set()
    boundary_occ = Counter()
    boundary_cells_per_frame = []
    boundary_rows_per_frame = []
    transitions_per_cell = Counter()
    class_counts = Counter()
    trace_counts = Counter()
    frames_total = 0

    for frame_path in args.frames:
        frames = list(csv.DictReader(open(frame_path, newline="")))
        trace = pathlib.Path(frame_path).stem
        for fr in frames:
            frames_total += 1
            pxcols = frame_pixel_columns(fr, segs)
            frame_cells = 0
            frame_rows = 0

            for tc in range(COLS):
                owner8 = tuple(pxcols[tc * 8 + x][0] for x in range(8))
                nt = transitions(owner8)
                mixed = nt != 0
                if mixed:
                    frame_cells += 1
                    transitions_per_cell[nt] += 1
                    distinct = set(owner8)
                    if None in distinct:
                        class_counts["wall_void"] += 1
                    elif len(distinct) == 2:
                        class_counts["wall_wall"] += 1
                    else:
                        class_counts["multi_owner"] += 1

                for tr in range(ROWS_TOP):
                    pat, owners = cell_pattern(pxcols, tc, tr)
                    cp = canon_h(pat)
                    if mixed:
                        # Only count a boundary-row occurrence when its final
                        # pixels actually depend on the X handoff. If the two
                        # owners happen to rasterize to the same 8x8 result here,
                        # the ordinary tile is already sufficient.
                        left = owners[0]
                        same_visual = True
                        # Cheap visual test: if every x-column in the semantic
                        # pattern is identical, no split-specific pattern is owed.
                        c0 = pat[0:8]
                        for lx in range(1, 8):
                            if pat[lx * 8 : lx * 8 + 8] != c0:
                                same_visual = False
                                break
                        if not same_visual:
                            boundary_patterns.add(cp)
                            boundary_occ[cp] += 1
                            frame_rows += 1
                    else:
                        normal_patterns.add(cp)

            boundary_cells_per_frame.append(frame_cells)
            boundary_rows_per_frame.append(frame_rows)
            trace_counts[trace] += frame_cells

    novel = boundary_patterns - normal_patterns
    novel_occ = Counter({p: boundary_occ[p] for p in novel})

    print(
        f"BOUNDARY_COMPOSITE_CENSUS label={args.label} frames={frames_total} "
        f"surfaces={len(segs)}"
    )
    print(
        f"patterns normal={len(normal_patterns)} boundary={len(boundary_patterns)} "
        f"novel_physical_hflip={len(novel)} slots_budget={RECLAIMABLE_PATTERN_SLOTS} "
        f"fits={'yes' if len(novel) <= RECLAIMABLE_PATTERN_SLOTS else 'no'}"
    )
    if boundary_cells_per_frame:
        print(
            "boundary_columns_per_frame "
            f"mean={statistics.fmean(boundary_cells_per_frame):.3f} "
            f"p50={pct(boundary_cells_per_frame,.50):.0f} "
            f"p95={pct(boundary_cells_per_frame,.95):.0f} "
            f"max={max(boundary_cells_per_frame)}"
        )
        print(
            "special_tophalf_cells_per_frame "
            f"mean={statistics.fmean(boundary_rows_per_frame):.3f} "
            f"p50={pct(boundary_rows_per_frame,.50):.0f} "
            f"p95={pct(boundary_rows_per_frame,.95):.0f} "
            f"max={max(boundary_rows_per_frame)}"
        )

    print(
        "boundary_classes "
        + ",".join(f"{k}={v}" for k, v in sorted(class_counts.items()))
    )
    print(
        "transitions_per_boundary_tile "
        + ",".join(f"{k}={v}" for k, v in sorted(transitions_per_cell.items()))
    )
    print(
        "trace_boundary_columns "
        + ",".join(f"{k}={v}" for k, v in sorted(trace_counts.items()))
    )

    if novel_occ:
        total_novel_occ = sum(novel_occ.values())
        slots50 = slots_for_coverage(novel_occ, 0.50)
        slots90 = slots_for_coverage(novel_occ, 0.90)
        slots99 = slots_for_coverage(novel_occ, 0.99)
        slots100 = slots_for_coverage(novel_occ, 1.00)
        covered_budget = sum(
            n for _, n in novel_occ.most_common(RECLAIMABLE_PATTERN_SLOTS)
        )
        coverage_budget = 100.0 * covered_budget / total_novel_occ
        print(
            "novel_slots_for_occurrence_coverage "
            f"p50={slots50} p90={slots90} p99={slots99} p100={slots100}"
        )
        print(
            "novel_slot_budget "
            f"slots={RECLAIMABLE_PATTERN_SLOTS} "
            f"headroom={RECLAIMABLE_PATTERN_SLOTS-len(novel)} "
            f"occurrence_coverage={coverage_budget:.3f}%"
        )
        print(
            "novel_pattern_occurrences "
            + ",".join(
                f"{encode_pattern(p)}:{n}" for p, n in novel_occ.most_common(64)
            )
        )
    else:
        print(
            "novel_slots_for_occurrence_coverage p50=0 p90=0 p99=0 p100=0"
        )
        print(
            f"novel_slot_budget slots={RECLAIMABLE_PATTERN_SLOTS} "
            f"headroom={RECLAIMABLE_PATTERN_SLOTS} occurrence_coverage=100.000%"
        )


if __name__ == "__main__":
    main()
