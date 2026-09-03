#!/usr/bin/env python3
"""Prototype nested distance codec against the DHC1 oracle corpus.

The nearest distance is the master raster.  Farther independently rendered
oracle views are predicted by pinhole shrinkage plus one tiny phase vector per
shared 8x8 master-pattern class.  Optionally, a deterministic direct-search pass
shuffles pixels *inside the shared master patterns* while preserving each
pattern's shade histogram.

This is intentionally an offline measurement tool, not a runtime format.
Its job is to answer whether distance can be factored into a master image plus
small distance-dependent selectors before we commit ROM/runtime architecture.
"""

import argparse
import pathlib

from analyze_doomguy_dense_corpus import Corpus, SHADE_RGB, write_ppm
from nested_lod_core import (
    LossWeights, Raster, compare, decode_with_global_phase,
    decode_with_pattern_phases, distinct_tile_count, fit_global_phase,
    fit_pattern_phases, phase_grid, refine_by_pattern_swaps, tile_classes,
)


def sample_raster(corpus, sample):
    out = Raster.blank(corpus.screen_w, corpus.screen_h)
    for y in range(sample.y0, sample.y1 + 1):
        for x in range(sample.x0, sample.x1 + 1):
            out.set(x, y, sample.at(x, y))
    return out


def write_semantic_ppm(path, raster):
    write_ppm(path, raster.width, raster.height, raster.pixels)


def fmt_loss(loss):
    return (
        f"silhouette_pct={loss.silhouette_pct:.3f} "
        f"shade_abs_mean={loss.shade_mean:.3f} "
        f"changed_tiles={loss.changed_tiles} weighted={loss.weighted:.1f}"
    )


def choose_angle_sample(corpus, band, angle):
    if angle < 0 or angle >= corpus.angles:
        raise SystemExit(f"angle {angle} outside 0..{corpus.angles - 1}")
    return corpus.band(band)[angle]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--angle", type=int, default=0,
                    help="dense-corpus angle index to study")
    ap.add_argument("--phase-radius", type=int, default=2)
    ap.add_argument("--silhouette-weight", type=float, default=12.0)
    ap.add_argument("--shade-weight", type=float, default=1.0)
    ap.add_argument("--near-weight", type=float, default=4.0,
                    help="penalty for damaging the nearest oracle during refinement")
    ap.add_argument("--refine-passes", type=int, default=1)
    ap.add_argument("--refine-candidates", type=int, default=256)
    ap.add_argument("--dump-dir")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    if c.bands < 2:
        raise SystemExit("nested distance experiment needs at least two bands")

    weights = LossWeights(args.silhouette_weight, args.shade_weight)
    phases = phase_grid(args.phase_radius)
    master_sample = choose_angle_sample(c, 0, args.angle)
    master_oracle = sample_raster(c, master_sample)
    master = master_oracle.copy()
    source_anchor = (master_sample.anchor_x, master_sample.anchor_y)
    source_radius = c.radii[0]
    class_map, class_origins = tile_classes(master_oracle)

    print("NESTED_DISTANCE_CODEC v1")
    print(
        f"angle={args.angle} source_radius={source_radius:.3f} "
        f"bands={c.bands} phase_candidates={len(phases)} "
        f"master_patterns={distinct_tile_count(master_oracle)}"
    )

    levels = []
    for band in range(1, c.bands):
        s = choose_angle_sample(c, band, args.angle)
        target = sample_raster(c, s)
        target_anchor = (s.anchor_x, s.anchor_y)
        radius = c.radii[band]

        nearest = decode_with_global_phase(
            master, (target.width, target.height), source_anchor, target_anchor,
            source_radius, radius, (0, 0))
        nearest_loss = compare(nearest, target, weights)

        gp, gl, global_pred = fit_global_phase(
            master, target, source_anchor, target_anchor,
            source_radius, radius, phases, weights)

        table, pl, pattern_pred = fit_pattern_phases(
            master, target, source_anchor, target_anchor,
            source_radius, radius, class_map, phases, weights)

        nonempty_table = sum(1 for key in table if any(key))
        descriptor_bytes = nonempty_table * 2
        print(
            f"band={band} radius={radius:.3f} model=nearest "
            f"{fmt_loss(nearest_loss)} patterns={distinct_tile_count(nearest)}"
        )
        print(
            f"band={band} radius={radius:.3f} model=global_phase phase={gp} "
            f"{fmt_loss(gl)} patterns={distinct_tile_count(global_pred)} "
            f"descriptor_bytes=2"
        )
        print(
            f"band={band} radius={radius:.3f} model=pattern_phase "
            f"{fmt_loss(pl)} patterns={distinct_tile_count(pattern_pred)} "
            f"phase_classes={nonempty_table} descriptor_bytes={descriptor_bytes}"
        )
        levels.append({
            "band": band, "sample": s, "target": target, "radius": radius,
            "anchor": target_anchor, "table": table,
        })

        if args.dump_dir:
            out = pathlib.Path(args.dump_dir)
            out.mkdir(parents=True, exist_ok=True)
            write_semantic_ppm(out / f"a{args.angle:03d}-b{band}-oracle.ppm", target)
            write_semantic_ppm(out / f"a{args.angle:03d}-b{band}-nearest.ppm", nearest)
            write_semantic_ppm(out / f"a{args.angle:03d}-b{band}-pattern.ppm", pattern_pred)

    def objective(candidate):
        # Preserve the close view strongly, but allow tiny spatial rearrangements
        # when they buy substantially better medium/far representations.
        total = args.near_weight * compare(candidate, master_oracle, weights).weighted
        for level in levels:
            pred = decode_with_pattern_phases(
                candidate, level["target"], source_anchor, level["anchor"],
                source_radius, level["radius"], class_map, level["table"])
            total += compare(pred, level["target"], weights).weighted
        return total

    history = []
    if args.refine_passes > 0 and args.refine_candidates > 0:
        before = objective(master)
        history = refine_by_pattern_swaps(
            master, class_origins, objective, args.refine_passes,
            args.refine_candidates)
        # Re-fit only the tiny phase tables after the class-wide pixel shuffles.
        for level in levels:
            table, _, _ = fit_pattern_phases(
                master, level["target"], source_anchor, level["anchor"],
                source_radius, level["radius"], class_map, phases, weights)
            level["table"] = table
        after = objective(master)
        near_damage = compare(master, master_oracle, weights)
        accepted = sum(h["accepted"] for h in history)
        print(
            f"refine passes={len(history)} accepted_swaps={accepted} "
            f"objective_before={before:.1f} objective_after={after:.1f} "
            f"near_damage_{fmt_loss(near_damage)}"
        )

        for level in levels:
            pred = decode_with_pattern_phases(
                master, level["target"], source_anchor, level["anchor"],
                source_radius, level["radius"], class_map, level["table"])
            loss = compare(pred, level["target"], weights)
            nonempty_table = sum(1 for key in level["table"] if any(key))
            print(
                f"band={level['band']} radius={level['radius']:.3f} "
                f"model=pattern_phase_refined {fmt_loss(loss)} "
                f"patterns={distinct_tile_count(pred)} "
                f"phase_classes={nonempty_table} "
                f"descriptor_bytes={nonempty_table * 2}"
            )
            if args.dump_dir:
                out = pathlib.Path(args.dump_dir)
                write_semantic_ppm(
                    out / f"a{args.angle:03d}-b{level['band']}-refined.ppm", pred)

        if args.dump_dir:
            write_semantic_ppm(
                pathlib.Path(args.dump_dir) / f"a{args.angle:03d}-master-refined.ppm",
                master)

    print("NESTED_DISTANCE_CODEC_PASS")


if __name__ == "__main__":
    main()
