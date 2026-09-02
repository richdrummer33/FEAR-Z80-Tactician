#!/usr/bin/env python3
"""
Pack the random-access Doomguy inspection lattice into autobanked GBDK C.

Every pose owns an absolute 20x18 name table plus one contiguous block of
ordinary 32-byte patterns. Runtime alternates two disjoint VRAM pools. The
resident hero dictionary is loaded once at boot and is shared by both pools.
"""
import argparse
import math
import pathlib
import struct

BANK_STREAM_MAX = 12000
MAX_BANK_FILES = 224
MAP_CELLS = 360
MAP_BYTES = MAP_CELLS * 2
PATTERN_BYTES = 32
TILE_ID_MASK = 0x01FF
DUAL_NAME_TABLE_TILE_LIMIT = 384

def u16(b, p):
    return struct.unpack_from("<H", b, p)[0]

def s16(b, p):
    return struct.unpack_from("<h", b, p)[0]

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

def parse_pack(path):
    data = pathlib.Path(path).read_bytes()
    if data[:4] != b"DGP1":
        raise SystemExit("bad Doomguy playable pack magic")
    if u16(data, 4) != 1:
        raise SystemExit("unsupported Doomguy playable pack version")

    meta = {
        "grid_w": data[6],
        "grid_h": data[7],
        "yaws": data[8],
        "positions": data[9],
        "origin_x": s16(data, 10),
        "origin_y": s16(data, 12),
        "step": data[14],
        "eye_z": data[15],
        "pool_a": u16(data, 16),
        "pool_b": u16(data, 18),
        "pool_size": u16(data, 20),
        "dict_base": u16(data, 22),
        "dict_count": data[24],
        "states": u16(data, 26),
    }
    if not meta["grid_w"] or not meta["grid_h"] or not meta["yaws"]:
        raise SystemExit("invalid playable dimensions")
    if meta["states"] != meta["positions"] * meta["yaws"]:
        raise SystemExit("playable state count does not match positions*yaws")
    if meta["pool_a"] + meta["pool_size"] > meta["pool_b"]:
        raise SystemExit("playable VRAM pools overlap")
    if meta["pool_b"] + meta["pool_size"] > meta["dict_base"]:
        raise SystemExit("playable pool B overlaps resident dictionary")
    if meta["dict_base"] + meta["dict_count"] > DUAL_NAME_TABLE_TILE_LIMIT:
        raise SystemExit("playable patterns overlap the 0x3000 name table")

    p = 28
    lut_n = meta["grid_w"] * meta["grid_h"]
    lut = data[p:p+lut_n]
    p += lut_n
    if len(lut) != lut_n:
        raise SystemExit("truncated playable grid LUT")

    dict_n = meta["dict_count"] * PATTERN_BYTES
    dictionary = data[p:p+dict_n]
    p += dict_n
    if len(dictionary) != dict_n:
        raise SystemExit("truncated playable resident dictionary")

    states = []
    for i in range(meta["states"]):
        if p + 2 + MAP_BYTES > len(data):
            raise SystemExit(f"truncated playable state {i}")
        n = u16(data, p)
        if n > meta["pool_size"]:
            raise SystemExit(f"state {i} exceeds one VRAM pool")
        size = 2 + MAP_BYTES + n * PATTERN_BYTES
        blob = data[p:p+size]
        if len(blob) != size:
            raise SystemExit(f"truncated playable state payload {i}")
        states.append(blob)
        p += size

    if p != len(data):
        raise SystemExit(f"playable pack has {len(data)-p} trailing bytes")
    if sum(1 for v in lut if v != 0xff) != meta["positions"]:
        raise SystemExit("playable grid LUT valid-position count mismatch")
    ordinals=sorted(v for v in lut if v != 0xff)
    if ordinals != list(range(meta["positions"])):
        raise SystemExit("playable grid LUT ordinals are not dense and unique")

    valid={i for i,v in enumerate(lut) if v != 0xff}
    reached=set()
    if valid:
        pending=[next(iter(valid))]
        reached.add(pending[0])
        while pending:
            q=pending.pop()
            x,y=q%meta["grid_w"],q//meta["grid_w"]
            for dy in (-1,0,1):
                for dx in (-1,0,1):
                    if not (dx or dy):
                        continue
                    nx,ny=x+dx,y+dy
                    if 0<=nx<meta["grid_w"] and 0<=ny<meta["grid_h"]:
                        ni=ny*meta["grid_w"]+nx
                        if ni in valid and ni not in reached:
                            reached.add(ni);pending.append(ni)
    if reached != valid:
        raise SystemExit("playable grid contains disconnected movement islands")

    for state_i,blob in enumerate(states):
        n=u16(blob,0)
        dynamic_end=meta["pool_a"]+n
        dict_end=meta["dict_base"]+meta["dict_count"]
        for cell in range(MAP_CELLS):
            tile=u16(blob,2+cell*2)&TILE_ID_MASK
            resident=tile<meta["pool_a"] or meta["dict_base"]<=tile<dict_end
            dynamic=meta["pool_a"]<=tile<dynamic_end
            if not (resident or dynamic):
                raise SystemExit(
                    f"state {state_i} cell {cell} references unavailable tile {tile}")
    return meta, lut, dictionary, states

def make_chunks(states):
    chunks = []
    first = 0
    while first < len(states):
        group = []
        size = 0
        i = first
        while i < len(states):
            entry = states[i]
            if group and size + len(entry) > BANK_STREAM_MAX:
                break
            if len(entry) > BANK_STREAM_MAX:
                raise SystemExit(f"single playable state {i} exceeds bank stream max")
            group.append(entry)
            size += len(entry)
            i += 1
        chunks.append((first, group))
        first = i
    if len(chunks) > MAX_BANK_FILES:
        raise SystemExit(
            f"playable pack needs {len(chunks)} data banks; limit is {MAX_BANK_FILES}")
    return chunks

def emit_bank(outdir, bank_index, first, states, meta):
    offsets = [0]
    raw = bytearray()
    for state in states:
        raw += state
        offsets.append(len(raw))

    s = f"""/* GENERATED Doomguy playable state bank {bank_index}. */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "doomguy_playable_meta.h"

#pragma bank 255
BANKREF(doomguy_playable_bank{bank_index})

extern uint16_t g_map[TSP_MAP_CELLS];

{c_u16_array("k_off", offsets)}
{c_u8_array("k_data", raw)}

#define STATES_IN_BANK {len(states)}u

static const uint8_t *state_ptr(uint16_t local) {{
    if(local>=STATES_IN_BANK)return (const uint8_t *)0;
    return &k_data[k_off[local]];
}}

uint16_t doom_play_bank{bank_index}(uint16_t local,uint8_t op,uint8_t pool,
                                   uint16_t first,uint16_t count) BANKED {{
    const uint8_t *p=state_ptr(local);
    uint16_t n,base;
    if(!p)return 0u;
    n=(uint16_t)p[0]|((uint16_t)p[1]<<8);
    if(op==DOOM_PLAY_OP_PATTERNS)return n;
    if(op==DOOM_PLAY_OP_UPLOAD){{
        if(!count||first>=n)return n;
        if(count>n-first)count=(uint16_t)(n-first);
        base=pool?DOOM_PLAY_POOL_B_BASE:DOOM_PLAY_POOL_A_BASE;
        p+=2u+DOOM_PLAY_MAP_BYTES+(uint32_t)first*32u;
        set_bkg_4bpp_data((uint16_t)(base+first),(uint8_t)count,p);
    }}else if(op==DOOM_PLAY_OP_NAME){{
        uint16_t i,shift=pool?DOOM_PLAY_POOL_SHIFT:0u;
        p+=2u;
        for(i=0u;i<TSP_MAP_CELLS;++i){{
            uint16_t w=(uint16_t)p[0]|((uint16_t)p[1]<<8);
            uint16_t id=(uint16_t)(w&TSP_TILE_ID_MASK);
            p+=2u;
            if(id>=DOOM_PLAY_POOL_A_BASE &&
               id<DOOM_PLAY_POOL_A_BASE+DOOM_PLAY_POOL_SIZE)
                w=(uint16_t)((w&~TSP_TILE_ID_MASK)|(id+shift));
            g_map[i]=w;
        }}
    }}
    return n;
}}
"""
    (outdir / f"doomguy_playable_bank{bank_index}.c").write_text(s)

def emit_dispatch(outdir, chunks, meta, lut, dictionary):
    decl = []
    for i, (first, states) in enumerate(chunks):
        decl.append(
            f"uint16_t doom_play_bank{i}(uint16_t local,uint8_t op,uint8_t pool,uint16_t first,uint16_t count) BANKED;")

    def dispatch_tree(lo, hi, indent="    "):
        """Emit a balanced selector: ~log2(bank count), not a 188-way walk."""
        if hi - lo == 1:
            first, _states = chunks[lo]
            return (
                f"{indent}return doom_play_bank{lo}((uint16_t)(state-{first}u),"
                "op,pool,first,count);"
            )
        mid = (lo + hi) // 2
        boundary = chunks[mid][0]
        return "\n".join((
            f"{indent}if(state<{boundary}u){{",
            dispatch_tree(lo, mid, indent + "    "),
            f"{indent}}}else{{",
            dispatch_tree(mid, hi, indent + "    "),
            f"{indent}}}",
        ))

    dispatch_cases = dispatch_tree(0, len(chunks))

    h = f"""#ifndef DOOMGUY_PLAYABLE_META_H
#define DOOMGUY_PLAYABLE_META_H
#include <stdint.h>
#include <gbdk/platform.h>

#define DOOM_PLAY_GRID_W {meta["grid_w"]}u
#define DOOM_PLAY_GRID_H {meta["grid_h"]}u
#define DOOM_PLAY_YAWS {meta["yaws"]}u
#define DOOM_PLAY_POSITION_COUNT {meta["positions"]}u
#define DOOM_PLAY_STATE_COUNT {meta["states"]}u
#define DOOM_PLAY_ORIGIN_X {meta["origin_x"]}
#define DOOM_PLAY_ORIGIN_Y {meta["origin_y"]}
#define DOOM_PLAY_STEP {meta["step"]}u
#define DOOM_PLAY_EYE_Z {meta["eye_z"]}u
#define DOOM_PLAY_POOL_A_BASE {meta["pool_a"]}u
#define DOOM_PLAY_POOL_B_BASE {meta["pool_b"]}u
#define DOOM_PLAY_POOL_SHIFT (DOOM_PLAY_POOL_B_BASE-DOOM_PLAY_POOL_A_BASE)
#define DOOM_PLAY_POOL_SIZE {meta["pool_size"]}u
#define DOOM_PLAY_DICT_BASE {meta["dict_base"]}u
#define DOOM_PLAY_DICT_COUNT {meta["dict_count"]}u
#define DOOM_PLAY_MAP_BYTES {MAP_BYTES}u
#define DOOM_PLAY_OP_PATTERNS 0u
#define DOOM_PLAY_OP_UPLOAD 1u
#define DOOM_PLAY_OP_NAME 2u

uint8_t doom_play_position_ordinal(uint8_t ix,uint8_t iy) BANKED;
uint16_t doom_play_dispatch(uint16_t state,uint8_t op,uint8_t pool,
                            uint16_t first,uint16_t count) BANKED;
void doom_play_load_dictionary(void) BANKED;

#define doom_play_state_patterns(state) \
    doom_play_dispatch((state),DOOM_PLAY_OP_PATTERNS,0u,0u,0u)
#define doom_play_upload_state(state,pool,first,count) \
    ((void)doom_play_dispatch((state),DOOM_PLAY_OP_UPLOAD,(pool),(first),(count)))
#define doom_play_apply_name(state,pool) \
    ((void)doom_play_dispatch((state),DOOM_PLAY_OP_NAME,(pool),0u,0u))
#endif
"""
    (outdir / "doomguy_playable_meta.h").write_text(h)

    s = f"""/* GENERATED Doomguy playable dispatcher + resident dictionary. */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "doomguy_playable_meta.h"

#pragma bank 255
BANKREF(doomguy_playable_dispatch)

{chr(10).join(decl)}

{c_u8_array("k_grid_lut", lut)}
{c_u8_array("k_dictionary", dictionary)}

uint8_t doom_play_position_ordinal(uint8_t ix,uint8_t iy) BANKED {{
    if(ix>=DOOM_PLAY_GRID_W||iy>=DOOM_PLAY_GRID_H)return 0xffu;
    return k_grid_lut[(uint16_t)iy*DOOM_PLAY_GRID_W+ix];
}}

uint16_t doom_play_dispatch(uint16_t state,uint8_t op,uint8_t pool,
                            uint16_t first,uint16_t count) BANKED {{
    if(state>=DOOM_PLAY_STATE_COUNT)return 0u;
{dispatch_cases}
}}

void doom_play_load_dictionary(void) BANKED {{
    set_bkg_4bpp_data(DOOM_PLAY_DICT_BASE,DOOM_PLAY_DICT_COUNT,k_dictionary);
}}
"""
    (outdir / "doomguy_playable_dispatch.c").write_text(s)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("outdir")
    args = ap.parse_args()

    outdir = pathlib.Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    meta, lut, dictionary, states = parse_pack(args.pack)
    chunks = make_chunks(states)
    for i, (first, group) in enumerate(chunks):
        emit_bank(outdir, i, first, group, meta)
    emit_dispatch(outdir, chunks, meta, lut, dictionary)

    payload = sum(len(s) for s in states)
    print(
        f"DOOM_PLAYABLE_C_PASS states={len(states)} banks={len(chunks)} "
        f"state_bytes={payload} dict_bytes={len(dictionary)} "
        f"dispatch_depth={math.ceil(math.log2(max(1,len(chunks))))} "
        "dual_name_tables=PASS graph=PASS")

if __name__ == "__main__":
    main()
