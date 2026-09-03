#!/usr/bin/env python3
"""Learned resident-LOD dictionary sweep.

Uses the exact same near vocabulary / far oracle demand construction as
analyze_resident_lod_dictionary.py, but compares two dictionaries at the same
number of additional 32-byte pattern slots:

1. greedy literal oracle-tile additions;
2. those same seeds after monotonic Lloyd-style shared-pattern learning.

Near-distance entries and the implicit empty pattern remain fixed.
"""

import argparse
import json
import os
import pathlib

from analyze_doomguy_dense_corpus import Corpus
from analyze_resident_lod_dictionary import (
    occupied_tiles, parse_csv_ints, sample_raster,
)
from resident_tile_dictionary import (
    TileWeights, dedupe_patterns, greedy_dictionary_growth, score_demands,
)
from resident_tile_lloyd import refine_learned_dictionary


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
                    default=parse_csv_ints("4,8,16,32"))
    ap.add_argument("--lloyd-iterations", type=int, default=4)
    ap.add_argument("--silhouette-weight", type=float, default=12.0)
    ap.add_argument("--shade-weight", type=float, default=1.0)
    ap.add_argument("--summary-json")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    weights = TileWeights(args.silhouette_weight, args.shade_weight)
    max_add = max(args.additions)
    report = {
        "schema": "learned-resident-lod-dictionary-v1",
        "status": "running",
        "angles": args.angles,
        "additions": args.additions,
        "lloyd_iterations": args.lloyd_iterations,
        "results": [],
    }
    atomic_json(args.summary_json, report)

    for angle in args.angles:
        near = sample_raster(c, c.band(0)[angle])
        base = dedupe_patterns(
            [p for _, _, p in occupied_tiles(near)],
            modulo_flips=True)

        demands = []
        for band in range(1, c.bands):
            far = sample_raster(c, c.band(band)[angle])
            for tx, ty, pattern in occupied_tiles(far):
                demands.append({
                    "pattern": pattern,
                    "angle": angle,
                    "band": band,
                    "radius": c.radii[band],
                    "tx": tx,
                    "ty": ty,
                })

        growth = greedy_dictionary_growth(
            demands, base, max_add, weights, allow_flips=True)
        fixed_count = growth["base_count_with_implicit_empty"]
        baseline = score_demands(
            demands, growth["dictionary"][:fixed_count],
            weights, allow_flips=True)

        points = []
        for additions in args.additions:
            available = min(
                additions,
                max(0, len(growth["dictionary"]) - fixed_count))
            seeded = growth["dictionary"][:fixed_count + available]
            greedy_score = score_demands(
                demands, seeded, weights, allow_flips=True)
            learned = refine_learned_dictionary(
                demands, seeded, fixed_count,
                iterations=args.lloyd_iterations,
                weights=weights, allow_flips=True)
            learned_score = learned["final_score"]
            points.append({
                "requested_additions": additions,
                "actual_additions": available,
                "stored_extra_bytes": available * 32,
                "greedy_total_cost": greedy_score["total_cost"],
                "greedy_exact_fraction": greedy_score["exact_fraction"],
                "learned_total_cost": learned_score["total_cost"],
                "learned_exact_fraction": learned_score["exact_fraction"],
                "learned_vs_greedy_improvement_abs": (
                    greedy_score["total_cost"] -
                    learned_score["total_cost"]),
                "learned_vs_greedy_improvement_pct": (
                    round(
                        100.0 * (
                            greedy_score["total_cost"] -
                            learned_score["total_cost"]) /
                        greedy_score["total_cost"], 4)
                    if greedy_score["total_cost"] else 0.0),
                "baseline_improvement_pct": (
                    round(
                        100.0 * (
                            baseline["total_cost"] -
                            learned_score["total_cost"]) /
                        baseline["total_cost"], 4)
                    if baseline["total_cost"] else 0.0),
                "lloyd_history": learned["history"],
            })

        result = {
            "angle": angle,
            "near_unique_patterns_mod_flips": len(base),
            "far_demand_cells": len(demands),
            "baseline_total_cost": baseline["total_cost"],
            "baseline_exact_fraction": baseline["exact_fraction"],
            "points": points,
        }
        report["results"].append(result)
        atomic_json(args.summary_json, report)
        print(
            "LEARNED_RESIDENT_LOD_RESULT " +
            json.dumps(result, sort_keys=True),
            flush=True)

    report["status"] = "complete"
    atomic_json(args.summary_json, report)
    print(
        "LEARNED_RESIDENT_LOD_SUMMARY " +
        json.dumps({
            "angles": len(report["results"]),
            "points_per_angle": len(args.additions),
            "max_additions": max_add,
        }, sort_keys=True),
        flush=True)
    print("LEARNED_RESIDENT_LOD_PASS")


if __name__ == "__main__":
    main()
