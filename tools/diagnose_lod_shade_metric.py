#!/usr/bin/env python3
"""Compare the far LOD reconstruction under raw-index and perceptual shade cost.

The hierarchical sprite vocabulary looked structurally sound on paper while the
rendered far views read as speckle. This isolates one candidate cause: the
compositor's semantic shade indices are not ordered by brightness, so measuring
shade distance with raw index arithmetic prices the darkest-to-brightest swaps
as if they were the smallest possible change.

Everything else about the pipeline is held constant. Only the shade distance
changes, so any difference in the rendered result is attributable to it.
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from analyze_doomguy_dense_corpus import SHADE_RGB, Corpus       # noqa: E402
from analyze_sprite_resident_lod import build_groups             # noqa: E402
from resident_tile_dictionary import (                           # noqa: E402
    PERCEPTUAL_RANK, TileWeights, flip_pattern,
)
from shared_resident_lod import (                                # noqa: E402
    greedy_shared_growth, refine_shared_dictionary, score_groups,
)

BG = (24, 24, 28)
GRID = (60, 60, 72)
WRONG_SHADE = (235, 170, 40)   # right coverage, wrong tone
LOST = (230, 60, 60)           # oracle had hero here, reconstruction does not
GAINED = (0, 220, 120)         # reconstruction invents hero here


def learn(groups, count, weights, iterations):
    growth = greedy_shared_growth(groups, count, weights, allow_flips=False)
    return refine_shared_dictionary(
        groups, growth["shared"][:count], iterations=iterations,
        weights=weights, allow_flips=False)


def reconstruct(groups, shared, score, views):
    """Paint each view's matched dictionary tiles back onto a screen canvas."""
    canvas = {v["view_index"]: {} for v in views}
    for gscore in score["groups"]:
        group = groups[gscore["group_index"]]
        for demand, match in zip(group["demands"], gscore["score"]["matches"]):
            table = list(group["base"]) + list(shared)
            pattern = table[match["dictionary_index"]]
            if match.get("flip_h") or match.get("flip_v"):
                pattern = flip_pattern(
                    pattern, match.get("flip_h"), match.get("flip_v"))
            plane = canvas[demand["view_index"]]
            for i, value in enumerate(pattern):
                if value:
                    plane[(demand["x"] + i % 8, demand["y"] + i // 8)] = value
    return canvas


def oracle_plane(groups, views):
    planes = {v["view_index"]: {} for v in views}
    for group in groups:
        for demand in group["demands"]:
            plane = planes[demand["view_index"]]
            for i, value in enumerate(demand["pattern"]):
                if value:
                    plane[(demand["x"] + i % 8, demand["y"] + i // 8)] = value
    return planes


def bounds(planes):
    keys = [k for plane in planes.values() for k in plane]
    xs = [k[0] for k in keys]
    ys = [k[1] for k in keys]
    return min(xs) - 2, min(ys) - 2, max(xs) + 2, max(ys) + 2


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [BG] * (w * h)

    def set(self, x, y, rgb):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = rgb

    def save(self, path):
        try:
            from PIL import Image
        except ImportError:
            body = bytearray()
            for r, g, b in self.px:
                body += bytes((r, g, b))
            path = path.with_suffix(".ppm")
            path.write_bytes(b"P6\n%d %d\n255\n" % (self.w, self.h) +
                             bytes(body))
            return path
        img = Image.new("RGB", (self.w, self.h))
        img.putdata(self.px)
        img.save(path)
        return path


def sheet(columns, box, scale, path):
    """columns: list of (plane, reference_plane_or_None)."""
    x0, y0, x1, y1 = box
    cw, ch = (x1 - x0 + 1) * scale, (y1 - y0 + 1) * scale
    c = Canvas(cw * len(columns) + len(columns) - 1, ch)
    for i, (plane, ref) in enumerate(columns):
        ox = i * (cw + 1)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                v = plane.get((x, y), 0)
                if ref is None:
                    rgb = SHADE_RGB[v] if v else BG
                else:
                    o = ref.get((x, y), 0)
                    if v and o:
                        rgb = BG if v == o else WRONG_SHADE
                    elif o:
                        rgb = LOST
                    elif v:
                        rgb = GAINED
                    else:
                        rgb = BG
                for sy in range(scale):
                    for sx in range(scale):
                        c.set(ox + (x - x0) * scale + sx,
                              (y - y0) * scale + sy, rgb)
        if i:
            for y in range(ch):
                c.set(ox - 1, y, GRID)
    return c.save(path)


def audit(oracle, recon, rank):
    """Where does the error land, in units a human can argue about?"""
    lost = gained = 0
    stops = [0] * 6
    for key, o in oracle.items():
        v = recon.get(key, 0)
        if not v:
            lost += 1
            continue
        d = abs(rank[o] - rank[v])
        stops[d] += 1
    for key, v in recon.items():
        if not oracle.get(key, 0):
            gained += 1
    filled = sum(stops)
    return {
        "oracle_px": len(oracle), "lost": lost, "gained": gained,
        "shade_exact": stops[0],
        "off_by_1": stops[1], "off_by_2": stops[2],
        "off_by_3_plus": sum(stops[3:]),
        "mean_stops_off": (sum(i * n for i, n in enumerate(stops)) / filled
                           if filled else 0.0),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("outdir")
    ap.add_argument("--band", type=int, default=3)
    ap.add_argument("--patterns", type=int, default=16)
    ap.add_argument("--angles", type=int, default=8)
    ap.add_argument("--lloyd-iterations", type=int, default=4)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--assert-gross-error-ratio", type=float,
                    help="fail unless raw-index gross tonal error (three or "
                         "more ramp stops wrong) is at least this many times "
                         "the perceptual variant's")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    stride = max(1, c.angles // args.angles)
    angles = [i * stride for i in range(args.angles)]
    out = pathlib.Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    variants = {
        "raw_index": TileWeights(12.0, 1.0, None),
        "perceptual": TileWeights(12.0, 1.0, PERCEPTUAL_RANK),
    }

    print("LOD_SHADE_METRIC_DIAGNOSTIC v1")
    print(f"band={args.band} radius={c.radii[args.band]} "
          f"patterns={args.patterns} angles={angles}")

    results = {}
    gross = {}
    oracle = None
    for name, weights in variants.items():
        groups, views = build_groups(c, angles, [args.band])
        trained = learn(groups, args.patterns, weights, args.lloyd_iterations)
        shared = trained["shared"]
        score = score_groups(groups, shared, weights, allow_flips=False)
        if oracle is None:
            oracle = oracle_plane(groups, views)
            box = bounds(oracle)
        results[name] = (reconstruct(groups, shared, score, views), views)

        # Always report in the perceptual space, whichever space trained it.
        agg = {}
        for v in views:
            a = audit(oracle[v["view_index"]],
                      results[name][0][v["view_index"]], PERCEPTUAL_RANK)
            for k, val in a.items():
                agg[k] = agg.get(k, 0) + val
        n = len(views)
        print(f"\ntrained_with={name}")
        print(f"  silhouette lost={agg['lost']} gained={agg['gained']} "
              f"of {agg['oracle_px']} oracle pixels "
              f"({100.0 * (agg['lost'] + agg['gained']) / agg['oracle_px']:.2f}%)")
        print(f"  shade exact={agg['shade_exact']} "
              f"off_by_1={agg['off_by_1']} off_by_2={agg['off_by_2']} "
              f"off_by_3_plus={agg['off_by_3_plus']}")
        print(f"  mean_ramp_stops_wrong_per_filled_pixel="
              f"{agg['mean_stops_off'] / n:.4f}")
        gross[name] = agg["off_by_3_plus"]

    if args.assert_gross_error_ratio:
        raw, fixed = gross["raw_index"], gross["perceptual"]
        print(f"\ngross_tonal_error raw_index={raw} perceptual={fixed} "
              f"ratio={raw / fixed if fixed else float('inf'):.1f} "
              f"required>={args.assert_gross_error_ratio}")
        if fixed * args.assert_gross_error_ratio > raw:
            raise SystemExit(
                "perceptual shade ordering no longer suppresses gross tonal "
                f"error (raw={raw}, perceptual={fixed})")

    # One wide before/after strip, because the per-angle error maps are for
    # arguing about and this is for looking at.
    views = results["raw_index"][1]
    rows = []
    for v in views:
        vi = v["view_index"]
        rows.append([(oracle[vi], None),
                     (results["raw_index"][0][vi], None),
                     (results["perceptual"][0][vi], None)])
    x0, y0, x1, y1 = box
    cw = (x1 - x0 + 1) * args.scale
    ch = (y1 - y0 + 1) * args.scale
    grid = Canvas(cw * 3 + 2, ch * len(rows) + len(rows) - 1)
    for r, cols in enumerate(rows):
        oy = r * (ch + 1)
        for ci, (plane, _ref) in enumerate(cols):
            ox = ci * (cw + 1)
            for y in range(y0, y1 + 1):
                for x in range(x0, x1 + 1):
                    v2 = plane.get((x, y), 0)
                    rgb = SHADE_RGB[v2] if v2 else BG
                    for sy in range(args.scale):
                        for sx in range(args.scale):
                            grid.set(ox + (x - x0) * args.scale + sx,
                                     oy + (y - y0) * args.scale + sy, rgb)
    for ci in (1, 2):
        for y in range(grid.h):
            grid.set(ci * (cw + 1) - 1, y, GRID)
    print("\nstrip columns: oracle | raw-index | perceptual")
    print(f"wrote {grid.save(out / f'compare-b{args.band}.png')}")

    for i, v in enumerate(results["raw_index"][1]):
        vi = v["view_index"]
        cols = [
            (oracle[vi], None),
            (results["raw_index"][0][vi], None),
            (results["raw_index"][0][vi], oracle[vi]),
            (results["perceptual"][0][vi], None),
            (results["perceptual"][0][vi], oracle[vi]),
        ]
        p = sheet(cols, box, args.scale,
                  out / f"metric-b{args.band}-a{v['angle']:03d}.png")
        if i == 0:
            print(f"\ncolumns: oracle | raw-index recon | its error | "
                  f"perceptual recon | its error")
            print(f"error colours: orange=wrong tone, red=lost, green=invented")
        print(f"wrote {p}")


if __name__ == "__main__":
    main()
