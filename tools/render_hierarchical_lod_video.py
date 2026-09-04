#!/usr/bin/env python3
"""Render a four-distance hierarchical hero LOD inspection sequence.

Output is a sequence of annotated PNG frames suitable for FFmpeg.  Every band
uses the same 32 quantized angular views from the DHC1 corpus.

- R24/R28: high-detail independent oracle/reference path.
- R32/R36: hierarchical transparent-sprite reconstruction from ONE shared
  vocabulary trained jointly on both bands' demands. Measurement showed this
  beats splitting the same total budget into two independent per-band
  dictionaries at every size tested -- the two distance bands share enough
  structure that a shared vocabulary amortizes it, while a split budget
  re-learns it twice. See docs/experiments/HERO_HIERARCHICAL_SPRITE_LOD.md.

The tool also writes compact JSON metrics used verbatim for the on-frame text.
No temporal interpolation is performed; the video workflow controls how long
one quantized view is held.
"""

import argparse
import json
import pathlib
import statistics

from PIL import Image, ImageDraw, ImageFont

from analyze_doomguy_dense_corpus import Corpus, SHADE_RGB
from analyze_hierarchical_sprite_lod import learn
from analyze_hero_sprite_footprint import best_tiling
from analyze_resident_lod_dictionary import sample_raster
from analyze_sprite_resident_lod import build_groups
from resident_tile_dictionary import TileWeights
from shared_resident_lod import score_groups


BG_RGB = (8, 9, 22)
TEXT_RGB = (236, 240, 248)
ACCENT_RGB = (255, 255, 255)
SCALE = 4


def blank_raster(corpus):
    return bytearray(corpus.screen_w * corpus.screen_h)


def raster_pixels(corpus, sample):
    out = blank_raster(corpus)
    for y in range(sample.y0, sample.y1 + 1):
        for x in range(sample.x0, sample.x1 + 1):
            out[y * corpus.screen_w + x] = sample.at(x, y)
    return out


def pattern_for_match(group, shared, match):
    di = match["dictionary_index"]
    base_count = len(group["base"])
    if di < base_count:
        return group["base"][di]
    return shared[di - base_count]


def reconstruct_group_view(corpus, group, score_group, shared, band, angle):
    out = blank_raster(corpus)
    demands = group["demands"]
    matches = score_group["score"]["matches"]
    used = 0
    costs = []
    for demand, match in zip(demands, matches):
        if demand["band"] != band or demand["angle"] != angle:
            continue
        costs.append(float(match["cost"]))
        pattern = pattern_for_match(group, shared, match)
        if not any(pattern):
            continue
        used += 1
        x0, y0 = int(demand["x"]), int(demand["y"])
        for py in range(8):
            y = y0 + py
            if y < 0 or y >= corpus.screen_h:
                continue
            for px in range(8):
                x = x0 + px
                if x < 0 or x >= corpus.screen_w:
                    continue
                v = pattern[py * 8 + px]
                if v:
                    out[y * corpus.screen_w + x] = v
    return out, used, costs


def semantic_image(corpus, pixels):
    img = Image.new("RGB", (corpus.screen_w, corpus.screen_h), BG_RGB)
    dst = img.load()
    for y in range(corpus.screen_h):
        row = y * corpus.screen_w
        for x in range(corpus.screen_w):
            v = pixels[row + x]
            if not v:
                continue
            if v < len(SHADE_RGB):
                dst[x, y] = SHADE_RGB[v]
            else:
                dst[x, y] = (240, 240, 240)
    return img.resize(
        (corpus.screen_w * SCALE, corpus.screen_h * SCALE),
        Image.Resampling.NEAREST)


def font(size=16):
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ]
    for path in candidates:
        if pathlib.Path(path).exists():
            return ImageFont.truetype(path, size=size)
    return ImageFont.load_default()


def draw_overlay(img, lines):
    d = ImageDraw.Draw(img)
    f = font(15)
    x, y = 12, 10
    line_h = 19
    for i, text in enumerate(lines):
        fill = ACCENT_RGB if i == 0 else TEXT_RGB
        d.text((x, y), text, font=f, fill=fill,
               stroke_width=2, stroke_fill=(0, 0, 0))
        y += line_h


def bbox_metrics(samples):
    widths = [s.width for s in samples]
    heights = [s.height for s in samples]
    pixels = [s.pixels for s in samples]
    return {
        "bbox_width_mean": statistics.mean(widths),
        "bbox_height_mean": statistics.mean(heights),
        "owned_pixels_mean": statistics.mean(pixels),
    }


def sprite_footprint_metrics(corpus, band):
    samples = []
    for angle in range(corpus.angles):
        raster = sample_raster(corpus, corpus.band(band)[angle])
        t = best_tiling(raster, 8, 64, 8)
        samples.append(t)
    return {
        "fit_fraction": sum(1 for s in samples if s["fits"]) / len(samples),
        "sprites_mean": statistics.mean(s["sprites"] for s in samples),
        "sprites_max": max(s["sprites"] for s in samples),
        "scanline_peak": max(s["scanline_peak"] for s in samples),
    }


def score_band(groups, score, band):
    costs = []
    refs = []
    for gscore in score["groups"]:
        group = groups[gscore["group_index"]]
        n = 0
        for demand, match in zip(group["demands"],
                                 gscore["score"]["matches"]):
            if demand["band"] != band:
                continue
            costs.append(float(match["cost"]))
            pattern = pattern_for_match(group, [], match) if False else None
            # Dictionary index zero is implicit transparent. Any other index
            # corresponds to a visible sprite reference in these empty-base or
            # core-base groups.
            if match["dictionary_index"] != 0:
                n += 1
        refs.append(n)
    return {
        "mean_cost_per_demand": statistics.mean(costs) if costs else 0.0,
        "sprite_refs_mean": statistics.mean(refs) if refs else 0.0,
        "sprite_refs_max": max(refs) if refs else 0,
        "sat_bytes_mean": statistics.mean(3 * n + 1 for n in refs) if refs else 0.0,
        "sat_bytes_max": max((3 * n + 1 for n in refs), default=0),
        "naive_map_bytes": sum(3 * n + 1 for n in refs),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("outdir")
    ap.add_argument("--flat-patterns", type=int, default=192)
    ap.add_argument("--lloyd-iterations", type=int, default=4)
    args = ap.parse_args()

    c = Corpus(args.corpus)
    if c.angles != 32 or c.bands < 4:
        raise SystemExit("video renderer expects 32 angles and at least 4 bands")
    out = pathlib.Path(args.outdir)
    frames = out / "frames"
    frames.mkdir(parents=True, exist_ok=True)

    weights = TileWeights(12.0, 1.0)
    angles = list(range(c.angles))

    midfar_groups, _ = build_groups(c, angles, [2, 3])
    flat_result = learn(midfar_groups, args.flat_patterns,
                        weights, args.lloyd_iterations)
    vocabulary = flat_result["shared"]
    midfar_score = score_groups(midfar_groups, vocabulary,
                                weights, allow_flips=False)

    metrics = {
        "schema": "hierarchical-lod-video-v2",
        "angles": c.angles,
        "angle_step_deg": 360.0 / c.angles,
        "distances": [],
        "room_reference": {
            "background_cache_with_192_hero_patterns": 256,
            "measured_room_scheduled_peak": 33,
            "approx_pattern_upload_ceiling": 48,
        },
    }

    footprint = [sprite_footprint_metrics(c, b) for b in range(4)]
    bbox = [bbox_metrics(c.band(b)) for b in range(4)]

    # Band-specific codec measurements sliced from the one joint solve.
    far_band = score_band(midfar_groups, midfar_score, 3)
    mid_band = score_band(midfar_groups, midfar_score, 2)

    distance_metrics = [
        {
            "band": 0, "radius": c.radii[0],
            "representation": "HIGH-DETAIL REFERENCE / NEAR PATH",
            "resident_patterns": None, "resident_bytes": None,
            "angle_pattern_uploads": None,
            **footprint[0], **bbox[0],
        },
        {
            "band": 1, "radius": c.radii[1],
            "representation": "HIGH-DETAIL REFERENCE / NEAR PATH",
            "resident_patterns": None, "resident_bytes": None,
            "angle_pattern_uploads": None,
            **footprint[1], **bbox[1],
        },
        {
            "band": 2, "radius": c.radii[2],
            "representation": "HIERARCHICAL TRANSPARENT SPRITES (SHARED)",
            "resident_patterns": len(vocabulary),
            "resident_bytes": len(vocabulary) * 32,
            "angle_pattern_uploads": 0,
            **footprint[2], **bbox[2], **mid_band,
        },
        {
            "band": 3, "radius": c.radii[3],
            "representation": "HIERARCHICAL TRANSPARENT SPRITES (SHARED)",
            "resident_patterns": len(vocabulary),
            "resident_bytes": len(vocabulary) * 32,
            "angle_pattern_uploads": 0,
            **footprint[3], **bbox[3], **far_band,
        },
    ]
    metrics["distances"] = distance_metrics

    frame_index = 0
    for band, dm in enumerate(distance_metrics):
        for angle in angles:
            if band < 2:
                pixels = raster_pixels(c, c.band(band)[angle])
            else:
                gi = angle
                pixels, _, _ = reconstruct_group_view(
                    c, midfar_groups[gi], midfar_score["groups"][gi],
                    vocabulary, band, angle)

            img = semantic_image(c, pixels)
            deg = angle * metrics["angle_step_deg"]
            lines = [
                f"R={dm['radius']:.0f}  |  {dm['representation']}",
                f"angle {angle+1:02d}/32  |  {deg:6.2f} deg  |  step {metrics['angle_step_deg']:.2f} deg",
            ]
            if band < 2:
                lines += [
                    f"projected hero: {dm['owned_pixels_mean']:.0f} px avg  |  bbox {dm['bbox_width_mean']:.1f} x {dm['bbox_height_mean']:.1f} px",
                    f"8x8 sprite legality: {100.0*dm['fit_fraction']:.1f}% views  |  mean {dm['sprites_mean']:.1f} / max {dm['sprites_max']} sprites",
                    f"sprite scanline peak: {dm['scanline_peak']}/8  |  resident LOD vocab not active",
                    "room: independent coarse-lattice lit background path",
                ]
            else:
                lines += [
                    f"shared hero vocab: {dm['resident_patterns']} patterns  |  {dm['resident_bytes']} B VRAM graphics",
                    f"distance/angle-change hero pattern uploads: {dm['angle_pattern_uploads']}  |  mean refs {dm['sprite_refs_mean']:.1f} / max {dm['sprite_refs_max']}",
                    f"SAT payload: mean {dm['sat_bytes_mean']:.1f} B / max {dm['sat_bytes_max']} B  |  scan peak {dm['scanline_peak']}/8",
                    f"codec weighted error / demanded tile: {dm['mean_cost_per_demand']:.2f}  |  naive 32-view map {dm['naive_map_bytes']} B",
                    "same 192-pattern vocabulary serves both R32 and R36; zero uploads either way",
                    "lit-room reference: 256-pattern BG cache | measured scheduled peak 33/~48",
                ]

            draw_overlay(img, lines)
            img.save(frames / f"frame-{frame_index:04d}.png", optimize=True)
            frame_index += 1

    # Four-column contact sheet: first angle from each distance.
    firsts = []
    for band in range(4):
        firsts.append(Image.open(frames / f"frame-{band*32:04d}.png"))
    sheet = Image.new("RGB", (firsts[0].width * 2, firsts[0].height * 2), BG_RGB)
    for i, im in enumerate(firsts):
        sheet.paste(im, ((i % 2) * im.width, (i // 2) * im.height))
    sheet.save(out / "distance-contact-sheet.png", optimize=True)

    (out / "metrics.json").write_text(
        json.dumps(metrics, sort_keys=True, indent=2) + "\n")
    print("HIERARCHICAL_LOD_VIDEO_FRAMES " + json.dumps({
        "frames": frame_index,
        "angles": c.angles,
        "distances": [d["radius"] for d in distance_metrics],
        "flat_patterns": len(vocabulary),
    }, sort_keys=True))
    print("HIERARCHICAL_LOD_VIDEO_FRAMES_PASS")


if __name__ == "__main__":
    main()
