#!/usr/bin/env python3
"""Shared resident hero LOD vocabulary for the transparent sprite plane.

Differences from the background-tile experiments:
- per-view sprite blocks may choose an arbitrary 0..7 X/Y phase;
- colour zero is genuinely transparent;
- H/V pattern flips are NOT assumed free;
- every selected block is one 8x8 sprite;
- one learned codebook is shared across all requested angles/distances.

The result estimates both resident pattern bytes and sprite attribute traffic.
It remains an offline visual-codec experiment; room occlusion/priority and
sprite coexistence with gameplay VFX are separate integration checks.
"""

import argparse
import json
import os
import pathlib

from analyze_doomguy_dense_corpus import Corpus
from analyze_hero_sprite_footprint import best_tiling
from analyze_resident_lod_dictionary import parse_csv_ints, sample_raster
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


def extract_pattern(raster, x0, y0):
    out = bytearray(64)
    p = 0
    for y in range(y0, y0 + 8):
        for x in range(x0, x0 + 8):
            out[p] = raster.at(x, y)
            p += 1
    return bytes(out)


def build_groups(corpus, angles, bands, max_sprites=64, max_scanline=8):
    groups = []
    views = []
    for angle in angles:
        demands = []
        for band in bands:
            raster = sample_raster(corpus, corpus.band(band)[angle])
            tiling = best_tiling(
                raster, 8, max_sprites, max_scanline)
            view_index = len(views)
            view = {
                "view_index": view_index,
                "angle": angle,
                "band": band,
                "radius": corpus.radii[band],
                "phase_x": tiling["phase_x"],
                "phase_y": tiling["phase_y"],
                "sprites_oracle": tiling["sprites"],
                "scanline_peak_oracle": tiling["scanline_peak"],
                "footprint_fits": tiling["fits"],
            }
            views.append(view)
            for bx, by in tiling["blocks"]:
                x0 = tiling["phase_x"] + bx * 8
                y0 = tiling["phase_y"] + by * 8
                pattern = extract_pattern(raster, x0, y0)
                if not any(pattern):
                    continue
                demands.append({
                    "pattern": pattern,
                    "view_index": view_index,
                    "angle": angle,
                    "band": band,
                    "radius": corpus.radii[band],
                    "bx": bx,
                    "by": by,
                    "x": x0,
                    "y": y0,
                })
        groups.append(prepare_group([], demands, allow_flips=False))
    return groups, views


def mapping_stats(groups, score, views):
    per_view = {
        v["view_index"]: {
            "view_index": v["view_index"],
            "angle": v["angle"],
            "band": v["band"],
            "radius": v["radius"],
            "sprite_refs": 0,
            "dropped_to_transparent": 0,
        }
        for v in views
    }
    for gscore in score["groups"]:
        gi = gscore["group_index"]
        demands = groups[gi]["demands"]
        base_count = gscore["base_count"]  # one implicit transparent entry
        for demand, match in zip(demands, gscore["score"]["matches"]):
            v = per_view[demand["view_index"]]
            if match["dictionary_index"] < base_count:
                v["dropped_to_transparent"] += 1
            else:
                v["sprite_refs"] += 1

    items = [per_view[i] for i in sorted(per_view)]
    for item in items:
        # SAT is split physically into one Y byte per sprite and X/tile pairs.
        # Count only changed active records here; add one byte for end marker.
        item["sat_payload_bytes"] = 3 * item["sprite_refs"] + 1
        # Straightforward ROM replay record: count + (x,y,tile) per sprite.
        item["naive_rom_map_bytes"] = 1 + 3 * item["sprite_refs"]
    return {
        "views": items,
        "sprite_refs_mean": (
            sum(x["sprite_refs"] for x in items) / len(items) if items else 0),
        "sprite_refs_max": max((x["sprite_refs"] for x in items), default=0),
        "sat_payload_bytes_mean": (
            sum(x["sat_payload_bytes"] for x in items) / len(items)
            if items else 0),
        "sat_payload_bytes_max": max(
            (x["sat_payload_bytes"] for x in items), default=0),
        "naive_rom_map_bytes_total": sum(
            x["naive_rom_map_bytes"] for x in items),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--angles", type=parse_csv_ints)
    ap.add_argument("--bands", type=parse_csv_ints,
                    default=parse_csv_ints("2,3"))
    ap.add_argument("--patterns", type=parse_csv_ints,
                    default=parse_csv_ints("16,32,64"))
    ap.add_argument("--lloyd-iterations", type=int, default=4)
    ap.add_argument("--max-sprites", type=int, default=64)
    ap.add_argument("--max-scanline", type=int, default=8)
    ap.add_argument("--summary-json")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    angles = args.angles if args.angles is not None else list(range(c.angles))
    groups, views = build_groups(
        c, angles, args.bands, args.max_sprites, args.max_scanline)
    weights = TileWeights(12.0, 1.0)
    baseline = score_groups(groups, [], weights, allow_flips=False)
    growth = greedy_shared_growth(
        groups, max(args.patterns), weights, allow_flips=False)

    points = []
    for requested in args.patterns:
        actual = min(requested, len(growth["shared"]))
        seeded = growth["shared"][:actual]
        greedy = score_groups(groups, seeded, weights, allow_flips=False)
        learned = refine_shared_dictionary(
            groups, seeded, iterations=args.lloyd_iterations,
            weights=weights, allow_flips=False)
        final = learned["final_score"]
        mapping = mapping_stats(groups, final, views)
        points.append({
            "requested_patterns": requested,
            "actual_patterns": actual,
            "resident_pattern_bytes": actual * 32,
            "learned_total_cost": final["total_cost"],
            "mean_cost": final["mean_cost"],
            "baseline_improvement_pct": (
                round(100.0 * (baseline["total_cost"] - final["total_cost"]) /
                      baseline["total_cost"], 4)
                if baseline["total_cost"] else 0.0),
            "learned_vs_greedy_improvement_pct": (
                round(100.0 * (greedy["total_cost"] - final["total_cost"]) /
                      greedy["total_cost"], 4)
                if greedy["total_cost"] else 0.0),
            "equivalent_silhouette_mismatch_pixels_per_sprite": (
                final["mean_cost"] / weights.silhouette),
            "mapping": mapping,
            "lloyd_history": learned["history"],
        })

    result = {
        "schema": "sprite-resident-lod-v1",
        "angles": angles,
        "bands": args.bands,
        "view_count": len(views),
        "all_oracle_footprints_fit": all(v["footprint_fits"] for v in views),
        "baseline_total_cost": baseline["total_cost"],
        "points": points,
        "notes": {
            "transparent_zero": True,
            "hardware_pattern_flips_assumed": False,
            "room_background_compositing_required": False,
            "occlusion_priority_not_yet_validated": True,
        },
    }
    atomic_json(args.summary_json, result)
    print("SPRITE_RESIDENT_LOD_RESULT " + json.dumps(result, sort_keys=True))
    print("SPRITE_RESIDENT_LOD_PASS")


if __name__ == "__main__":
    main()
