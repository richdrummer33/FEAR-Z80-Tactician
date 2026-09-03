#!/usr/bin/env python3
"""Read and inspect a DHC1 dense hero-view corpus.

The heavy predictor/anchor arithmetic lives in tools/hero_corpus_analyze.c --
it runs over every ordered pair in a band and would take minutes in Python.
This module is the readable half: a dependency-free reader, the structural
summary, and the eyes-on image dumps.

The corpus is object-only and anchor-normalized by construction: the camera
orbits the authored hero pivot on an exact circle and the angle count divides
256, so the aim yaw lands on a hardware yaw step and the pivot projects to
screen x=80.0 with a per-band constant y. That is why "normalize" below is a
plain integer translation and not a registration search.
"""

import argparse
import pathlib
import struct

HEADER_STRUCT = "<4sHBBHBB4i H 8I"
RECORD_HEAD = "<HBBiihhBBBBH"
RECORD_HEAD_SIZE = struct.calcsize(RECORD_HEAD)

# Same preview ramp the host compositor writes, so a corpus dump and a bake
# capture of the same view are directly comparable by eye.
SHADE_RGB = [
    (255, 0, 255),   # 0: not the hero
    (0, 0, 0), (16, 16, 48), (64, 64, 96), (96, 112, 144),
    (144, 160, 192), (208, 224, 240), (120, 136, 168), (176, 192, 216),
]


class Sample:
    __slots__ = ("angle", "band", "yaw", "cam_x", "cam_y", "anchor_x",
                 "anchor_y", "x0", "y0", "x1", "y1", "pixels", "crop")

    @property
    def width(self):
        return self.x1 - self.x0 + 1

    @property
    def height(self):
        return self.y1 - self.y0 + 1

    def at(self, sx, sy):
        """Semantic value at a screen pixel: 0 outside, else 1+ramp index."""
        if not (self.x0 <= sx <= self.x1 and self.y0 <= sy <= self.y1):
            return 0
        return self.crop[(sy - self.y0) * self.width + (sx - self.x0)]

    def row_spans(self):
        """Leftmost/rightmost owned column per crop row, None when empty.

        This is the side information a per-row morph predictor would have to
        ship, so its size is a real codec cost and worth reporting.
        """
        out = []
        for r in range(self.height):
            row = self.crop[r * self.width:(r + 1) * self.width]
            lo = next((i for i, v in enumerate(row) if v), None)
            if lo is None:
                out.append(None)
                continue
            hi = len(row) - 1 - next(i for i, v in enumerate(reversed(row)) if v)
            out.append((self.x0 + lo, self.x0 + hi))
        return out


class Corpus:
    def __init__(self, path):
        data = pathlib.Path(path).read_bytes()
        size = struct.calcsize(HEADER_STRUCT)
        if len(data) < size:
            raise SystemExit(f"{path}: too short to be a DHC1 corpus")
        fields = struct.unpack_from(HEADER_STRUCT, data, 0)
        magic, version, self.screen_w, self.screen_h = fields[0:4]
        self.angles, self.bands, self.owner = fields[4:7]
        px, py, pz, ez = fields[7:11]
        self.focal = fields[11]
        radii = fields[12:20]
        if magic != b"DHC1":
            raise SystemExit(f"{path}: not a DHC1 corpus")
        if version != 1:
            raise SystemExit(f"{path}: unsupported corpus version {version}")
        if not self.angles or not 1 <= self.bands <= 8:
            raise SystemExit(f"{path}: corpus header out of range")
        self.pivot = (px / 256.0, py / 256.0, pz / 256.0)
        self.eye_z = ez / 256.0
        self.radii = [r / 256.0 for r in radii[:self.bands]]

        self.samples = []
        p = size
        for _ in range(self.angles * self.bands):
            if p + RECORD_HEAD_SIZE > len(data):
                raise SystemExit(f"{path}: corpus truncated in record header")
            s = Sample()
            (s.angle, s.band, s.yaw, cx, cy, ax, ay,
             s.x0, s.y0, s.x1, s.y1, s.pixels) = struct.unpack_from(
                RECORD_HEAD, data, p)
            p += RECORD_HEAD_SIZE
            s.cam_x, s.cam_y = cx / 256.0, cy / 256.0
            s.anchor_x, s.anchor_y = ax / 256.0, ay / 256.0
            if s.x1 < s.x0 or s.y1 < s.y0:
                raise SystemExit(f"{path}: empty bounding box in record")
            n = s.width * s.height
            if p + n > len(data):
                raise SystemExit(f"{path}: corpus truncated in crop payload")
            s.crop = data[p:p + n]
            p += n
            if sum(1 for v in s.crop if v) != s.pixels:
                raise SystemExit(f"{path}: crop disagrees with pixel count")
            self.samples.append(s)
        if p != len(data):
            raise SystemExit(f"{path}: {len(data) - p} trailing bytes")

    def band(self, index):
        return self.samples[index * self.angles:(index + 1) * self.angles]


def write_ppm(path, w, h, pixels):
    body = bytearray()
    for v in pixels:
        body += bytes(SHADE_RGB[v])
    pathlib.Path(path).write_bytes(
        b"P6\n%d %d\n255\n" % (w, h) + bytes(body))


def dump_normalized(corpus, sample, path, pad=4):
    """Write one sample in the shared local frame.

    Every dump from a band lands on the same grid, so flipping between two of
    them shows only what the figure did, with no camera motion mixed in.
    """
    ax, ay = round(sample.anchor_x), round(sample.anchor_y)
    x0, x1 = sample.x0 - pad, sample.x1 + pad
    y0, y1 = sample.y0 - pad, sample.y1 + pad
    w, h = x1 - x0 + 1, y1 - y0 + 1
    px = []
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            v = sample.at(x, y)
            # Mark the anchor cross so the normalization is visible by eye.
            if not v and (x == ax or y == ay):
                v = 2
            px.append(v)
    write_ppm(path, w, h, px)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--dump-dir")
    ap.add_argument("--dump-every", type=int, default=64)
    args = ap.parse_args()

    c = Corpus(args.corpus)
    print("DOOM_DENSE_CORPUS_READ v1")
    print(f"samples={len(c.samples)} angles={c.angles} bands={c.bands} "
          f"angular_step_deg={360.0 / c.angles:.4f} owner=0x{c.owner:02x}")
    print(f"pivot={c.pivot} eye_z={c.eye_z} focal={c.focal} radii={c.radii}")

    for b in range(c.bands):
        views = c.band(b)
        anchors_x = {round(v.anchor_x, 4) for v in views}
        anchors_y = {round(v.anchor_y, 4) for v in views}
        # The whole point of the capture geometry: within a band, placement is
        # a constant. If this ever splits, the corpus stopped being normalized
        # and every downstream error figure silently becomes a search result.
        if len(anchors_x) != 1 or len(anchors_y) != 1:
            raise SystemExit(f"band {b}: placement anchor is not constant")
        rows = [sum(1 for s in v.row_spans() if s) for v in views]
        holes = 0
        for v in views:
            for r, span in enumerate(v.row_spans()):
                if not span:
                    continue
                lo, hi = span
                row = v.crop[r * v.width:(r + 1) * v.width]
                holes += sum(1 for x in range(lo, hi + 1)
                             if not row[x - v.x0])
        px = [v.pixels for v in views]
        print(f"band={b} radius={c.radii[b]:.2f} "
              f"anchor=({anchors_x.pop():.3f},{anchors_y.pop():.3f}) "
              f"pixels={min(px)}..{max(px)} mean={sum(px) // len(px)} "
              f"occupied_rows={min(rows)}..{max(rows)} "
              f"row_side_bytes={2 * min(rows)}..{2 * max(rows)} "
              f"interior_holes_mean={holes / len(views):.1f}")

    if args.dump_dir:
        out = pathlib.Path(args.dump_dir)
        out.mkdir(parents=True, exist_ok=True)
        n = 0
        for s in c.samples:
            if args.dump_every and s.angle % args.dump_every:
                continue
            dump_normalized(
                c, s, out / f"norm-b{s.band}-a{s.angle:03d}.ppm")
            n += 1
        print(f"dumped_normalized_views={n} dir={out}")


if __name__ == "__main__":
    main()
