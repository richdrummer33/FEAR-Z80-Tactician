#!/usr/bin/env python3
"""Hierarchical transparent sprite LOD codebook.

Two runtime policies are measured here, not one.

`--flat-patterns` (the shipped policy): one vocabulary, trained jointly on the
union of mid and far demands, used unchanged by both bands. No core, no
refinement prefix, no distance-triggered pattern upload at all -- the room's
measured VRAM headroom (see docs/experiments/HERO_HIERARCHICAL_SPRITE_LOD.md)
turned out generous enough that there is no reason to keep the vocabulary
artificially small. The worst-error views (arm/rifle silhouettes at extreme
angles, reported separately per angle) went from blocky rectangular stumps at
16 patterns to legible limbs by 64 and near-oracle by 128; this sweep is what
picks the shipped size.

`--core-patterns` / `--refinement-patterns` (retained for comparison): the
originally proposed prefix-nested scheme, where far sees only a small core and
mid adds a refinement layer on top of it. Measurement showed the nesting
penalty tracks the ABSOLUTE size of the frozen core (1.8-3.5% at a 16-pattern
core, 7.2-11.9% at 32-64), so it only pays off while the core stays small --
exactly the regime this project no longer needs, now that a large flat
vocabulary fits the VRAM budget with room to spare. Kept so the tradeoff stays
measured rather than asserted.
"""

import argparse
import json
import os
import pathlib

from analyze_doomguy_dense_corpus import Corpus
from analyze_resident_lod_dictionary import parse_csv_ints
from analyze_sprite_resident_lod import build_groups
from diagnose_lod_shade_metric import audit, oracle_plane, reconstruct
from resident_tile_dictionary import PERCEPTUAL_RANK, TileWeights
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


def pixel_audit(groups, dictionary, score, views, rank):
    """Reconstruction fidelity in the units a human argues about, not cost
    units: lost/gained silhouette pixels and how many ramp stops off a
    correctly-covered pixel's shade lands. This is what actually predicts
    whether a view looks like Doomguy or like static.
    """
    oracle = oracle_plane(groups, views)
    recon = reconstruct(groups, dictionary, score, views)
    agg = {}
    for v in views:
        vi = v["view_index"]
        a = audit(oracle[vi], recon[vi], rank)
        for k, val in a.items():
            agg[k] = agg.get(k, 0) + val
    n = len(views)
    filled = agg["shade_exact"] + agg["off_by_1"] + agg["off_by_2"] + \
        agg["off_by_3_plus"]
    return {
        "silhouette_error_pct": (
            100.0 * (agg["lost"] + agg["gained"]) / agg["oracle_px"]
            if agg["oracle_px"] else 0.0),
        "gross_tonal_errors": agg["off_by_3_plus"],
        "mean_ramp_stops_wrong": (
            (agg["off_by_1"] + 2 * agg["off_by_2"] + 3 * agg["off_by_3_plus"])
            / filled if filled else 0.0),
    }


def flat_point(midfar_groups, far_groups, midfar_views, far_views,
              count, weights, iterations):
    result = learn(midfar_groups, count, weights, iterations)
    dictionary = result["shared"]
    midfar_score = result["final_score"]
    far_score = score_groups(far_groups, dictionary, weights,
                             allow_flips=False)
    return {
        "patterns": count,
        "resident_bytes": count * 32,
        "midfar_mean_cost": midfar_score["mean_cost"],
        "far_mean_cost": far_score["mean_cost"],
        "midfar_audit": pixel_audit(
            midfar_groups, dictionary, midfar_score, midfar_views,
            PERCEPTUAL_RANK),
        "far_audit": pixel_audit(
            far_groups, dictionary, far_score, far_views, PERCEPTUAL_RANK),
        "midfar_replay": replay_stats(midfar_groups, midfar_score,
                                      midfar_views),
        "far_replay": replay_stats(far_groups, far_score, far_views),
        "lloyd": result["history"],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--angles", type=parse_csv_ints)
    ap.add_argument("--far-band", type=int, default=3)
    ap.add_argument("--mid-band", type=int, default=2)
    ap.add_argument("--flat-patterns", type=parse_csv_ints,
                    default=parse_csv_ints("64,128,192"),
                    help="192 is the hardware ceiling for this sprite/name-"
                         "table VRAM layout: sprite tile id N reads unified "
                         "pattern tile 256+N, and the name table begins at "
                         "unified tile 448, so 191 is the last id that does "
                         "not read into it")
    ap.add_argument("--core-patterns", type=int, default=0,
                    help="0 skips the nested-prefix comparison entirely")
    ap.add_argument("--refinement-patterns", type=parse_csv_ints,
                    default=parse_csv_ints("16,48"))
    ap.add_argument("--lloyd-iterations", type=int, default=4)
    ap.add_argument("--summary-json")
    ap.add_argument("--assert-far-silhouette-pct", type=float,
                    help="fail unless the largest --flat-patterns point's "
                         "far-band silhouette error is at or below this")
    ap.add_argument("--assert-resident-bytes-max", type=int,
                    help="fail unless every --flat-patterns point fits in "
                         "this many resident bytes")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    angles = args.angles if args.angles is not None else list(range(c.angles))
    weights = TileWeights(12.0, 1.0)

    far_groups, far_views = build_groups(c, angles, [args.far_band])
    midfar_groups, midfar_views = build_groups(
        c, angles, [args.mid_band, args.far_band])

    flat_points = []
    for count in args.flat_patterns:
        if args.assert_resident_bytes_max and \
                count * 32 > args.assert_resident_bytes_max:
            raise SystemExit(
                f"flat point {count} patterns ({count * 32} bytes) exceeds "
                f"--assert-resident-bytes-max={args.assert_resident_bytes_max}")
        # Fresh groups per point: prepare_group's demand list is shared but
        # score caching keys off (weights, allow_flips), which do not change
        # here, so reuse would be safe too -- rebuilding is just cheap insurance
        # against a future caller passing different weights per point.
        far_groups_f, far_views_f = build_groups(c, angles, [args.far_band])
        midfar_groups_f, midfar_views_f = build_groups(
            c, angles, [args.mid_band, args.far_band])
        flat_points.append(flat_point(
            midfar_groups_f, far_groups_f, midfar_views_f, far_views_f,
            count, weights, args.lloyd_iterations))

    nested_points = []
    if args.core_patterns:
        core_result = learn(
            far_groups, args.core_patterns, weights, args.lloyd_iterations)
        core = core_result["shared"]
        far_core_score = score_groups(far_groups, core, weights,
                                      allow_flips=False)
        for extra_count in args.refinement_patterns:
            staged_groups = with_fixed_base(midfar_groups, core)
            extra_result = learn(
                staged_groups, extra_count, weights, args.lloyd_iterations)
            extras = extra_result["shared"]
            staged_score = score_groups(
                staged_groups, extras, weights, allow_flips=False)
            total_patterns = len(core) + len(extras)
            flat_same_size = learn(
                midfar_groups, total_patterns, weights, args.lloyd_iterations)
            flat_score = flat_same_size["final_score"]
            nested_points.append({
                "core_patterns": len(core),
                "refinement_patterns": len(extras),
                "total_patterns": total_patterns,
                "resident_bytes_if_full": total_patterns * 32,
                "far_resident_bytes": len(core) * 32,
                "far_core_mean_cost": far_core_score["mean_cost"],
                "midfar_nested_mean_cost": staged_score["mean_cost"],
                "midfar_flat_same_size_mean_cost": flat_score["mean_cost"],
                "hierarchy_penalty_pct": (
                    round(100.0 * (staged_score["total_cost"] -
                                   flat_score["total_cost"]) /
                          flat_score["total_cost"], 4)
                    if flat_score["total_cost"] else 0.0),
            })

    result = {
        "schema": "hierarchical-sprite-lod-v2",
        "angles": angles,
        "far_band": args.far_band,
        "mid_band": args.mid_band,
        "shipped_policy": "flat",
        "flat_points": flat_points,
        "nested_points": nested_points,
        "runtime_policy": {
            "flat": "one shared vocabulary sized from flat_points; identical "
                    "for mid and far bands; zero pattern uploads for either "
                    "angle or mid/far distance changes",
            "nested_superseded": "small core + refinement prefix; measured "
                                 "for comparison only, see nested_points",
        },
    }
    atomic_json(args.summary_json, result)
    print("HIERARCHICAL_SPRITE_LOD_RESULT " + json.dumps(result, sort_keys=True))

    if args.assert_far_silhouette_pct is not None and flat_points:
        worst = flat_points[-1]
        got = worst["far_audit"]["silhouette_error_pct"]
        print(f"largest flat point: {worst['patterns']} patterns, "
              f"far silhouette error {got:.2f}%, "
              f"required <= {args.assert_far_silhouette_pct}")
        if got > args.assert_far_silhouette_pct:
            raise SystemExit(
                f"far-band silhouette error {got:.2f}% exceeds "
                f"--assert-far-silhouette-pct={args.assert_far_silhouette_pct}")

    print("HIERARCHICAL_SPRITE_LOD_PASS")


if __name__ == "__main__":
    main()
