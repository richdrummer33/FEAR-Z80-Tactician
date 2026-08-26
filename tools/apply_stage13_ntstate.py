#!/usr/bin/env python3
from pathlib import Path
import re

core_p = Path('src/tilesector_core.c')
main_p = Path('src/main_tilesector_gg.c')
raster_p = Path('src/tilesector_raster_gg.s')
nt_p = Path('src/tilesector_ntstate_gg.s')

core = core_p.read_text()
main = main_p.read_text()
raster = raster_p.read_text()
nt = nt_p.read_text()

if 'STAGE13_NTSTATE_WIRED' in core:
    print('Stage 13 authoritative name-table state already wired')
    raise SystemExit(0)

# ---------------------------------------------------------------------------
# Core: the Game Gear no longer rebuilds a 360-word base frame. The existing
# host/reference renderer remains unchanged for acceptance tests.
# ---------------------------------------------------------------------------
marker = 'volatile uint8_t g_ts_render_stage;\n'
if marker not in core:
    raise SystemExit('render-stage marker not found')
core = core.replace(marker, marker + '''\n#ifdef __SDCC\n/* STAGE13_NTSTATE_WIRED: the GG tilemap shadow is persistent authoritative state. */\nvoid ts_nt_begin_frame(void);\nvoid ts_nt_end_frame(void);\n#endif\n''', 1)

m = re.search(r'static void clear_frame\(uint16_t out_map\[TS_MAP_CELLS\], TSColumn cols\[TS_COLS\]\) \{.*?\n\}\n\nvoid ts_render_columns', core, re.S)
if not m:
    raise SystemExit('clear_frame function not found')
old_clear = m.group(0)
old_body = old_clear[:old_clear.rfind('\n\nvoid ts_render_columns')]
# Keep the exact existing implementation for host/reference builds.
host_body = old_body.replace('static void clear_frame(', 'static void clear_frame_host(', 1)
new_clear = '''#ifndef __SDCC\n''' + host_body + '''\n#endif\n\nstatic void clear_frame(uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {\n#ifdef __SDCC\n    uint8_t c;\n    (void)out_map;\n    (void)cols;\n    ts_nt_begin_frame();\n    /* Only the root portal aperture is per-frame state now. No tile words are\n     * cleared or copied; geometry overwrites the authoritative shadow in place. */\n    for(c=0u;c<TS_COLS;++c) {\n        g_clip_top[0][c]=0u;\n        g_clip_bottom[0][c]=143u;\n    }\n#else\n    clear_frame_host(out_map,cols);\n#endif\n}\n\nvoid ts_render_columns'''
core = core[:m.start()] + new_clear + core[m.end():]

old_tail = '''    render_sector(sector,TS_NO_PORTAL,0u,0u,(uint8_t)(TS_COLS-1u),out_map,cols);\n\n    g_ts_render_stage=0u;\n}'''
new_tail = '''    render_sector(sector,TS_NO_PORTAL,0u,0u,(uint8_t)(TS_COLS-1u),out_map,cols);\n\n#ifdef __SDCC\n    /* Reclaim only geometry cells that disappeared since the previous render. */\n    g_ts_render_stage=1u;\n    ts_nt_end_frame();\n#endif\n    g_ts_render_stage=0u;\n}'''
if old_tail not in core:
    raise SystemExit('ts_build_tilemap tail not found')
core = core.replace(old_tail, new_tail, 1)

# ---------------------------------------------------------------------------
# Main: remove the previous-frame map and old compare uploader entirely.
# ---------------------------------------------------------------------------
main = main.replace('uint16_t g_map[TS_MAP_CELLS];\nuint16_t g_prev_map[TS_MAP_CELLS];',
                    'uint16_t g_map[TS_MAP_CELLS];', 1)
main = main.replace('''/* Purpose-built GG name-table uploader in tilesector_vram_gg.s. */\nvoid ts_upload_dirty_map_fast(void);''',
                    '''/* Stage 13 authoritative name-table state / dirty-bit uploader. */\nvoid ts_nt_init(void);\nvoid ts_nt_upload_dirty(void);''', 1)

main = re.sub(r'\nstatic void invalidate_map\(void\) \{.*?\n\}\n', '\n', main, count=1, flags=re.S)
old_upload = '''static uint16_t upload_dirty_map(void) {\n    ts_upload_dirty_map_fast();\n    return g_ts_dirty_words;\n}'''
new_upload = '''static uint16_t upload_dirty_map(void) {\n    ts_nt_upload_dirty();\n    return g_ts_dirty_words;\n}'''
if old_upload not in main:
    raise SystemExit('old upload wrapper not found')
main = main.replace(old_upload, new_upload, 1)
main = main.replace('''    ts_reset(&g_state);\n    invalidate_map();\n    ts_build_tilemap(&g_state,g_map,g_cols);''',
                    '''    ts_reset(&g_state);\n    ts_nt_init();\n    ts_build_tilemap(&g_state,g_map,g_cols);''', 1)

# ---------------------------------------------------------------------------
# Raster: every final tile store goes through the authoritative-shadow primitive.
# Pattern graphics remain preloaded; these are name-table words only.
# ---------------------------------------------------------------------------
raster = raster.replace('        .globl  _g_map\n',
                        '        .globl  _g_map\n        .globl  _ts_nt_store_word\n', 1)

old_edge = '''        push    de\n        ld      a, (#r_row$)\n        call    map_ptr_row_col$\n        pop     de\n        ld      (hl), e\n        inc     hl\n        ld      (hl), d\n        ld      a, (#r_row$)\n        ret'''
new_edge = '''        push    de\n        ld      a, (#r_row$)\n        call    map_ptr_row_col$\n        pop     de\n        ld      a, (#r_row$)\n        call    _ts_nt_store_word\n        ld      a, (#r_row$)\n        ret'''
if old_edge not in raster:
    raise SystemExit('edge store block not found')
raster = raster.replace(old_edge, new_edge, 1)

old_full = '''        call    full_tile_low$\n        ld      e, a\n        ld      a, (#r_cap_delta$)\n        add     a, e\n        ld      (hl), a\n        inc     hl\n        ld      (hl), #0\n        ret'''
new_full = '''        call    full_tile_low$\n        ld      e, a\n        ld      a, (#r_cap_delta$)\n        add     a, e\n        ld      e, a\n        ld      d, #0\n        ld      a, (#r_row$)\n        call    _ts_nt_store_word\n        ret'''
if old_full not in raster:
    raise SystemExit('single full store block not found')
raster = raster.replace(old_full, new_full, 1)

old_interior = '''        call    full_tile_low$\n        ld      (#r_full_tile$), a\n        ld      de, #39              ; after high byte, +39 => next row low\ninterior_loop$:\n        ld      a, (#r_full_tile$)\n        ld      (hl), a\n        inc     hl\n        ld      (hl), #0\n        add     hl, de\n        dec     c\n        jr      nz, interior_loop$\n        ret'''
new_interior = '''        call    full_tile_low$\n        ld      (#r_full_tile$), a\n        ld      a, (#r_fill_first$)\n        ld      (#r_row$), a\ninterior_loop$:\n        ld      a, (#r_full_tile$)\n        ld      e, a\n        ld      d, #0\n        ld      a, (#r_row$)\n        call    _ts_nt_store_word\n        ld      de, #40              ; next hardware name-table row, same column\n        add     hl, de\n        ld      a, (#r_row$)\n        inc     a\n        ld      (#r_row$), a\n        dec     c\n        jr      nz, interior_loop$\n        ret'''
if old_interior not in raster:
    raise SystemExit('interior store block not found')
raster = raster.replace(old_interior, new_interior, 1)

# ---------------------------------------------------------------------------
# Fix two invariants in the first ntstate draft before it is linked:
#  1) changed stores must really preserve HL as promised;
#  2) stale-cell restoration must not destroy the persistent cov_prev pointer.
# Also delete dead VDP-address shift instructions from the ceiling prototype.
# ---------------------------------------------------------------------------
old_dirty = '''        ; Same compact row/column addressing for the dirty bitset. This work is\n        ; paid only when the final name-table word actually changed.\n        ld      a, c'''
new_dirty = '''        ; Same compact row/column addressing for the dirty bitset. This work is\n        ; paid only when the final name-table word actually changed. Preserve\n        ; the authoritative word pointer promised by this routine's ABI.\n        push    hl\n        ld      a, c'''
if old_dirty not in nt:
    raise SystemExit('ntstate dirty-address anchor not found')
nt = nt.replace(old_dirty, new_dirty, 1)
nt = nt.replace('''        ld      a, (#nts_mask$)\n        or      (hl)\n        ld      (hl), a\n\nnts_done$:''',
                '''        ld      a, (#nts_mask$)\n        or      (hl)\n        ld      (hl), a\n        pop     hl\n\nnts_done$:''', 1)

stale_anchor = '''        ; IY = map row base + byte*8 cells (16 bytes).\n        ld      a, (#nte_row$)'''
if stale_anchor not in nt:
    raise SystemExit('stale map-pointer anchor not found')
nt = nt.replace(stale_anchor,
                '''        ; IY = map row base + byte*8 cells (16 bytes). DE is the\n        ; persistent cov_prev cursor, so preserve it across this rare path.\n        push    de\n        ld      a, (#nte_row$)''', 1)
nt = nt.replace('''        djnz    nte_bit_loop$\n\nnte_byte_done$:''',
                '''        djnz    nte_bit_loop$\n        pop     de\n\nnte_byte_done$:''', 1)

nt = nt.replace('''        ; VDP name-table row base = 0x18CC + row*64; byte group adds 0/16/32.\n        ld      a, (#ntu_row$)\n        ld      d, a\n        ld      e, #0\n        ; DE = row << 6 using 16-bit shifts.\n        sla     d\n        rr      e\n        sla     d\n        rr      e\n        ; Above pair is awkward for 8-bit row; use table instead below.\n        ld      a, (#ntu_row$)''',
                '''        ; VDP name-table row base = 0x18CC + row*64; byte group adds\n        ; 0/16/32. Direct row table avoids runtime stride arithmetic.\n        ld      a, (#ntu_row$)''', 1)

core_p.write_text(core)
main_p.write_text(main)
raster_p.write_text(raster)
nt_p.write_text(nt)
print('Applied Stage 13 authoritative name-table shadow + dirty-bit pipeline.')
