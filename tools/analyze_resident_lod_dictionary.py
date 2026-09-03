#!/usr/bin/env python3
"""Measure far-LOD demand against an actual resident 8x8 pattern vocabulary.

For each requested angle:
- band zero supplies the near resident pattern vocabulary;
- all farther bands supply independently rendered 8x8 oracle demands;
- H/V flips are free;
- the baseline asks how well near patterns alone serve those demands;
- greedy growth then adds the currently worst-represented oracle tile.

This is intentionally hardware-native: every decoded far cell is still exactly
one ordinary Game Gear 8x8 pattern reference.  Arbitrary host-side resampling is
only used later to synthesize/train the dictionary, never required at runtime.
"""

import argparse
import json
import os
import pathlib

from analyze_doomguy_dense_corpus import Corpus
from nested_lod_core import Raster, tile_signature
from resident_tile_dictionary import (
    TileWeights, dedupe_patterns, greedy_dictionary_growth,
)


def parse_csv_ints(text):
    vals = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        v = int(part)
        if v not in vals:
            vals.append(v)
    if not vals:
        raise argparse.ArgumentTypeError("empty integer list")
    return vals


def sample_raster(corpus, sample):
    r = Raster.blank(corpus.screen_w, corpus.screen_h)
    for y in range(sample.y0, sample.y1 + 1):
        for x in range(sample.x0, sample.x1 + 1):
            r.set(x, y, sample.at(x, y))
    return r


def occupied_tiles(raster):
    cols = (raster.width + 7) // 8
    rows = (raster.height + 7) // 8
    out = []
    for ty in range(rows):
        for tx in range(cols):
            p = tile_signature(raster, tx, ty)
            if any(p):
                out.append((tx, ty, p))
    return out


def atomic_json(path, payload):
    if not path:
        return
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n")
    os.replace(tmp, path)


def history_point(history, additions):
    eligible = [h for h in history if h["additions"] <= additions]
    if not eligible:
        return history[0]
    return eligible[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--angles", type=parse_csv_ints,
                    default=parse_csv_ints("0,64,128,192"))
    ap.add_argument("--additions", type=int, default=32)
    ap.add_argument("--checkpoints", type=parse_csv_ints,
                    default=parse_csv_ints("0,4,8,16,32"))
    ap.add_argument("--silhouette-weight", type=float, default=12.0)
    ap.add_argument("--shade-weight", type=float, default=1.0)
    ap.add_argument("--summary-json")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    weights = TileWeights(args.silhouette_weight, args.shade_weight)
    report = {
        "schema": "resident-lod-dictionary-v1",
        "status": "running",
        "angles": args.angles,
        "additions_limit": args.additions,
        "allow_hv_flips": True,
        "results": [],
    }
    atomic_json(args.summary_json, report)

    for angle in args.angles:
        if angle < 0 or angle >= c.angles:
            raise SystemExit(
                f"angle {angle} outside 0..{c.angles - 1}")

        near = sample_raster(c, c.band(0)[angle])
        base_raw = [p for _, _, p in occupied_tiles(near)]
        base = dedupe_patterns(base_raw, modulo_flips=True)

        demands = []
        demand_by_band = {}
        for band in range(1, c.bands):
            far = sample_raster(c, c.band(band)[angle])
            cells = occupied_tiles(far)
            demand_by_band[band] = len(cells)
            for tx, ty, pattern in cells:
                demands.append({
                    "pattern": pattern,
                    "angle": angle,
                    "band": band,
                    "radius": c.radii[band],
                    "tx": tx,
                    "ty": ty,
                })

        growth = greedy_dictionary_growth(
            demands, base, args.additions, weights, allow_flips=True)
        history = growth["history"]
        checkpoints = []
        for requested in args.checkpoints:
            p = history_point(history, min(requested, args.additions))
            checkpoints.append({
                "requested_additions": requested,
                "actual_additions": p["additions"],
                # Empty is implicit and does not need to be charged here.
                "stored_pattern_count": p["dictionary_count"] - 1,
                "stored_pattern_bytes": (p["dictionary_count"] - 1) * 32,
                "total_cost": p["total_cost"],
                "mean_cost_per_occupied_far_tile": p["mean_cost"],
                "exact_fraction": p["exact_fraction"],
            })

        baseline = history[0]
        final = history[-1]
        result = {
            "angle": angle,
            "near_occupied_cells": len(base_raw),
            "near_unique_patterns_mod_flips": len(base),
            "near_pattern_bytes": len(base) * 32,
            "far_demand_cells": len(demands),
            "far_demand_by_band": {
                str(k): v for k, v in sorted(demand_by_band.items())
            },
            "baseline_total_cost": baseline["total_cost"],
            "baseline_exact_fraction": baseline["exact_fraction"],
            "final_additions": final["additions"],
            "final_total_cost": final["total_cost"],
            "final_exact_fraction": final["exact_fraction"],
            "final_extra_pattern_bytes": final["additions"] * 32,
            "checkpoints": checkpoints,
        }
        report["results"].append(result)
        atomic_json(args.summary_json, report)
        print(
            "RESIDENT_LOD_DICTIONARY_RESULT " +
            json.dumps(result, sort_keys=True),
            flush=True)

    report["status"] = "complete"
    atomic_json(args.summary_json, report)

    aggregate = {
        "angles": len(report["results"]),
        "mean_near_unique_patterns": (
            sum(r["near_unique_patterns_mod_flips"]
                for r in report["results"]) / len(report["results"])
            if report["results"] else 0.0),
        "mean_baseline_exact_fraction": (
            sum(r["baseline_exact_fraction"] for r in report["results"]) /
            len(report["results"])
            if report["results"] else 0.0),
        "mean_final_exact_fraction": (
            sum(r["final_exact_fraction"] for r in report["results"]) /
            len(report["results"])
            if report["results"] else 0.0),
        "mean_final_extra_pattern_bytes": (
            sum(r["final_extra_pattern_bytes"] for r in report["results"]) /
            len(report["results"])
            if report["results"] else 0.0),
    }
    print(
        "RESIDENT_LOD_DICTIONARY_SUMMARY " +
        json.dumps(aggregate, sort_keys=True),
        flush=True)
    print("RESIDENT_LOD_DICTIONARY_PASS")


if __name__ == "__main__":
    main()
