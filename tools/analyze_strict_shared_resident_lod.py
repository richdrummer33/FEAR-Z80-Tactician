#!/usr/bin/env python3
"""Strict shared-only resident LOD experiment.

Unlike analyze_shared_resident_lod.py, farther hero tiles may NOT fall back to
an angle-local near-distance vocabulary.  Every occupied far tile must choose
only from:

  * implicit empty; and
  * one globally shared learned 8x8 LOD vocabulary.

H/V flips remain free.  This is the hardware-clean zero-hero-pattern-upload
model for medium/far distance: once the shared dictionary is loaded, changing
angle/distance changes name-table references rather than pattern bytes.
"""

import argparse
import json
import os
import pathlib

from analyze_doomguy_dense_corpus import Corpus
from analyze_resident_lod_dictionary import occupied_tiles, parse_csv_ints, sample_raster
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--angles", type=parse_csv_ints,
                    default=parse_csv_ints("0,8,16,24"))
    ap.add_argument("--bands", type=parse_csv_ints,
                    default=parse_csv_ints("1,2,3"))
    ap.add_argument("--additions", type=parse_csv_ints,
                    default=parse_csv_ints("16,32,64,96"))
    ap.add_argument("--lloyd-iterations", type=int, default=6)
    ap.add_argument("--silhouette-weight", type=float, default=12.0)
    ap.add_argument("--shade-weight", type=float, default=1.0)
    ap.add_argument("--summary-json")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    weights = TileWeights(args.silhouette_weight, args.shade_weight)
    groups = []
    meta = []

    for angle in args.angles:
        if angle < 0 or angle >= c.angles:
            raise SystemExit(f"angle {angle} outside 0..{c.angles - 1}")
        demands = []
        by_band = {}
        for band in args.bands:
            if band <= 0 or band >= c.bands:
                raise SystemExit(f"band {band} outside 1..{c.bands - 1}")
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

        # Empty base => prepare_group installs only the implicit zero tile.
        group = prepare_group([], demands, allow_flips=True)
        groups.append(group)
        meta.append({
            "angle": angle,
            "far_demand_cells": len(demands),
            "far_demand_by_band": by_band,
        })

    baseline = score_groups(groups, [], weights, allow_flips=True)
    growth = greedy_shared_growth(
        groups, max(args.additions), weights, allow_flips=True)

    points = []
    for requested in args.additions:
        available = min(requested, len(growth["shared"]))
        seeded = growth["shared"][:available]
        greedy = score_groups(groups, seeded, weights, allow_flips=True)
        learned = refine_shared_dictionary(
            groups, seeded, iterations=args.lloyd_iterations,
            weights=weights, allow_flips=True)
        final = learned["final_score"]
        points.append({
            "requested_patterns": requested,
            "actual_patterns": available,
            "resident_pattern_bytes": available * 32,
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
        "schema": "strict-shared-resident-lod-v1",
        "angles": args.angles,
        "bands": args.bands,
        "groups": meta,
        "total_far_demand_cells": baseline["demand_count"],
        "empty_only_total_cost": baseline["total_cost"],
        "points": points,
        "runtime_model": {
            "hero_pattern_uploads_after_dictionary_load": 0,
            "per_visible_hero_cell_runtime": "name-table reference + optional H/V flip",
            "dictionary_is_global_across_selected_angles": True,
        },
    }
    atomic_json(args.summary_json, result)
    print("STRICT_SHARED_RESIDENT_LOD_RESULT " + json.dumps(result, sort_keys=True))
    print("STRICT_SHARED_RESIDENT_LOD_PASS")


if __name__ == "__main__":
    main()
