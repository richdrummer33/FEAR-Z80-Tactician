#!/usr/bin/env python3
"""Measure one shared resident LOD vocabulary across multiple view angles.

Each angle keeps its own near-distance base patterns.  The added LOD patterns
are global: one ROM/VRAM vocabulary is shared across all selected angles.
This is the key test for whether the attractive per-angle 1 KB result scales
into a practical runtime architecture rather than becoming 1 KB per angle.
"""

import argparse
import json
import os
import pathlib

from analyze_doomguy_dense_corpus import Corpus
from analyze_resident_lod_dictionary import (
    occupied_tiles, parse_csv_ints, sample_raster,
)
from resident_tile_dictionary import TileWeights
from shared_resident_lod import (
    greedy_shared_growth, prepare_group, refine_shared_dictionary,
    score_groups,
)


def atomic_json(path, payload):
    if not path:
        return
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n")
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--angles", type=parse_csv_ints,
                    default=parse_csv_ints("0,64,128,192"))
    ap.add_argument("--additions", type=parse_csv_ints,
                    default=parse_csv_ints("8,16,32,64"))
    ap.add_argument("--lloyd-iterations", type=int, default=4)
    ap.add_argument("--silhouette-weight", type=float, default=12.0)
    ap.add_argument("--shade-weight", type=float, default=1.0)
    ap.add_argument("--summary-json")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    weights = TileWeights(args.silhouette_weight, args.shade_weight)
    groups = []
    group_meta = []

    for angle in args.angles:
        if angle < 0 or angle >= c.angles:
            raise SystemExit(f"angle {angle} outside 0..{c.angles - 1}")
        near = sample_raster(c, c.band(0)[angle])
        near_patterns = [p for _, _, p in occupied_tiles(near)]
        demands = []
        by_band = {}
        for band in range(1, c.bands):
            far = sample_raster(c, c.band(band)[angle])
            cells = occupied_tiles(far)
            by_band[str(band)] = len(cells)
            for tx, ty, pattern in cells:
                demands.append({
                    "pattern": pattern,
                    "angle": angle,
                    "band": band,
                    "radius": c.radii[band],
                    "tx": tx,
                    "ty": ty,
                })
        group = prepare_group(near_patterns, demands, allow_flips=True)
        groups.append(group)
        group_meta.append({
            "angle": angle,
            "near_base_patterns_with_empty": len(group["base"]),
            "near_pattern_bytes_excluding_empty": (len(group["base"]) - 1) * 32,
            "far_demand_cells": len(demands),
            "far_demand_by_band": by_band,
        })

    baseline = score_groups(groups, [], weights, allow_flips=True)
    max_add = max(args.additions)
    growth = greedy_shared_growth(
        groups, max_add, weights, allow_flips=True)

    points = []
    for requested in args.additions:
        available = min(requested, len(growth["shared"]))
        seeded = growth["shared"][:available]
        greedy = score_groups(groups, seeded, weights, allow_flips=True)
        learned = refine_shared_dictionary(
            groups, seeded, iterations=args.lloyd_iterations,
            weights=weights, allow_flips=True)
        final = learned["final_score"]

        separate_rom_bytes = available * 32 * len(groups)
        shared_rom_bytes = available * 32
        points.append({
            "requested_additions": requested,
            "actual_additions": available,
            "shared_pattern_bytes": shared_rom_bytes,
            "naive_separate_per_angle_pattern_bytes": separate_rom_bytes,
            "rom_bytes_saved_vs_separate_for_selected_angles": (
                separate_rom_bytes - shared_rom_bytes),
            "greedy_total_cost": greedy["total_cost"],
            "learned_total_cost": final["total_cost"],
            "baseline_improvement_pct": (
                round(100.0 * (baseline["total_cost"] - final["total_cost"]) /
                      baseline["total_cost"], 4)
                if baseline["total_cost"] else 0.0),
            "learned_vs_greedy_improvement_pct": (
                round(100.0 * (greedy["total_cost"] - final["total_cost"]) /
                      greedy["total_cost"], 4)
                if greedy["total_cost"] else 0.0),
            "exact_fraction": final["exact_fraction"],
            "mean_cost": final["mean_cost"],
            "lloyd_history": learned["history"],
        })

    result = {
        "schema": "shared-resident-lod-v1",
        "angles": args.angles,
        "angle_count": len(args.angles),
        "groups": group_meta,
        "total_far_demand_cells": baseline["demand_count"],
        "baseline_total_cost": baseline["total_cost"],
        "baseline_exact_fraction": baseline["exact_fraction"],
        "points": points,
    }
    atomic_json(args.summary_json, result)
    print("SHARED_RESIDENT_LOD_RESULT " + json.dumps(result, sort_keys=True))
    print("SHARED_RESIDENT_LOD_PASS")


if __name__ == "__main__":
    main()
