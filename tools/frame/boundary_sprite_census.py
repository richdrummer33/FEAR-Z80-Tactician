#!/usr/bin/env python3
"""Census a sprite-overlay alternative to whole-tile exact-X composites.

The whole-tile census showed that exact mixed-owner 8x8 background tiles create
far too many new patterns. This probe asks a narrower hardware question:

Can the ordinary background materializer keep ONE owner as the coarse substrate,
while an 8px-wide sprite, positioned at the TRUE X boundary, redraws the owner
on the other side?

Because sprites have pixel X positioning, boundary phase no longer consumes a
pattern dimension. For A|B|C inside one coarse tile, choose B as the substrate;
an A sprite ends at the left boundary and a C sprite starts at the right
boundary. This preserves a 1..7px middle face without a mixed background tile.

This is a host-side capacity/corpus census only. It counts:
  * exactable 1- and 2-transition boundary tiles versus deferred 3+ transitions
  * required 8x8 correction sprites per frame and per scanline
  * unique sprite patterns when the correction sprite is fully authoritative
    over its 8px strip (ceiling/wall/edge/floor are distinct semantic colours)
  * vocabulary coverage versus the GG 64-sprite / 8-sprites-per-line limits

It deliberately does NOT assume sprite H/V flip support.
"""
from __future__ import annotations

import argparse
import csv
import math
import pathlib
import statistics
from collections import Counter

from boundary_composite_census import (
    COLS,
    FOCAL,
    W,
    arr,
    cross,
    derive_segments,
    encode_pattern,
    frame_pixel_columns,
    full_top_bottom,
    pct,
    slots_for_coverage,
)

SCREEN_H = 144
ROWS = SCREEN_H // 8
SPRITE_LIMIT_TOTAL = 64
SPRITE_LIMIT_LINE = 8
SPRITE_PATTERN_PAGE = 192  # IDs 256..447 before name table at $3800.

CEIL = 0
WALL = 1
EDGE = 2
FLOOR = 3


def frame_context(frame):
    camx = int(frame["x_q4"]) / 16.0
    camy = int(frame["y_q4"]) / 16.0
    yaw = int(frame["yaw"]) * 2.0 * math.pi / 256.0
    fx, fy = math.cos(yaw), math.sin(yaw)
    rx, ry = -fy, fx
    return camx, camy, fx, fy, rx, ry


def line_depth(ctx, seg_by_sid, sid, screen_x):
    if sid is None:
        return None
    camx, camy, fx, fy, rx, ry = ctx
    ax, ay, bx, by = seg_by_sid[sid]
    u = (screen_x - 80.0) / FOCAL
    dx = fx + u * rx
    dy = fy + u * ry
    sx = bx - ax
    sy = by - ay
    den = cross(dx, dy, sx, sy)
    if abs(den) < 1e-10:
        return None
    qx = ax - camx
    qy = ay - camy
    t = cross(qx, qy, sx, sy) / den
    return t if t > 1e-7 else None


def surface_pixel(ctx, seg_by_sid, sid, gx, gy):
    depth = line_depth(ctx, seg_by_sid, sid, gx + 0.5)
    if depth is None:
        return CEIL if gy < 72 else FLOOR
    top, bot = full_top_bottom(depth)
    if gy == top or gy == bot:
        return EDGE
    if top < gy < bot:
        return WALL
    return CEIL if gy < 72 else FLOOR


def surface_row_pattern(ctx, seg_by_sid, sid, sprite_x, tile_row):
    out = []
    y0 = tile_row * 8
    for lx in range(8):
        gx = sprite_x + lx
        for ly in range(8):
            out.append(surface_pixel(ctx, seg_by_sid, sid, gx, y0 + ly))
    return tuple(out)


def owner_runs(owner8):
    out = []
    start = 0
    cur = owner8[0]
    for i in range(1, 8):
        if owner8[i] != cur:
            out.append((start, i, cur))
            start = i
            cur = owner8[i]
    out.append((start, 8, cur))
    return out


def correction_rows(ctx, seg_by_sid, base_sid, corr_sid, sprite_x,
                    sliver_x0, sliver_x1):
    """Return [(tile_row, authoritative_sprite_pattern), ...].

    Only rows where base and correction differ inside the actually-wrong sliver
    need a sprite. The sprite itself spans a full 8px strip wholly on corr_sid's
    side of the exact boundary, so it may safely redraw pixels outside the
    sliver too.
    """
    out = []
    for tr in range(ROWS):
        y0 = tr * 8
        differs = False
        for gx in range(sliver_x0, sliver_x1):
            for ly in range(8):
                gy = y0 + ly
                if surface_pixel(ctx, seg_by_sid, base_sid, gx, gy) !=                    surface_pixel(ctx, seg_by_sid, corr_sid, gx, gy):
                    differs = True
                    break
            if differs:
                break
        if differs:
            out.append((tr, surface_row_pattern(ctx, seg_by_sid, corr_sid,
                                                sprite_x, tr)))
    return out


def choose_single_transition(ctx, seg_by_sid, tc, runs):
    """Choose A or B substrate by correction sprite-row cost.

    Ties preserve the owner at the coarse column's centre pixel.
    """
    tile_x0 = tc * 8
    tile_x1 = tile_x0 + 8
    left, right = runs
    boundary = tile_x0 + right[0]

    # A substrate, redraw B from exact boundary rightward.
    a_rows = correction_rows(
        ctx, seg_by_sid, left[2], right[2], boundary,
        boundary, tile_x1,
    )
    # B substrate, redraw A in the 8px strip ending at the exact boundary.
    b_rows = correction_rows(
        ctx, seg_by_sid, right[2], left[2], boundary - 8,
        tile_x0, boundary,
    )

    if len(a_rows) < len(b_rows):
        return left[2], [(right[2], boundary, a_rows)]
    if len(b_rows) < len(a_rows):
        return right[2], [(left[2], boundary - 8, b_rows)]

    centre_owner = left[2] if 4 < right[0] else right[2]
    if centre_owner == left[2]:
        return left[2], [(right[2], boundary, a_rows)]
    return right[2], [(left[2], boundary - 8, b_rows)]


def two_transition_plan(ctx, seg_by_sid, tc, runs):
    """A|B|C: render B as substrate, patch A left and C right."""
    tile_x0 = tc * 8
    tile_x1 = tile_x0 + 8
    left, mid, right = runs
    b0 = tile_x0 + mid[0]
    b1 = tile_x0 + right[0]
    left_rows = correction_rows(
        ctx, seg_by_sid, mid[2], left[2], b0 - 8,
        tile_x0, b0,
    )
    right_rows = correction_rows(
        ctx, seg_by_sid, mid[2], right[2], b1,
        b1, tile_x1,
    )
    return mid[2], [
        (left[2], b0 - 8, left_rows),
        (right[2], b1, right_rows),
    ]


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
    seg_by_sid = {sid: (ax, ay, bx, by) for sid, ax, ay, bx, by in segs}

    frames_total = 0
    boundary_tiles = 0
    exactable_tiles = 0
    one_transition = 0
    two_transition = 0
    deferred_3plus = 0
    sprite_counts = []
    max_line_counts = []
    total_limit_violations = 0
    line_limit_violations = 0
    pattern_occ = Counter()
    trace_sprites = Counter()
    trace_deferred = Counter()

    for frame_path in args.frames:
        frames = list(csv.DictReader(open(frame_path, newline="")))
        trace = pathlib.Path(frame_path).stem
        for fr in frames:
            frames_total += 1
            ctx = frame_context(fr)
            pxcols = frame_pixel_columns(fr, segs)
            frame_sprites = 0
            row_load = [0] * ROWS

            for tc in range(COLS):
                owner8 = tuple(pxcols[tc * 8 + x][0] for x in range(8))
                runs = owner_runs(owner8)
                nt = len(runs) - 1
                if nt == 0:
                    continue
                boundary_tiles += 1

                if nt == 1:
                    one_transition += 1
                    exactable_tiles += 1
                    _, corrections = choose_single_transition(
                        ctx, seg_by_sid, tc, runs
                    )
                elif nt == 2:
                    two_transition += 1
                    exactable_tiles += 1
                    _, corrections = two_transition_plan(
                        ctx, seg_by_sid, tc, runs
                    )
                else:
                    deferred_3plus += 1
                    trace_deferred[trace] += 1
                    continue

                for _sid, _sx, rows in corrections:
                    for tr, pat in rows:
                        frame_sprites += 1
                        row_load[tr] += 1
                        pattern_occ[pat] += 1

            sprite_counts.append(frame_sprites)
            max_line = max(row_load) if row_load else 0
            max_line_counts.append(max_line)
            trace_sprites[trace] += frame_sprites
            if frame_sprites > SPRITE_LIMIT_TOTAL:
                total_limit_violations += 1
            if max_line > SPRITE_LIMIT_LINE:
                line_limit_violations += 1

    unique_patterns = len(pattern_occ)
    p50_slots = slots_for_coverage(pattern_occ, 0.50)
    p90_slots = slots_for_coverage(pattern_occ, 0.90)
    p99_slots = slots_for_coverage(pattern_occ, 0.99)
    p100_slots = slots_for_coverage(pattern_occ, 1.00)
    total_occ = sum(pattern_occ.values())
    page_cov = sum(n for _, n in pattern_occ.most_common(SPRITE_PATTERN_PAGE))
    page_pct = (100.0 * page_cov / total_occ) if total_occ else 100.0

    exact_pct = 100.0 * exactable_tiles / boundary_tiles if boundary_tiles else 100.0

    print(
        f"BOUNDARY_SPRITE_CENSUS label={args.label} frames={frames_total} "
        f"surfaces={len(segs)}"
    )
    print(
        f"boundary_tiles total={boundary_tiles} exactable_1or2={exactable_tiles} "
        f"exactable_pct={exact_pct:.3f} one={one_transition} two={two_transition} "
        f"deferred_3plus={deferred_3plus}"
    )
    print(
        "sprites_per_frame "
        f"mean={statistics.fmean(sprite_counts):.3f} "
        f"p50={pct(sprite_counts,.50):.0f} "
        f"p95={pct(sprite_counts,.95):.0f} "
        f"max={max(sprite_counts) if sprite_counts else 0} "
        f"frames_over_64={total_limit_violations}"
    )
    print(
        "sprites_per_scanline "
        f"mean_frame_max={statistics.fmean(max_line_counts):.3f} "
        f"p50_frame_max={pct(max_line_counts,.50):.0f} "
        f"p95_frame_max={pct(max_line_counts,.95):.0f} "
        f"max={max(max_line_counts) if max_line_counts else 0} "
        f"frames_over_8={line_limit_violations}"
    )
    print(
        f"sprite_patterns unique_no_flip={unique_patterns} "
        f"high_page_capacity={SPRITE_PATTERN_PAGE} "
        f"fits_page={'yes' if unique_patterns <= SPRITE_PATTERN_PAGE else 'no'}"
    )
    print(
        "sprite_pattern_slots_for_occurrence_coverage "
        f"p50={p50_slots} p90={p90_slots} p99={p99_slots} p100={p100_slots}"
    )
    print(
        f"sprite_page_occurrence_coverage slots={SPRITE_PATTERN_PAGE} "
        f"coverage={page_pct:.3f}%"
    )
    print(
        "trace_sprite_occurrences "
        + ",".join(f"{k}={v}" for k, v in sorted(trace_sprites.items()))
    )
    print(
        "trace_deferred_3plus "
        + ",".join(f"{k}={v}" for k, v in sorted(trace_deferred.items()))
    )
    if pattern_occ:
        print(
            "sprite_pattern_occurrences "
            + ",".join(
                f"{encode_pattern(p)}:{n}" for p, n in pattern_occ.most_common(64)
            )
        )


if __name__ == "__main__":
    main()
