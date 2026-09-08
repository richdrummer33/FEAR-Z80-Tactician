#!/usr/bin/env python3
"""Re-render captured composite frames through a solved Game Gear palette.

The host compositor writes its review frames through one fixed 8-entry preview
ramp, and that ramp is a bijection, so a captured frame can be inverted back to
the exact semantic code per pixel and re-rendered through any palette without
re-running the bake. That is the whole trick here: colour on this machine is a
palette-index remapping, so proving what colour looks like does not require
re-baking anything, and any difference between two of these images is
guaranteed to be palette-only.

Hero and room are recoloured from DIFFERENT palettes. That is not a stylistic
choice, it is what the hardware does: the hero is drawn as sprites and reads
the 16-entry sprite palette while the room is background tiles reading the BG
palettes. A companion owner-mask PGM says which pixels are the hero.

Modes:
  grey    the palette currently shipped in src/main_hero_sprite_lod_flat_room_gg.c
  static  the solved colour palette, one setting, no temporal trickery
  ilv-a   frame A of the temporal interleave
  ilv-b   frame B
  blend   what the eye actually integrates from A and B, computed in LINEAR
          light. This is the honest "what you would see" image; ilv-a and
          ilv-b are only useful for judging how bad the flicker would be.
"""

import argparse
import json
import pathlib
import struct
import sys

# tools/polar_baked_composite.c, tsp_host_composite_write_ppm().
PREVIEW_RAMP = [
    (0, 0, 0), (16, 16, 48), (64, 64, 96), (96, 112, 144),
    (144, 160, 192), (208, 224, 240), (120, 136, 168), (176, 192, 216),
]
PREVIEW_INDEX = {rgb: i for i, rgb in enumerate(PREVIEW_RAMP)}

# src/main_hero_sprite_lod_flat_room_gg.c, k_bg_palettes / k_sprite_palette.
SHIPPED_BG = [(0, 0, 0), (1, 1, 3), (2, 2, 3), (3, 4, 6), (6, 7, 9),
              (10, 11, 13), (4, 5, 7), (8, 9, 11)]
SHIPPED_SPRITE = SHIPPED_BG


def gg_to_srgb8(c):
    return tuple(round(v * 255 / 15) for v in c)


def srgb_to_linear(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb8(v):
    v = max(0.0, min(1.0, v))
    s = v * 12.92 if v <= 0.0031308 else 1.055 * (v ** (1 / 2.4)) - 0.055
    return int(round(s * 255))


def read_ppm(path):
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{path}: not a binary PPM")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, _ = fields
    return w, h, data[pos:pos + w * h * 3]


def read_pgm(path):
    data = path.read_bytes()
    if not data.startswith(b"P5"):
        raise SystemExit(f"{path}: not a binary PGM")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, _ = fields
    return w, h, data[pos:pos + w * h]


def to_semantic(w, h, rgb):
    """Invert the preview ramp. Any colour not in the table is a bug, not a
    pixel to guess at, so this raises rather than picking a nearest match."""
    out = bytearray(w * h)
    for i in range(w * h):
        px = (rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2])
        try:
            out[i] = PREVIEW_INDEX[px]
        except KeyError:
            raise SystemExit(
                f"frame contains {px}, which is not a preview-ramp colour; "
                "the capture was not written by tsp_host_composite_write_ppm")
    return out


def hero_index(design, family, semantic):
    """Palette index a hero pixel resolves to.

    mono-ramp leaves the compositor's semantic value alone, so the index IS the
    semantic. family-split repacks it as 1 + family*5 + position on the
    brightness ramp, because three ramps do not fit in eight semantic values.
    The design JSON carries the table so this is a lookup, not a re-derivation.
    """
    # design is None for the greyscale reference: that palette is indexed by
    # the compositor's semantic value directly, and running it through the
    # family repacking would send hero pixels to entries the shipped table
    # never populated (which reads as large black holes in the figure -- how
    # this was caught).
    table = design.get("spriteIndex") if design else None
    if not table:
        return semantic
    order = design["brightnessOrder"]
    if semantic not in order:
        return 0
    return table[min(family, len(table) - 1)][order.index(semantic)]


def palette_tables(design, mode):
    """Return (bg16, sprite16) as sRGB8 tuples, or (bg, sprite) pairs for blend."""
    if mode == "grey":
        bg = [gg_to_srgb8(c) for c in SHIPPED_BG] + [(0, 0, 0)] * 8
        return bg, list(bg)
    key = {"static": "static", "ilv-a": "interleaveA", "ilv-b": "interleaveB"}[mode]
    # BG palette 0 only: the compositor has already resolved lit pixels into a
    # brighter semantic before writing the frame, so the runtime's palette-1
    # brightening must not be applied a second time here.
    bg = [gg_to_srgb8(c) for c in design["bg"][key][0]]
    sprite = [gg_to_srgb8(c) for c in design["sprite"][key]]
    return bg, sprite


def blend_tables(design):
    def mix(a, b):
        return tuple(linear_to_srgb8((srgb_to_linear(gg_to_srgb8(a)[k])
                                      + srgb_to_linear(gg_to_srgb8(b)[k])) / 2)
                     for k in range(3))
    bg = [mix(a, b) for a, b in zip(design["bg"]["interleaveA"][0],
                                    design["bg"]["interleaveB"][0])]
    sprite = [mix(a, b) for a, b in zip(design["sprite"]["interleaveA"],
                                        design["sprite"]["interleaveB"])]
    return bg, sprite


def oklab_L(c):
    """Oklab lightness of an sRGB8 triple. Only L is needed here: the claim
    being checked is that colour left the LIGHTNESS structure alone."""
    r, g, b = (srgb_to_linear(v) for v in c)
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    return (0.2104542553 * l ** (1 / 3) + 0.7936177850 * m ** (1 / 3)
            - 0.0040720468 * s ** (1 / 3))


def lightness_delta(ref_bg, ref_sprite, bg, sprite, counts, design):
    """Weighted mean and max |dL| between two palettes over the pixels that
    actually occurred, per semantic and per hero/room role.

    This is the check that matters. Depth and shade in this renderer are
    carried entirely by the shade ramp, and the tile vocabulary was trained
    against a lightness-ordered objective, so if the coloured palette puts
    every semantic at the lightness the greyscale one did, the coloured image
    cannot read as flatter -- it is the same picture with hue added. Anything
    else is a taste argument about saturation, which is a separate knob."""
    total = sum(counts.values()) or 1
    acc, worst = 0.0, (0.0, None)
    for (role, f, v), n in counts.items():
        a = ref_sprite[v] if role else ref_bg[v]
        b = sprite[hero_index(design, f, v)] if role else bg[v]
        d = abs(oklab_L(a) - oklab_L(b))
        acc += d * n
        if d > worst[0]:
            worst = (d, (role, f, v))
    return acc / total, worst


def render(sem, mask, family, w, h, bg, sprite, design):
    out = bytearray(w * h * 3)
    lut = {}
    for i in range(w * h):
        v = sem[i]
        if mask and mask[i]:
            f = (family[i] - 1) if family else 0
            if f < 0:
                f = 0
            key = (f, v)
            idx = lut.get(key)
            if idx is None:
                idx = lut[key] = hero_index(design, f, v)
            c = sprite[idx]
        else:
            c = bg[v]
        out[i * 3:i * 3 + 3] = bytes(c)
    return bytes(out)


def save_png(path, w, h, rgb, scale=1):
    from PIL import Image
    im = Image.frombytes("RGB", (w, h), rgb)
    if scale != 1:
        im = im.resize((w * scale, h * scale), Image.NEAREST)
    im.save(path)


def contact_sheet(path, images, w, h, cols, scale):
    from PIL import Image
    tiles = [Image.frombytes("RGB", (w, h), im).resize(
        (w * scale, h * scale), Image.NEAREST) for im in images]
    rows = (len(tiles) + cols - 1) // cols
    tw, th = tiles[0].size
    sheet = Image.new("RGB", (tw * cols, th * rows), (24, 24, 28))
    for k, t in enumerate(tiles):
        sheet.paste(t, ((k % cols) * tw, (k // cols) * th))
    sheet.save(path)


def palette_card(path, design, scale=48):
    """A swatch strip of everything the design puts on screen, static over
    interleaved-blend, so the two are judged side by side rather than from
    numbers alone."""
    from PIL import Image, ImageDraw
    bg_s, sp_s = palette_tables(design, "static")
    bg_b, sp_b = blend_tables(design)
    order = design["brightnessOrder"]
    sem = design["semantics"]
    fams = len(design.get("spriteIndex") or [1])
    rows = []
    for f in range(fams):
        idx = [hero_index(design, f, v) for v in order]
        label = f"hero fam{f}" if fams > 1 else "hero"
        rows.append((label + " static", [sp_s[i] for i in idx]))
        rows.append((label + " blend", [sp_b[i] for i in idx]))
    rows.append(("room static", [bg_s[sem["CEILING"]], bg_s[sem["FLOOR"]]] + [bg_s[v] for v in order]))
    rows.append(("room blend", [bg_b[sem["CEILING"]], bg_b[sem["FLOOR"]]] + [bg_b[v] for v in order]))
    cols = max(len(r[1]) for r in rows)
    label_w = 150
    im = Image.new("RGB", (label_w + cols * scale, len(rows) * scale), (18, 18, 22))
    d = ImageDraw.Draw(im)
    for r, (name, colours) in enumerate(rows):
        d.text((8, r * scale + scale // 2 - 6), name, fill=(220, 220, 225))
        for c, col in enumerate(colours):
            d.rectangle([label_w + c * scale, r * scale,
                         label_w + (c + 1) * scale - 1, (r + 1) * scale - 1], fill=col)
    im.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("design", type=pathlib.Path, help="gg_palette_design.mjs JSON")
    ap.add_argument("frames", type=pathlib.Path, help="directory of review-*.ppm")
    ap.add_argument("outdir", type=pathlib.Path)
    ap.add_argument("--tag", default="doomguy-detail")
    ap.add_argument("--modes", default="grey,static,blend,ilv-a,ilv-b")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--sheet-every", type=int, default=15)
    ap.add_argument("--sheet-cols", type=int, default=4)
    args = ap.parse_args()

    design = json.loads(args.design.read_text())
    args.outdir.mkdir(parents=True, exist_ok=True)
    palette_card(args.outdir / "palette-card.png", design)

    frames = sorted(args.frames.glob(f"review-{args.tag}-*.ppm"))
    if not frames:
        raise SystemExit(f"no review-{args.tag}-*.ppm under {args.frames}")

    modes = [m for m in args.modes.split(",") if m]
    sheets = {m: [] for m in modes}
    counts = {}
    n_masked = 0
    n_family = 0
    for f in frames:
        w, h, rgb = read_ppm(f)
        sem = to_semantic(w, h, rgb)
        idx = f.stem.rsplit("-", 1)[1]
        maskp = f.parent / f"mask-{args.tag}-{idx}.pgm"
        mask = None
        if maskp.exists():
            mw, mh, m = read_pgm(maskp)
            if (mw, mh) != (w, h):
                raise SystemExit(f"{maskp}: size mismatch with {f}")
            mask = m
            n_masked += 1
        # Material family per pixel, written as family+1 so 0 means "not the
        # hero". Absent for a mono-ramp asset, where every hero pixel is the
        # one material and family 0 is the only answer.
        famp = f.parent / f"family-{args.tag}-{idx}.pgm"
        family = None
        if famp.exists():
            fw, fh, fm = read_pgm(famp)
            if (fw, fh) != (w, h):
                raise SystemExit(f"{famp}: size mismatch with {f}")
            family = fm
            n_family += 1
        for i in range(w * h):
            hero = 1 if (mask and mask[i]) else 0
            f = (family[i] - 1) if (hero and family and family[i]) else 0
            key = (hero, f, sem[i])
            counts[key] = counts.get(key, 0) + 1
        for mode in modes:
            bg, sprite = blend_tables(design) if mode == "blend" \
                else palette_tables(design, mode)
            img = render(sem, mask, family, w, h, bg, sprite,
                         None if mode == "grey" else design)
            d = args.outdir / mode
            d.mkdir(exist_ok=True)
            save_png(d / f"{idx}.png", w, h, img, args.scale)
            if int(idx) % args.sheet_every == 0:
                sheets[mode].append(img)

    for mode in modes:
        if sheets[mode]:
            contact_sheet(args.outdir / f"sheet-{mode}.png", sheets[mode],
                          w, h, args.sheet_cols, 2)

    ref_bg, ref_sprite = palette_tables(design, "grey")
    for mode in modes:
        if mode == "grey":
            continue
        bg, sprite = blend_tables(design) if mode == "blend" \
            else palette_tables(design, mode)
        mean, (worst, where) = lightness_delta(ref_bg, ref_sprite, bg, sprite,
                                               counts, design)
        role = "hero" if where and where[0] else "room"
        at = f"{role}/fam{where[1]}/sem{where[2]}" if where else "-"
        print(f"LIGHTNESS_DELTA mode={mode} mean_abs_dL={mean:.4f} "
              f"max_abs_dL={worst:.4f} at={at}")

    print(f"RECOLOUR_PASS frames={len(frames)} masked={n_masked} "
          f"family={n_family} modes={','.join(modes)} out={args.outdir}")
    if n_masked != len(frames):
        print(f"note: {len(frames) - n_masked} frames had no owner mask; the hero "
              f"in those was drawn from the BG palette (set ROOM_BUNDLE_CAPTURE_OWNER)",
              file=sys.stderr)


if __name__ == "__main__":
    main()
