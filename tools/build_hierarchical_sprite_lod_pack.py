#!/usr/bin/env python3
"""Generate a Game Gear runtime pack from the hierarchical sprite LOD solve.

Default physical sprite pattern layout assumes the SMS/GG sprite pattern table
at VRAM 0x2000 and a single background name table at 0x3800:

  sprite tile 160..175 -> VRAM 0x3400..0x35ff -> mid refinement (16 tiles)
  sprite tile 176..191 -> VRAM 0x3600..0x37ff -> far core       (16 tiles)
  background tile 0..415 remains available to the live room renderer
  name table begins at VRAM 0x3800

Thus R=36 can conceptually reserve only the top 16 tiles, while R=32 enables
another 16 immediately below.  The first proof bakes the room against the full
32-tile reservation so no allocator mutation is required at runtime.
"""

import argparse
import pathlib

from analyze_doomguy_dense_corpus import Corpus
from analyze_hierarchical_sprite_lod import learn, with_fixed_base
from analyze_sprite_resident_lod import build_groups
from resident_tile_dictionary import TileWeights, dedupe_patterns
from shared_resident_lod import score_groups


def planar_4bpp(pattern):
    if len(pattern) != 64:
        raise ValueError("semantic pattern must contain 64 pixels")
    out = bytearray()
    for y in range(8):
        row = pattern[y * 8:(y + 1) * 8]
        for plane in range(4):
            b = 0
            for x, value in enumerate(row):
                if value & (1 << plane):
                    b |= 0x80 >> x
            out.append(b)
    return bytes(out)


def c_u8_array(name, data, static=True):
    prefix = "static const" if static else "const"
    lines = [f"{prefix} uint8_t {name}[{max(1, len(data))}] = {{"]
    if not data:
        lines.append("    0,")
    else:
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(str(v) for v in data[i:i+16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def c_u16_array(name, vals):
    lines = [f"static const uint16_t {name}[{max(1, len(vals))}] = {{"]
    if not vals:
        lines.append("    0,")
    else:
        for i in range(0, len(vals), 12):
            lines.append("    " + ", ".join(str(v) for v in vals[i:i+12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def view_records(groups, score, core_count, wanted_band, staged=False):
    records = {}
    for gscore in score["groups"]:
        group = groups[gscore["group_index"]]
        base_count = gscore["base_count"]
        for demand, match in zip(group["demands"], gscore["score"]["matches"]):
            if demand["band"] != wanted_band:
                continue
            key = (demand["band"], demand["angle"])
            records.setdefault(key, [])
            di = match["dictionary_index"]
            if di == 0:
                continue
            if staged:
                if di < base_count:
                    logical = di - 1
                else:
                    logical = core_count + (di - base_count)
            else:
                # Empty base is index zero; shared core begins at one.
                logical = di - 1
            if logical < 0 or logical >= 256:
                raise ValueError("logical pattern index outside byte range")
            records[key].append((
                int(demand["x"]), int(demand["y"]), int(logical)))

    for key in records:
        records[key].sort(key=lambda r: (r[1], r[0], r[2]))
    return records


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("outdir")
    ap.add_argument("--angles", type=int, default=32)
    ap.add_argument("--far-band", type=int, default=3)
    ap.add_argument("--mid-band", type=int, default=2)
    ap.add_argument("--core-patterns", type=int, default=16)
    ap.add_argument("--refinement-patterns", type=int, default=16)
    ap.add_argument("--lloyd-iterations", type=int, default=4)
    ap.add_argument("--core-sprite-base", type=int, default=176)
    ap.add_argument("--refine-sprite-base", type=int, default=160)
    args = ap.parse_args()

    c = Corpus(args.corpus)
    if args.angles > c.angles:
        raise SystemExit("requested angles exceed corpus")
    angles = list(range(args.angles))
    weights = TileWeights(12.0, 1.0)

    far_groups, _ = build_groups(c, angles, [args.far_band])
    core_result = learn(
        far_groups, args.core_patterns, weights, args.lloyd_iterations)
    core = dedupe_patterns(core_result["shared"], modulo_flips=False)
    if len(core) != args.core_patterns:
        raise SystemExit(
            f"core deduped from {args.core_patterns} to {len(core)} patterns")
    far_score = score_groups(far_groups, core, weights, allow_flips=False)

    midfar_groups, _ = build_groups(
        c, angles, [args.mid_band, args.far_band])
    staged_groups = with_fixed_base(midfar_groups, core)
    extra_result = learn(
        staged_groups, args.refinement_patterns,
        weights, args.lloyd_iterations)
    extras = dedupe_patterns(extra_result["shared"], modulo_flips=False)
    if len(extras) != args.refinement_patterns:
        raise SystemExit(
            f"refinement deduped from {args.refinement_patterns} to {len(extras)} patterns")
    staged_score = score_groups(
        staged_groups, extras, weights, allow_flips=False)

    far_records = view_records(
        far_groups, far_score, len(core), args.far_band, staged=False)
    mid_records = view_records(
        staged_groups, staged_score, len(core), args.mid_band, staged=True)

    all_patterns = core + extras
    raw_patterns = b"".join(planar_4bpp(p) for p in all_patterns)

    # Logical view order: all far angles, then all mid angles. Runtime selects
    # the corresponding table; this keeps lookup arithmetic trivial on Z80.
    views = []
    for band, source in ((args.far_band, far_records),
                         (args.mid_band, mid_records)):
        for angle in angles:
            views.append((band, angle, source.get((band, angle), [])))

    offsets = [0]
    records = bytearray()
    max_sprites = 0
    for _, _, recs in views:
        max_sprites = max(max_sprites, len(recs))
        for x, y, logical in recs:
            if x < 0 or x > 255 or y < 0 or y > 255:
                raise SystemExit(f"sprite coordinate outside byte range: {x},{y}")
            records += bytes((x, y, logical))
        offsets.append(len(records))

    outdir = pathlib.Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    header = f"""#ifndef HERO_SPRITE_LOD_PACK_H
#define HERO_SPRITE_LOD_PACK_H

#include <stdint.h>

#define HERO_LOD_ANGLE_COUNT {len(angles)}u
#define HERO_LOD_FAR_BAND {args.far_band}u
#define HERO_LOD_MID_BAND {args.mid_band}u
#define HERO_LOD_CORE_COUNT {len(core)}u
#define HERO_LOD_REFINE_COUNT {len(extras)}u
#define HERO_LOD_CORE_SPRITE_BASE {args.core_sprite_base}u
#define HERO_LOD_REFINE_SPRITE_BASE {args.refine_sprite_base}u
#define HERO_LOD_MAX_SPRITES {max_sprites}u

void hero_lod_load_core(void) BANKED;
void hero_lod_load_refinement(void) BANKED;
void hero_lod_apply_view(uint8_t band,uint8_t angle) BANKED;
uint8_t hero_lod_view_sprite_count(uint8_t band,uint8_t angle) BANKED;

#endif
"""
    (outdir / "hero_sprite_lod_pack.h").write_text(header)

    source = f"""/* GENERATED hierarchical transparent hero LOD pack. */
#include <stdint.h>
#include <gbdk/platform.h>
#include "hero_sprite_lod_pack.h"

#pragma bank 255
BANKREF(hero_sprite_lod_pack)

extern uint8_t g_hero_lod_last_count;

{c_u8_array("k_patterns", raw_patterns)}
{c_u16_array("k_view_offsets", offsets)}
{c_u8_array("k_view_records", records)}

static uint8_t view_index(uint8_t band,uint8_t angle){{
    angle=(uint8_t)(angle%HERO_LOD_ANGLE_COUNT);
    if(band==HERO_LOD_FAR_BAND)return angle;
    return (uint8_t)(HERO_LOD_ANGLE_COUNT+angle);
}}

static uint8_t physical_tile(uint8_t logical){{
    if(logical<HERO_LOD_CORE_COUNT)
        return (uint8_t)(HERO_LOD_CORE_SPRITE_BASE+logical);
    return (uint8_t)(HERO_LOD_REFINE_SPRITE_BASE+
                     (logical-HERO_LOD_CORE_COUNT));
}}

void hero_lod_load_core(void) BANKED {{
    set_sprite_4bpp_data(HERO_LOD_CORE_SPRITE_BASE,HERO_LOD_CORE_COUNT,
                         k_patterns);
}}

void hero_lod_load_refinement(void) BANKED {{
    set_sprite_4bpp_data(HERO_LOD_REFINE_SPRITE_BASE,HERO_LOD_REFINE_COUNT,
                         k_patterns+(uint16_t)HERO_LOD_CORE_COUNT*32u);
}}

uint8_t hero_lod_view_sprite_count(uint8_t band,uint8_t angle) BANKED {{
    uint8_t vi=view_index(band,angle);
    uint16_t a=k_view_offsets[vi],b=k_view_offsets[(uint8_t)(vi+1u)];
    return (uint8_t)((b-a)/3u);
}}

void hero_lod_apply_view(uint8_t band,uint8_t angle) BANKED {{
    uint8_t vi=view_index(band,angle),i=0u;
    uint16_t p=k_view_offsets[vi],end=k_view_offsets[(uint8_t)(vi+1u)];
    while(p<end && i<64u){{
        uint8_t x=k_view_records[p++];
        uint8_t y=k_view_records[p++];
        uint8_t logical=k_view_records[p++];
        set_sprite_tile(i,physical_tile(logical));
        move_sprite(i,(uint8_t)(DEVICE_SPRITE_PX_OFFSET_X+x),
                      (uint8_t)(DEVICE_SPRITE_PX_OFFSET_Y+y));
        ++i;
    }}
    while(i<g_hero_lod_last_count)hide_sprite(i++);
    g_hero_lod_last_count=hero_lod_view_sprite_count(band,angle);
}}
"""
    (outdir / "hero_sprite_lod_pack.c").write_text(source)

    report = {
        "angles": len(angles),
        "core_patterns": len(core),
        "refinement_patterns": len(extras),
        "pattern_bytes": len(raw_patterns),
        "map_bytes": len(records) + len(offsets) * 2,
        "record_bytes": len(records),
        "offset_bytes": len(offsets) * 2,
        "max_sprites": max_sprites,
        "far_mean_cost": far_score["mean_cost"],
        "midfar_mean_cost": staged_score["mean_cost"],
        "core_sprite_base": args.core_sprite_base,
        "refine_sprite_base": args.refine_sprite_base,
        "room_background_tile_limit": args.refine_sprite_base + 256,
    }
    import json
    (outdir / "hero_sprite_lod_pack.json").write_text(
        json.dumps(report, sort_keys=True, indent=2) + "\n")
    print("HERO_SPRITE_LOD_PACK " + json.dumps(report, sort_keys=True))
    print("HERO_SPRITE_LOD_PACK_PASS")


if __name__ == "__main__":
    main()
