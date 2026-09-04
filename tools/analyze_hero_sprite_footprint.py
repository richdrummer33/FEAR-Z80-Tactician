#!/usr/bin/env python3
"""Measure whether object-only hero LODs fit the Game Gear sprite envelope.

This does not claim sprites are the final architecture. It answers the first
necessary question created by background-plane opacity: at each distance, can
Doomguy's independently rendered silhouette be covered by a legal-ish set of
8-pixel-wide sprite blocks without exceeding configurable total/per-scanline
budgets?

The baker may choose an X/Y phase independently for each view. That models the
fact that sprite positions are pixel-addressable rather than locked to the
background tile grid. Sprite evaluation is conservatively charged for every
scanline crossed by a selected block, even when some pixels on that line are
transparent.
"""

import argparse
import json
import os
import pathlib

from analyze_doomguy_dense_corpus import Corpus
from analyze_resident_lod_dictionary import parse_csv_ints, sample_raster


def atomic_json(path, payload):
    if not path:
        return
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n")
    os.replace(tmp, path)


def occupied_blocks(raster, phase_x, phase_y, height):
    blocks = set()
    for y in range(raster.height):
        for x in range(raster.width):
            if not raster.at(x, y):
                continue
            bx = (x - phase_x) // 8
            by = (y - phase_y) // height
            blocks.add((bx, by))
    return blocks


def block_scan_peak(blocks, phase_y, height, screen_h):
    counts = [0] * screen_h
    for _, by in blocks:
        y0 = phase_y + by * height
        y1 = y0 + height - 1
        lo = max(0, y0)
        hi = min(screen_h - 1, y1)
        for y in range(lo, hi + 1):
            counts[y] += 1
    return max(counts) if counts else 0


def best_tiling(raster, height, max_total, max_scanline):
    best = None
    for py in range(height):
        for px in range(8):
            blocks = occupied_blocks(raster, px, py, height)
            total = len(blocks)
            peak = block_scan_peak(blocks, py, height, raster.height)
            overflow_scan = max(0, peak - max_scanline)
            overflow_total = max(0, total - max_total)
            # Legality first, then total sprites, then scanline pressure.
            key = (overflow_scan, overflow_total, total, peak, py, px)
            if best is None or key < best[0]:
                best = (key, {
                    "phase_x": px,
                    "phase_y": py,
                    "sprites": total,
                    "scanline_peak": peak,
                    "fits_total": total <= max_total,
                    "fits_scanline": peak <= max_scanline,
                    "fits": total <= max_total and peak <= max_scanline,
                    "blocks": sorted([list(b) for b in blocks]),
                })
    return best[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--angles", type=parse_csv_ints)
    ap.add_argument("--bands", type=parse_csv_ints,
                    default=parse_csv_ints("0,1,2,3"))
    ap.add_argument("--sprite-height", type=int, choices=[8,16], default=8)
    ap.add_argument("--max-sprites", type=int, default=64)
    ap.add_argument("--max-scanline", type=int, default=8)
    ap.add_argument("--summary-json")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    angles = args.angles if args.angles is not None else list(range(c.angles))
    result = {
        "schema": "hero-sprite-footprint-v1",
        "angles": angles,
        "bands": args.bands,
        "sprite_height": args.sprite_height,
        "max_sprites": args.max_sprites,
        "max_scanline": args.max_scanline,
        "bands_summary": [],
        "samples": [],
    }

    for band in args.bands:
        if band < 0 or band >= c.bands:
            raise SystemExit(f"band {band} outside 0..{c.bands - 1}")
        band_samples = []
        for angle in angles:
            if angle < 0 or angle >= c.angles:
                raise SystemExit(f"angle {angle} outside 0..{c.angles - 1}")
            raster = sample_raster(c, c.band(band)[angle])
            best = best_tiling(
                raster, args.sprite_height,
                args.max_sprites, args.max_scanline)
            item = {
                "band": band,
                "radius": c.radii[band],
                "angle": angle,
                "phase_x": best["phase_x"],
                "phase_y": best["phase_y"],
                "sprites": best["sprites"],
                "scanline_peak": best["scanline_peak"],
                "fits": best["fits"],
            }
            result["samples"].append(item)
            band_samples.append(item)

        result["bands_summary"].append({
            "band": band,
            "radius": c.radii[band],
            "sample_count": len(band_samples),
            "all_fit": all(x["fits"] for x in band_samples),
            "fit_fraction": (
                sum(1 for x in band_samples if x["fits"]) / len(band_samples)
                if band_samples else 1.0),
            "sprites_min": min(x["sprites"] for x in band_samples),
            "sprites_mean": (
                sum(x["sprites"] for x in band_samples) / len(band_samples)),
            "sprites_max": max(x["sprites"] for x in band_samples),
            "scanline_peak_max": max(x["scanline_peak"] for x in band_samples),
        })

    atomic_json(args.summary_json, result)
    print("HERO_SPRITE_FOOTPRINT_RESULT " + json.dumps({
        "sprite_height": result["sprite_height"],
        "bands_summary": result["bands_summary"],
    }, sort_keys=True))
    print("HERO_SPRITE_FOOTPRINT_PASS")


if __name__ == "__main__":
    main()
