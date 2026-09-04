#!/usr/bin/env python3
"""Hierarchical transparent sprite LOD codebook.

Stage A learns a small far-only core shared across every angle.
Stage B freezes that core and learns additional mid/far refinement entries.

Runtime policy:
- far band sees only the core prefix;
- mid/far sees core + refinement prefix;
- patterns are never replaced merely because angle changes.

This tests an explicitly nested vocabulary rather than independently optimized
flat dictionaries at each distance.
"""

import argparse
import json
import os
import pathlib

from analyze_doomguy_dense_corpus import Corpus
from analyze_resident_lod_dictionary import parse_csv_ints
from analyze_sprite_resident_lod import build_groups
from resident_tile_dictionary import TileWeights
from shared_resident_lod import (
    greedy_shared_growth, prepare_group, refine_shared_dictionary, score_groups,
)


def atomic_json(path, payload):
    if not path:
        return
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n")
    os.replace(tmp, path)


def with_fixed_base(groups, fixed):
    return [
        prepare_group(fixed, group["demands"], allow_flips=False)
        for group in groups
    ]


def replay_stats(groups, score, views):
    refs_by_view = {v["view_index"]: 0 for v in views}
    drops_by_view = {v["view_index"]: 0 for v in views}
    for gscore in score["groups"]:
        group = groups[gscore["group_index"]]
        for demand, match in zip(group["demands"], gscore["score"]["matches"]):
            if match["dictionary_index"] == 0:
                drops_by_view[demand["view_index"]] += 1
            else:
                refs_by_view[demand["view_index"]] += 1
    refs = list(refs_by_view.values())
    return {
        "sprite_refs_mean": sum(refs) / len(refs) if refs else 0.0,
        "sprite_refs_max": max(refs, default=0),
        "sat_bytes_mean": (
            sum(3 * n + 1 for n in refs) / len(refs) if refs else 0.0),
        "sat_bytes_max": max((3 * n + 1 for n in refs), default=0),
        "naive_rom_map_bytes_total": sum(3 * n + 1 for n in refs),
        "dropped_to_transparent_total": sum(drops_by_view.values()),
    }


def learn(groups, count, weights, iterations):
    growth = greedy_shared_growth(groups, count, weights, allow_flips=False)
    seeded = growth["shared"][:count]
    return refine_shared_dictionary(
        groups, seeded, iterations=iterations,
        weights=weights, allow_flips=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--angles", type=parse_csv_ints)
    ap.add_argument("--far-band", type=int, default=3)
    ap.add_argument("--mid-band", type=int, default=2)
    ap.add_argument("--core-patterns", type=int, default=16)
    ap.add_argument("--refinement-patterns", type=parse_csv_ints,
                    default=parse_csv_ints("16,48"))
    ap.add_argument("--lloyd-iterations", type=int, default=4)
    ap.add_argument("--summary-json")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    angles = args.angles if args.angles is not None else list(range(c.angles))
    weights = TileWeights(12.0, 1.0)

    far_groups, far_views = build_groups(c, angles, [args.far_band])
    midfar_groups, midfar_views = build_groups(
        c, angles, [args.mid_band, args.far_band])

    core_result = learn(
        far_groups, args.core_patterns, weights, args.lloyd_iterations)
    core = core_result["shared"]
    far_score = score_groups(far_groups, core, weights, allow_flips=False)

    points = []
    for extra_count in args.refinement_patterns:
        staged_groups = with_fixed_base(midfar_groups, core)
        extra_result = learn(
            staged_groups, extra_count, weights, args.lloyd_iterations)
        extras = extra_result["shared"]
        staged_score = score_groups(
            staged_groups, extras, weights, allow_flips=False)

        total_patterns = len(core) + len(extras)
        # Flat comparison with exactly the same total number of patterns.
        flat_result = learn(
            midfar_groups, total_patterns, weights, args.lloyd_iterations)
        flat_score = flat_result["final_score"]

        points.append({
            "core_patterns": len(core),
            "refinement_patterns": len(extras),
            "total_patterns": total_patterns,
            "resident_bytes_if_full": total_patterns * 32,
            "far_resident_bytes": len(core) * 32,
            "far_core_mean_cost": far_score["mean_cost"],
            "midfar_nested_mean_cost": staged_score["mean_cost"],
            "midfar_flat_same_size_mean_cost": flat_score["mean_cost"],
            "hierarchy_penalty_pct": (
                round(100.0 * (staged_score["total_cost"] -
                               flat_score["total_cost"]) /
                      flat_score["total_cost"], 4)
                if flat_score["total_cost"] else 0.0),
            "equivalent_silhouette_pixels_far": (
                far_score["mean_cost"] / weights.silhouette),
            "equivalent_silhouette_pixels_midfar": (
                staged_score["mean_cost"] / weights.silhouette),
            "far_replay": replay_stats(far_groups, far_score, far_views),
            "midfar_replay": replay_stats(
                staged_groups, staged_score, midfar_views),
            "core_lloyd": core_result["history"],
            "refinement_lloyd": extra_result["history"],
        })

    result = {
        "schema": "hierarchical-sprite-lod-v1",
        "angles": angles,
        "far_band": args.far_band,
        "mid_band": args.mid_band,
        "core_patterns": len(core),
        "core_far_mean_cost": far_score["mean_cost"],
        "points": points,
        "runtime_policy": {
            "far": "core prefix only",
            "midfar": "core + refinement prefix",
            "angle_change_pattern_uploads": 0,
            "distance_approach_uploads": "only newly enabled refinement prefix",
        },
    }
    atomic_json(args.summary_json, result)
    print("HIERARCHICAL_SPRITE_LOD_RESULT " + json.dumps(result, sort_keys=True))
    print("HIERARCHICAL_SPRITE_LOD_PASS")


if __name__ == "__main__":
    main()
