#!/usr/bin/env python3
from pathlib import Path

p = Path('src/tilesector_core.c')
s = p.read_text()

if 'typedef struct TSRasterCtx' in s:
    old='''#else\n    (void)cols;\n#endif\n\n    if (seg->profile == TS_PROFILE_FULL || seg->profile == TS_PROFILE_RAISED_FULL) {\n'''
    new='''#endif\n\n    if (seg->profile == TS_PROFILE_FULL || seg->profile == TS_PROFILE_RAISED_FULL) {\n'''
    if old in s:
        s=s.replace(old,new,1)
        p.write_text(s)
        print('Fixed SDCC-only raster context diagnostics branch.')
    else:
        print('Stage 7 raster context already materialized and compile fix already present.')
    raise SystemExit(0)


def repl(old, new, label):
    global s
    if old not in s:
        raise SystemExit(f'pattern not found: {label}')
    s = s.replace(old, new, 1)

marker = '''/* Raster one already-visible surface column. The caller supplies connected\n * edge endpoints; this function only writes the small set of tile rows that the\n * surface actually occupies. No depth compare and no row*20 multiply remain. */\n'''
ctx = '''typedef struct TSRasterCtx {\n    uint16_t *out_map;\n#ifndef __SDCC\n    TSColumn *cols;\n#endif\n    uint8_t seg_id;\n    int16_t inv_l_q6, inv_r_q6;\n    int16_t top_l, top_r, bot_l, bot_r;\n    uint8_t snap_top, snap_bottom, border;\n    uint8_t *clip_top, *clip_bottom;\n} TSRasterCtx;\n\n/* Persistent hot-path context. The pointer is deliberately not passed: under\n * Z80 __sdcccall(1), a lone uint8_t column arrives in A with zero stack args.\n * Loop-stable pointers are written once by the caller; only per-column geometry\n * is updated before entering the raster kernel. */\nstatic TSRasterCtx g_raster_ctx;\n\n'''
repl(marker, ctx + marker, 'insert raster context')

old_sig = '''static inline void raster_surface_column(uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS],\n                                  uint8_t col, uint8_t seg_id,\n                                  int16_t inv_l_q6, int16_t inv_r_q6,\n                                  int16_t top_l, int16_t top_r,\n                                  int16_t bot_l, int16_t bot_r,\n                                  uint8_t snap_top, uint8_t snap_bottom,\n                                  uint8_t border,\n                                  uint8_t *clip_top, uint8_t *clip_bottom,\n                                  uint8_t mutate_clip) {\n    const TSSegment *seg = &k_segments[seg_id];\n'''
new_sig = '''static void raster_surface_column(uint8_t col) {\n    uint16_t *out_map = g_raster_ctx.out_map;\n#ifndef __SDCC\n    TSColumn *cols = g_raster_ctx.cols;\n#endif\n    uint8_t seg_id = g_raster_ctx.seg_id;\n    int16_t inv_l_q6 = g_raster_ctx.inv_l_q6;\n    int16_t inv_r_q6 = g_raster_ctx.inv_r_q6;\n    int16_t top_l = g_raster_ctx.top_l;\n    int16_t top_r = g_raster_ctx.top_r;\n    int16_t bot_l = g_raster_ctx.bot_l;\n    int16_t bot_r = g_raster_ctx.bot_r;\n    uint8_t snap_top = g_raster_ctx.snap_top;\n    uint8_t snap_bottom = g_raster_ctx.snap_bottom;\n    uint8_t border = g_raster_ctx.border;\n    uint8_t *clip_top = g_raster_ctx.clip_top;\n    uint8_t *clip_bottom = g_raster_ctx.clip_bottom;\n    const TSSegment *seg = &k_segments[seg_id];\n'''
repl(old_sig, new_sig, 'raster signature')

repl('''    if (!mutate_clip) return;\n    if (seg->profile == TS_PROFILE_FULL || seg->profile == TS_PROFILE_RAISED_FULL) {\n''','''    if (seg->profile == TS_PROFILE_FULL || seg->profile == TS_PROFILE_RAISED_FULL) {\n''','drop mutate flag')

repl('''    int16_t carry_top=0,carry_bottom=0;\n\n    for (c=view_c0;c<=view_c1;++c) {\n''','''    int16_t carry_top=0,carry_bottom=0;\n\n    g_raster_ctx.out_map=out_map;\n#ifndef __SDCC\n    g_raster_ctx.cols=cols;\n#else\n    (void)cols;\n#endif\n    for (c=view_c0;c<=view_c1;++c) {\n''','solid stable ctx')

old_call1 = '''        raster_surface_column(out_map,cols,c,seg_id,\n                              g_best_inv_l_q6[c],g_best_inv_r_q6[c],\n                              top_l,top_r,bot_l,bot_r,\n                              (uint8_t)(stl||str),(uint8_t)(sbl||sbr),g_best_border[c],\n                              &g_clip_top[depth][c],&g_clip_bottom[depth][c],1u);\n'''
new_call1 = '''        g_raster_ctx.seg_id=seg_id;\n        g_raster_ctx.inv_l_q6=g_best_inv_l_q6[c];\n        g_raster_ctx.inv_r_q6=g_best_inv_r_q6[c];\n        g_raster_ctx.top_l=top_l; g_raster_ctx.top_r=top_r;\n        g_raster_ctx.bot_l=bot_l; g_raster_ctx.bot_r=bot_r;\n        g_raster_ctx.snap_top=(uint8_t)(stl||str);\n        g_raster_ctx.snap_bottom=(uint8_t)(sbl||sbr);\n        g_raster_ctx.border=g_best_border[c];\n        g_raster_ctx.clip_top=&g_clip_top[depth][c];\n        g_raster_ctx.clip_bottom=&g_clip_bottom[depth][c];\n        raster_surface_column(c);\n'''
repl(old_call1, new_call1, 'solid raster call')

repl('''    uint8_t have_carry=0u;\n    if (seg_id==TS_NO_WALL) return;\n''','''    uint8_t have_carry=0u;\n    if (seg_id==TS_NO_WALL) return;\n    g_raster_ctx.out_map=out_map;\n#ifndef __SDCC\n    g_raster_ctx.cols=cols;\n#else\n    (void)cols;\n#endif\n''','portal stable ctx')

old_call2 = '''            raster_surface_column(out_map,cols,uc,seg_id,inv_q6,next_q6,\n                                  top_l,top_r,bot_l,bot_r,\n                                  (uint8_t)(stl||str),(uint8_t)(sbl||sbr),border,\n                                  &g_clip_top[depth][uc],&g_clip_bottom[depth][uc],1u);\n'''
new_call2 = '''            g_raster_ctx.seg_id=seg_id;\n            g_raster_ctx.inv_l_q6=inv_q6; g_raster_ctx.inv_r_q6=next_q6;\n            g_raster_ctx.top_l=top_l; g_raster_ctx.top_r=top_r;\n            g_raster_ctx.bot_l=bot_l; g_raster_ctx.bot_r=bot_r;\n            g_raster_ctx.snap_top=(uint8_t)(stl||str);\n            g_raster_ctx.snap_bottom=(uint8_t)(sbl||sbr);\n            g_raster_ctx.border=border;\n            g_raster_ctx.clip_top=&g_clip_top[depth][uc];\n            g_raster_ctx.clip_bottom=&g_clip_bottom[depth][uc];\n            raster_surface_column(uc);\n'''
repl(old_call2, new_call2, 'portal raster call')

p.write_text(s)
print('Materialized persistent Stage 7 raster context.')
