#!/usr/bin/env python3
"""Measure the quality-filtered aimed Doomguy orbit used by the codec lab.

This is deliberately an analyzer, not a decoder. It answers three questions
before adaptive anchors are chosen:
  * how irregularly the legal camera ring samples object-relative angle;
  * how much the hero silhouette changes between neighboring aimed views;
  * how much of each view is still pinned to the brightest semantic stop.

The input frames are the exact owner-isolated captures produced by the playable
bake, so room pixels cannot contaminate the measurements.
"""

import argparse
import math
import pathlib

from doomguy_playable_pack_to_c import build_orbit_entries, parse_pack

MAGENTA = (255, 0, 255)
TOP_SHADE = (208, 224, 240)


def read_netpbm(path, channels):
    data = pathlib.Path(path).read_bytes()
    p = 0
    toks = []
    while len(toks) < 4:
        while p < len(data) and data[p] in b" \t\r\n":
            p += 1
        if p < len(data) and data[p] == 35:  # '#'
            while p < len(data) and data[p] != 10:
                p += 1
            continue
        q = p
        while q < len(data) and data[q] not in b" \t\r\n":
            q += 1
        toks.append(data[p:q].decode("ascii"))
        p = q
    magic, sw, sh, smax = toks
    want_magic = "P5" if channels == 1 else "P6"
    if magic != want_magic or int(smax) != 255:
        raise SystemExit(f"unexpected Netpbm header in {path}: {toks}")
    while p < len(data) and data[p] in b" \t\r\n":
        p += 1
    w, h = int(sw), int(sh)
    body = data[p:]
    if len(body) != w * h * channels:
        raise SystemExit(f"truncated Netpbm payload in {path}")
    return w, h, body


def shifted_index(x, y, dx, w, h):
    sx = x - dx
    if 0 <= sx < w:
        return y * w + sx
    return None


def silhouette_error(a, b, w, h, dx):
    xor = 0
    union = 0
    for y in range(h):
        for x in range(w):
            ia = y * w + x
            ib = shifted_index(x, y, dx, w, h)
            ma = a[ia] != 0
            mb = ib is not None and b[ib] != 0
            union += ma or mb
            xor += ma != mb
    return xor, union


def tone_error(rgb_a, rgb_b, mask_a, mask_b, w, h, dx):
    different = 0
    overlap = 0
    abs_sum = 0
    for y in range(h):
        for x in range(w):
            ia = y * w + x
            ib = shifted_index(x, y, dx, w, h)
            if ib is None or not mask_a[ia] or not mask_b[ib]:
                continue
            overlap += 1
            oa, ob = ia * 3, ib * 3
            pa = rgb_a[oa:oa+3]
            pb = rgb_b[ob:ob+3]
            if pa != pb:
                different += 1
            abs_sum += sum(abs(int(pa[k]) - int(pb[k])) for k in range(3))
    mean_abs = abs_sum / (overlap * 3.0) if overlap else 255.0
    return different, overlap, mean_abs


def pct(n, d):
    return 100.0 * n / d if d else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("capture_dir")
    ap.add_argument("--max-shift", type=int, default=6)
    args = ap.parse_args()

    meta, lut, _dictionary, _states = parse_pack(args.pack)
    packed_orbit = build_orbit_entries(meta, lut)
    orbit = [(angle, ordinal, x, y)
             for angle, _radius, ordinal, _state, x, y in packed_orbit]
    cap = pathlib.Path(args.capture_dir)

    frames = {}
    top_fracs = []
    for _ang, ordinal, _x, _y in orbit:
        mw, mh, mask_bytes = read_netpbm(
            cap / f"playable-mask-{ordinal:03d}.pgm", 1)
        pw, ph, rgb = read_netpbm(
            cap / f"playable-eval-{ordinal:03d}.ppm", 3)
        if (mw, mh) != (pw, ph):
            raise SystemExit("mask/eval dimensions differ")
        mask = bytes(1 if v else 0 for v in mask_bytes)
        owned = sum(mask)
        top = 0
        for i, m in enumerate(mask):
            if not m:
                continue
            p = tuple(rgb[i*3:i*3+3])
            if p == TOP_SHADE:
                top += 1
            if p == MAGENTA:
                raise SystemExit("owner mask includes diagnostic background")
        frames[ordinal] = (mw, mh, mask, rgb)
        top_fracs.append((pct(top, owned), ordinal, top, owned))

    pairs = []
    n = len(orbit)
    for i in range(n):
        a = orbit[i]
        b = orbit[(i + 1) % n]
        aa, ao = a[0], a[1]
        ba, bo = b[0], b[1]
        gap = ba - aa
        if i + 1 == n:
            gap += 2.0 * math.pi
        w, h, ma, ra = frames[ao]
        _w, _h, mb, rb = frames[bo]

        raw_xor, raw_union = silhouette_error(ma, mb, w, h, 0)
        best = None
        for dx in range(-args.max_shift, args.max_shift + 1):
            x, u = silhouette_error(ma, mb, w, h, dx)
            key = (pct(x, u), abs(dx), dx)
            if best is None or key < best[0]:
                best = (key, dx, x, u)
        _key, dx, x, u = best
        diff, overlap, mean_abs = tone_error(ra, rb, ma, mb, w, h, dx)
        pairs.append({
            "a": ao, "b": bo, "gap_deg": math.degrees(gap),
            "raw_mask_pct": pct(raw_xor, raw_union),
            "best_dx": dx, "mask_pct": pct(x, u),
            "tone_diff_pct": pct(diff, overlap),
            "tone_mean_abs": mean_abs,
        })

    gaps = [p["gap_deg"] for p in pairs]
    mask_err = [p["mask_pct"] for p in pairs]
    tone_err = [p["tone_diff_pct"] for p in pairs]

    print("DOOM_ORBIT_CORPUS v1")
    print(
        f"views={n} angle_gap_deg_min={min(gaps):.3f} "
        f"angle_gap_deg_mean={sum(gaps)/n:.3f} angle_gap_deg_max={max(gaps):.3f}"
    )
    print(
        f"adjacent_best_xshift_mask_error_pct_mean={sum(mask_err)/n:.3f} "
        f"max={max(mask_err):.3f} max_shift={args.max_shift}"
    )
    print(
        f"adjacent_tone_change_pct_mean={sum(tone_err)/n:.3f} "
        f"max={max(tone_err):.3f}"
    )
    tmean = sum(q[0] for q in top_fracs) / n
    tmax = max(top_fracs)
    tmin = min(top_fracs)
    print(
        f"top_shade_pct_min={tmin[0]:.3f} mean={tmean:.3f} "
        f"max={tmax[0]:.3f} max_ordinal={tmax[1]}"
    )

    print("hardest_silhouette_pairs")
    for p in sorted(pairs, key=lambda q: q["mask_pct"], reverse=True)[:8]:
        print(
            f"pair={p['a']:03d}->{p['b']:03d} gap_deg={p['gap_deg']:.3f} "
            f"best_dx={p['best_dx']:+d} mask_pct={p['mask_pct']:.3f} "
            f"raw_mask_pct={p['raw_mask_pct']:.3f} "
            f"tone_pct={p['tone_diff_pct']:.3f} "
            f"tone_mean_abs={p['tone_mean_abs']:.3f}"
        )

    # These are diagnostics, not pass/fail quality thresholds. They show how
    # many neighbor transitions are already close enough that a future anchor
    # codec can plausibly predict them with only a tiny silhouette residual.
    for threshold in (2.0, 5.0, 10.0):
        k = sum(1 for p in pairs if p["mask_pct"] <= threshold)
        print(
            f"adjacent_mask_within_{threshold:.0f}pct={k}/{n} "
            f"pct={pct(k,n):.1f}"
        )


if __name__ == "__main__":
    main()
