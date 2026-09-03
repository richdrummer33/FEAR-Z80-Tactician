#!/usr/bin/env python3
"""Contact sheets for a DHC1 dense hero-view corpus.

The numeric report card says how large the errors are. These sheets say what
they look like, which is the only way to judge whether an error that measures
small is actually acceptable on a 160x144 screen -- and the only way a human
can sanity-check that the corpus is capturing what we think it is.

Three sheets per band:
  orbit  -- the normalized views themselves, evenly spaced around the orbit
  step   -- what one angular step changes, with added/removed pixels flagged
  anchor -- a view reconstructed from a distant anchor, with its damage flagged

Writes PNG when Pillow is importable and Netpbm otherwise, so it degrades
cleanly on a bare CI runner.
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from analyze_doomguy_dense_corpus import Corpus, SHADE_RGB  # noqa: E402

BG = (24, 24, 28)
GRID = (60, 60, 72)
ADDED = (0, 220, 120)     # present in the target, missing from the reference
REMOVED = (230, 60, 60)   # present in the reference, gone in the target
KEPT = (70, 70, 80)


class Canvas:
    def __init__(self, w, h, fill=BG):
        self.w, self.h = w, h
        self.px = [fill] * (w * h)

    def set(self, x, y, rgb):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = rgb

    def vline(self, x, rgb):
        for y in range(self.h):
            self.set(x, y, rgb)

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


def local_box(views, pad=3):
    """One local-frame window that fits every view in the list."""
    x0 = min(v.x0 - round(v.anchor_x) for v in views) - pad
    x1 = max(v.x1 - round(v.anchor_x) for v in views) + pad
    y0 = min(v.y0 - round(v.anchor_y) for v in views) - pad
    y1 = max(v.y1 - round(v.anchor_y) for v in views) + pad
    return x0, y0, x1, y1


def blit(canvas, view, box, ox, oy, scale, other=None):
    """Draw one view. With `other`, draw the silhouette difference instead."""
    x0, y0, x1, y1 = box
    ax, ay = round(view.anchor_x), round(view.anchor_y)
    bx, by = (round(other.anchor_x), round(other.anchor_y)) if other else (0, 0)
    for ly in range(y0, y1 + 1):
        for lx in range(x0, x1 + 1):
            v = view.at(lx + ax, ly + ay)
            if other is None:
                rgb = SHADE_RGB[v] if v else BG
            else:
                o = other.at(lx + bx, ly + by)
                if v and o:
                    rgb = KEPT
                elif v:
                    rgb = ADDED
                elif o:
                    rgb = REMOVED
                else:
                    rgb = BG
            for sy in range(scale):
                for sx in range(scale):
                    canvas.set(ox + (lx - x0) * scale + sx,
                               oy + (ly - y0) * scale + sy, rgb)


def sheet(views, box, scale, refs=None):
    x0, y0, x1, y1 = box
    cw, ch = (x1 - x0 + 1) * scale, (y1 - y0 + 1) * scale
    c = Canvas(cw * len(views) + len(views) - 1, ch)
    for i, v in enumerate(views):
        ox = i * (cw + 1)
        blit(c, v, box, ox, 0, scale, refs[i] if refs else None)
        if i:
            c.vline(ox - 1, GRID)
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("outdir")
    ap.add_argument("--columns", type=int, default=8)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--anchor-count", type=int, default=16,
                    help="uniform anchor spacing to illustrate in the "
                         "anchor-damage sheet")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    out = pathlib.Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    for b in range(c.bands):
        views = c.band(b)
        box = local_box(views)
        stride = max(1, c.angles // args.columns)
        picks = [views[i * stride] for i in range(args.columns)]

        p = sheet(picks, box, args.scale).save(out / f"orbit-b{b}.png")
        print(f"wrote {p}")

        # One angular step, as the codec sees it: green appears, red vanishes.
        nxt = [views[(i * stride + 1) % c.angles] for i in range(args.columns)]
        p = sheet(nxt, box, args.scale, refs=picks).save(
            out / f"step-b{b}.png")
        print(f"wrote {p}")

        # Predicting from a sparse keyframe with no residual, shown at the
        # worst case rather than the average: each view sampled exactly half a
        # span away from its anchor, which is where a uniform layout is
        # weakest. This is what an error percentage actually looks like.
        span = max(1, c.angles // args.anchor_count)
        worst = [views[(i * stride + span // 2) % c.angles]
                 for i in range(args.columns)]
        anchors = [views[(round(v.angle / span) * span) % c.angles]
                   for v in worst]
        p = sheet(worst, box, args.scale, refs=anchors).save(
            out / f"anchor-b{b}.png")
        print(f"wrote {p} (uniform anchors={args.anchor_count}, "
              f"views half a span away)")


if __name__ == "__main__":
    main()
