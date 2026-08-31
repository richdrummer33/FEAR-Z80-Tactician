#!/usr/bin/env python3
import argparse
import pathlib
import struct

BANK_STREAM_MAX = 12000
MAX_BANK_FILES = 224

def u16(b, p):
    return struct.unpack_from("<H", b, p)[0]

def u32(b, p):
    return struct.unpack_from("<I", b, p)[0]

def parse_pack(path):
    data = pathlib.Path(path).read_bytes()
    if data[:4] != b"RBP2":
        raise SystemExit("bad room bundle pack magic")
    version = u16(data, 4)
    if version != 2:
        raise SystemExit(f"unsupported room bundle pack version {version}")
    bundle_count = data[6]
    p = 8
    bundles = []
    for _ in range(bundle_count):
        if p + 4 > len(data):
            raise SystemExit("truncated room bundle header")
        bundle_id = data[p]
        route_count = data[p+1]
        p += 4  # id, route count, reserved u16
        routes = []
        for _r in range(route_count):
            if p + 12 > len(data):
                raise SystemExit("truncated room route header")
            entry = data[p]
            exit_portal = data[p+1]
            frame_count = u16(data, p+2)
            patch_bytes = u32(data, p+4)
            tile_bytes = u32(data, p+8)
            p += 12
            frames = []
            for _f in range(frame_count):
                if p + 4 > len(data):
                    raise SystemExit("truncated room route frame header")
                plen = u16(data, p)
                tlen = u16(data, p+2)
                p += 4
                patch = data[p:p+plen]
                p += plen
                tile = data[p:p+tlen]
                p += tlen
                if len(patch) != plen or len(tile) != tlen:
                    raise SystemExit("truncated room route frame")
                frames.append((patch, tile))
            if sum(len(x[0]) for x in frames) != patch_bytes:
                raise SystemExit(f"bundle {bundle_id} route {entry}->{exit_portal} patch byte count mismatch")
            if sum(len(x[1]) for x in frames) != tile_bytes:
                raise SystemExit(f"bundle {bundle_id} route {entry}->{exit_portal} tile byte count mismatch")
            routes.append((entry, exit_portal, frames))
        bundles.append((bundle_id, routes))
    if p != len(data):
        raise SystemExit(f"room bundle pack has {len(data)-p} trailing bytes")
    return bundles

def c_u8_array(name, data):
    lines = [f"static const uint8_t {name}[{max(1,len(data))}] = {{"]
    if not data:
        lines.append("    0,")
    else:
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(str(v) for v in data[i:i+16]) + ",")
    lines.append("};")
    return "\n".join(lines)

def c_u16_array(name, vals):
    lines = [f"static const uint16_t {name}[{max(1,len(vals))}] = {{"]
    if not vals:
        lines.append("    0,")
    else:
        for i in range(0, len(vals), 12):
            lines.append("    " + ", ".join(str(v) for v in vals[i:i+12]) + ",")
    lines.append("};")
    return "\n".join(lines)

def make_chunks(global_frames):
    chunks = []
    first = 0
    while first < len(global_frames):
        frames = []
        size = 0
        i = first
        while i < len(global_frames):
            patch, tile = global_frames[i]
            entry_size = 4 + len(patch) + len(tile)
            if frames and size + entry_size > BANK_STREAM_MAX:
                break
            if entry_size > BANK_STREAM_MAX:
                raise SystemExit(f"single frame {i} exceeds bank stream max")
            frames.append((patch, tile))
            size += entry_size
            i += 1
        chunks.append((first, frames))
        first = i
    if len(chunks) > MAX_BANK_FILES:
        raise SystemExit("room bundle pack exceeds generated bank file budget")
    return chunks

def emit_bank(outdir, bank_index, first, frames):
    offsets = [0]
    raw = bytearray()
    for patch, tile in frames:
        raw += struct.pack("<HH", len(patch), len(tile))
        raw += patch
        raw += tile
        offsets.append(len(raw))

    s = f"""/* GENERATED room-bundle PoC data bank {bank_index}. */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"

#pragma bank 255
BANKREF(room_bundle_poc_data_bank{bank_index})

extern uint16_t g_map[TSP_MAP_CELLS];
extern uint8_t g_polar_nt_row_min[TSP_ROWS];
extern uint8_t g_polar_nt_row_max[TSP_ROWS];

{c_u16_array("k_off", offsets)}

{c_u8_array("k_data", raw)}

#define FRAMES_IN_BANK {len(frames)}u

static const uint8_t *frame_ptr(uint16_t local) {{
    if(local>=FRAMES_IN_BANK)return (const uint8_t *)0;
    return &k_data[k_off[local]];
}}

void tsp_room_bundle_name_bank{bank_index}(uint16_t local) BANKED {{
    const uint8_t *p=frame_ptr(local);
    uint16_t patch_len,tile_len,n,i;
    if(!p)return;
    patch_len=(uint16_t)p[0]|((uint16_t)p[1]<<8);
    tile_len=(uint16_t)p[2]|((uint16_t)p[3]<<8);
    (void)tile_len;
    p+=4;
    if(patch_len<2u)return;
    n=(uint16_t)*p++;n|=(uint16_t)*p++<<8;
    for(i=0u;i<n;++i){{
        uint8_t row=*p++,x=*p++,count=*p++,c;
        uint16_t idx=(uint16_t)row*TSP_COLS+x;
        uint8_t last=(uint8_t)(x+count-1u);
        if(g_polar_nt_row_min[row]==0xffu||x<g_polar_nt_row_min[row])
            g_polar_nt_row_min[row]=x;
        if(last>g_polar_nt_row_max[row])g_polar_nt_row_max[row]=last;
        for(c=0u;c<count;++c){{
            uint16_t w=(uint16_t)*p++;w|=(uint16_t)*p++<<8;
            g_map[idx++]=w;
        }}
    }}
}}

void tsp_room_bundle_tile_bank{bank_index}(uint16_t local) BANKED {{
    const uint8_t *p=frame_ptr(local);
    uint16_t patch_len,tile_len,n,i;
    if(!p)return;
    patch_len=(uint16_t)p[0]|((uint16_t)p[1]<<8);
    tile_len=(uint16_t)p[2]|((uint16_t)p[3]<<8);
    p+=4u+patch_len;
    if(tile_len<2u)return;
    n=(uint16_t)*p++;n|=(uint16_t)*p++<<8;
    for(i=0u;i<n;++i){{
        uint16_t slot=(uint16_t)*p++;slot|=(uint16_t)*p++<<8;
        set_bkg_4bpp_data(slot,1u,p);
        p+=32u;
    }}
}}
"""
    (outdir / f"room_bundle_poc_data_bank{bank_index}.c").write_text(s)

def emit_dispatch(outdir, bundles, chunks, canonical):
    bundle_ids = [b[0] for b in bundles]
    if bundle_ids != list(range(len(bundle_ids))):
        raise SystemExit("PoC runtime requires dense bundle ids from zero")

    route_bundle = []
    route_entry = []
    route_exit = []
    route_first = []
    route_count = []
    pos = 0
    for bundle_id, routes in bundles:
        for entry, exit_portal, frames in routes:
            route_bundle.append(bundle_id)
            route_entry.append(entry)
            route_exit.append(exit_portal)
            route_first.append(pos)
            route_count.append(len(frames))
            pos += len(frames)

    words = list(struct.unpack("<" + "H"*(len(canonical)//2), canonical))
    if len(words) != 360:
        raise SystemExit("canonical seam map is not 360 words")

    decl = []
    for i in range(len(chunks)):
        decl.append(f"void tsp_room_bundle_name_bank{i}(uint16_t local) BANKED;")
        decl.append(f"void tsp_room_bundle_tile_bank{i}(uint16_t local) BANKED;")

    dispatch_cases_name = []
    dispatch_cases_tile = []
    for i,(first, frames) in enumerate(chunks):
        end = first + len(frames)
        kw = "if" if i == 0 else "else if"
        dispatch_cases_name.append(
            f"    {kw}(global<{end}u){{tsp_room_bundle_name_bank{i}((uint16_t)(global-{first}u));return;}}")
        dispatch_cases_tile.append(
            f"    {kw}(global<{end}u){{tsp_room_bundle_tile_bank{i}((uint16_t)(global-{first}u));return;}}")

    s = f"""/* GENERATED room-bundle PoC dispatcher + canonical seam. */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"

{chr(10).join(decl)}

extern uint16_t g_map[TSP_MAP_CELLS];
extern uint8_t g_polar_nt_row_min[TSP_ROWS];
extern uint8_t g_polar_nt_row_max[TSP_ROWS];

{c_u8_array("k_route_bundle", route_bundle)}
{c_u8_array("k_route_entry", route_entry)}
{c_u8_array("k_route_exit", route_exit)}
{c_u16_array("k_route_first", route_first)}
{c_u16_array("k_route_count", route_count)}
{c_u16_array("k_canonical_map", words)}

#define ROOM_BUNDLE_COUNT {len(bundles)}u
#define ROOM_ROUTE_COUNT {len(route_bundle)}u

uint8_t tsp_room_bundle_generated_count(void){{return ROOM_BUNDLE_COUNT;}}
uint8_t tsp_room_bundle_generated_route_count(void){{return ROOM_ROUTE_COUNT;}}

static int16_t find_route(uint8_t bundle,uint8_t entry,uint8_t exit_portal){{
    uint8_t i;
    for(i=0u;i<ROOM_ROUTE_COUNT;++i)
        if(k_route_bundle[i]==bundle &&
           k_route_entry[i]==entry &&
           k_route_exit[i]==exit_portal)
            return (int16_t)i;
    return -1;
}}

uint16_t tsp_room_bundle_generated_frames(uint8_t bundle,uint8_t entry,uint8_t exit_portal){{
    int16_t ri=find_route(bundle,entry,exit_portal);
    if(ri<0)return 0u;
    return k_route_count[(uint8_t)ri];
}}

static uint16_t global_frame(uint8_t bundle,uint8_t entry,uint8_t exit_portal,uint16_t frame){{
    int16_t ri=find_route(bundle,entry,exit_portal);
    uint8_t i;
    if(ri<0)return 0xffffu;
    i=(uint8_t)ri;
    if(frame>=k_route_count[i])return 0xffffu;
    return (uint16_t)(k_route_first[i]+frame);
}}

void tsp_room_bundle_generated_apply_name(uint8_t bundle,uint8_t entry,uint8_t exit_portal,uint16_t frame){{
    uint16_t global=global_frame(bundle,entry,exit_portal,frame);
    if(global==0xffffu)return;
{chr(10).join(dispatch_cases_name)}
}}

void tsp_room_bundle_generated_apply_tile(uint8_t bundle,uint8_t entry,uint8_t exit_portal,uint16_t frame){{
    uint16_t global=global_frame(bundle,entry,exit_portal,frame);
    if(global==0xffffu)return;
{chr(10).join(dispatch_cases_tile)}
}}

void tsp_room_bundle_generated_load_canonical(void){{
    uint16_t i;
    uint8_t row;
    for(i=0u;i<TSP_MAP_CELLS;++i)g_map[i]=k_canonical_map[i];
    for(row=0u;row<TSP_ROWS;++row){{
        g_polar_nt_row_min[row]=0u;
        g_polar_nt_row_max[row]=TSP_COLS-1u;
    }}
}}
"""
    (outdir / "room_bundle_poc_dispatch.c").write_text(s)

    h = f"""#ifndef ROOM_BUNDLE_POC_META_H
#define ROOM_BUNDLE_POC_META_H
#include <stdint.h>
#define ROOM_BUNDLE_POC_COUNT {len(bundles)}u
#define ROOM_BUNDLE_POC_ROUTE_COUNT {len(route_bundle)}u
uint8_t tsp_room_bundle_generated_count(void);
uint8_t tsp_room_bundle_generated_route_count(void);
uint16_t tsp_room_bundle_generated_frames(uint8_t bundle,uint8_t entry,uint8_t exit_portal);
void tsp_room_bundle_generated_apply_name(uint8_t bundle,uint8_t entry,uint8_t exit_portal,uint16_t frame);
void tsp_room_bundle_generated_apply_tile(uint8_t bundle,uint8_t entry,uint8_t exit_portal,uint16_t frame);
void tsp_room_bundle_generated_load_canonical(void);
#endif
"""
    (outdir / "room_bundle_poc_meta.h").write_text(h)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("canonical")
    ap.add_argument("outdir")
    args = ap.parse_args()

    outdir = pathlib.Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    bundles = parse_pack(args.pack)
    canonical = pathlib.Path(args.canonical).read_bytes()

    global_frames = []
    for _, routes in bundles:
        for _entry, _exit, frames in routes:
            global_frames.extend(frames)
    chunks = make_chunks(global_frames)

    for i,(first,frames) in enumerate(chunks):
        emit_bank(outdir, i, first, frames)
    emit_dispatch(outdir, bundles, chunks, canonical)

    total = sum(4 + len(p) + len(t) for p,t in global_frames)
    route_total = sum(len(routes) for _, routes in bundles)
    print(f"ROOM_BUNDLE_C_GEN_PASS bundles={len(bundles)} routes={route_total} "
          f"frames={len(global_frames)} banks={len(chunks)} stream_bytes={total}")

if __name__ == "__main__":
    main()
