#!/usr/bin/env python3
from pathlib import Path

p=Path('src/tilesector_core.c')
s=p.read_text()

# Restore the measured winner at the call boundary: persistent context, col only.
if 'static void raster_surface_column(uint8_t col, TSRasterCtx *ctx) {' in s:
    start=s.index('static void raster_surface_column(uint8_t col, TSRasterCtx *ctx) {')
    end=s.index('\n}\n\n/* Quantize one tile',start)+2
    block=s[start:end]
    block=block.replace('static void raster_surface_column(uint8_t col, TSRasterCtx *ctx) {',
                        'static void raster_surface_column(uint8_t col) {',1)
    block=block.replace('ctx->','g_raster_ctx.')
    s=s[:start]+block+s[end:]
    s=s.replace('raster_surface_column(c,&g_raster_ctx);','raster_surface_column(c);')
    s=s.replace('raster_surface_column(uc,&g_raster_ctx);','raster_surface_column(uc);')

if '#define TS_FAST_LOCAL static' in s:
    print('static-scratch raster experiment already materialized')
    p.write_text(s)
    raise SystemExit(0)

# On SDCC, hot scratch lives in fixed RAM instead of an IX-relative stack frame.
# Host builds keep ordinary automatic locals so test behavior remains conventional.
needle='#include <string.h>\n'
if needle not in s:
    raise SystemExit('include anchor not found')
s=s.replace(needle,needle+'''\n#ifdef __SDCC\n#define TS_FAST_LOCAL static\n#else\n#define TS_FAST_LOCAL\n#endif\n''',1)

def repl(old,new,label):
    global s
    if old not in s:
        raise SystemExit(f'pattern not found: {label}')
    s=s.replace(old,new,1)

repl('''static inline uint16_t edge_entry(uint8_t shade, int16_t local_left, int8_t slope, uint8_t bottom) {\n    uint16_t attr = 0u;\n    uint8_t mag;\n    int8_t off;\n''','''static inline uint16_t edge_entry(uint8_t shade, int16_t local_left, int8_t slope, uint8_t bottom) {\n    TS_FAST_LOCAL uint16_t attr;\n    TS_FAST_LOCAL uint8_t mag;\n    TS_FAST_LOCAL int8_t off;\n    attr = 0u;\n''','edge scratch')

repl('''    int8_t r,plain_last;\n    uint16_t idx,plain;\n''','''    TS_FAST_LOCAL int8_t r,plain_last;\n    TS_FAST_LOCAL uint16_t idx,plain;\n''','full-row scratch')
repl('''        uint8_t cap=cap_last ? TS_CAP_BOTTOM : (cap_first ? TS_CAP_TOP : TS_CAP_NONE);\n''','''        TS_FAST_LOCAL uint8_t cap;\n        cap=cap_last ? TS_CAP_BOTTOM : (cap_first ? TS_CAP_TOP : TS_CAP_NONE);\n''','cap scratch')

repl('''    int8_t slope=clamp_s8((int16_t)(right_y-left_y),-7,7);\n    int8_t r0=row_floor(left_y<right_y?left_y:right_y);\n    int8_t r1=row_floor(left_y>right_y?left_y:right_y);\n    int8_t r;\n''','''    TS_FAST_LOCAL int8_t slope,r0,r1,r;\n    slope=clamp_s8((int16_t)(right_y-left_y),-7,7);\n    r0=row_floor(left_y<right_y?left_y:right_y);\n    r1=row_floor(left_y>right_y?left_y:right_y);\n''','edge-row scratch')
repl('''        int16_t local=(int16_t)(left_y-((int16_t)r<<3));\n''','''        TS_FAST_LOCAL int16_t local;\n        local=(int16_t)(left_y-((int16_t)r<<3));\n''','edge local scratch')

old='''static void raster_surface_column(uint8_t col) {\n    uint16_t *out_map = g_raster_ctx.out_map;\n#ifndef __SDCC\n    TSColumn *cols = g_raster_ctx.cols;\n#endif\n    uint8_t seg_id = g_raster_ctx.seg_id;\n    int16_t inv_l_q6 = g_raster_ctx.inv_l_q6;\n    int16_t inv_r_q6 = g_raster_ctx.inv_r_q6;\n    int16_t top_l = g_raster_ctx.top_l;\n    int16_t top_r = g_raster_ctx.top_r;\n    int16_t bot_l = g_raster_ctx.bot_l;\n    int16_t bot_r = g_raster_ctx.bot_r;\n    uint8_t snap_top = g_raster_ctx.snap_top;\n    uint8_t snap_bottom = g_raster_ctx.snap_bottom;\n    uint8_t border = g_raster_ctx.border;\n    uint8_t *clip_top = g_raster_ctx.clip_top;\n    uint8_t *clip_bottom = g_raster_ctx.clip_bottom;\n    const TSSegment *seg = &k_segments[seg_id];\n    uint8_t shade = shade_for((uint8_t)(((inv_l_q6 + inv_r_q6) >> 1) >> 6),seg->shade_bias);\n    uint8_t clip_first, clip_last;\n    int8_t top_min_row, top_max_row, bot_min_row, bot_max_row;\n    int16_t top_min = top_l < top_r ? top_l : top_r;\n    int16_t top_max = top_l > top_r ? top_l : top_r;\n    int16_t bot_min = bot_l < bot_r ? bot_l : bot_r;\n    int16_t bot_max = bot_l > bot_r ? bot_l : bot_r;\n'''
new='''static void raster_surface_column(uint8_t col) {\n    TS_FAST_LOCAL uint16_t *out_map;\n#ifndef __SDCC\n    TS_FAST_LOCAL TSColumn *cols;\n#endif\n    TS_FAST_LOCAL uint8_t seg_id;\n    TS_FAST_LOCAL int16_t inv_l_q6,inv_r_q6;\n    TS_FAST_LOCAL int16_t top_l,top_r,bot_l,bot_r;\n    TS_FAST_LOCAL uint8_t snap_top,snap_bottom,border;\n    TS_FAST_LOCAL uint8_t *clip_top,*clip_bottom;\n    TS_FAST_LOCAL const TSSegment *seg;\n    TS_FAST_LOCAL uint8_t shade,clip_first,clip_last;\n    TS_FAST_LOCAL int8_t top_min_row,top_max_row,bot_min_row,bot_max_row;\n    TS_FAST_LOCAL int16_t top_min,top_max,bot_min,bot_max;\n\n    out_map=g_raster_ctx.out_map;\n#ifndef __SDCC\n    cols=g_raster_ctx.cols;\n#endif\n    seg_id=g_raster_ctx.seg_id;\n    inv_l_q6=g_raster_ctx.inv_l_q6; inv_r_q6=g_raster_ctx.inv_r_q6;\n    top_l=g_raster_ctx.top_l; top_r=g_raster_ctx.top_r;\n    bot_l=g_raster_ctx.bot_l; bot_r=g_raster_ctx.bot_r;\n    snap_top=g_raster_ctx.snap_top; snap_bottom=g_raster_ctx.snap_bottom;\n    border=g_raster_ctx.border;\n    clip_top=g_raster_ctx.clip_top; clip_bottom=g_raster_ctx.clip_bottom;\n    seg=&k_segments[seg_id];\n    shade=shade_for((uint8_t)(((inv_l_q6+inv_r_q6)>>1)>>6),seg->shade_bias);\n    top_min=top_l<top_r?top_l:top_r; top_max=top_l>top_r?top_l:top_r;\n    bot_min=bot_l<bot_r?bot_l:bot_r; bot_max=bot_l>bot_r?bot_l:bot_r;\n'''
repl(old,new,'raster stack locals')

repl('''        int16_t y = bot_max;\n        int16_t next = (int16_t)(((y >> 3) + 1) << 3);\n        if (next > *clip_top) *clip_top = clamp_u8(next,143u);\n''','''        TS_FAST_LOCAL int16_t y,next;\n        y=bot_max;\n        next=(int16_t)(((y>>3)+1)<<3);\n        if (next>*clip_top) *clip_top=clamp_u8(next,143u);\n''','lintel bound scratch')
repl('''        int16_t y = top_min;\n        int16_t prev = (int16_t)(((y >> 3) << 3) - 1);\n        if (prev < (int16_t)*clip_bottom) *clip_bottom = clamp_u8(prev,143u);\n''','''        TS_FAST_LOCAL int16_t y,prev;\n        y=top_min;\n        prev=(int16_t)(((y>>3)<<3)-1);\n        if (prev<(int16_t)*clip_bottom) *clip_bottom=clamp_u8(prev,143u);\n''','riser bound scratch')

s=s.replace('''/* Persistent hot-path context. ABI experiment: the raster kernel receives\n * (uint8_t col, TSRasterCtx *ctx). Under Z80 __sdcccall(1), col is eligible for\n * A and the following 16-bit pointer for DE, so neither argument needs stack\n * transport. Gearsystem profiling and generated assembly decide whether the\n * extra pointer beats direct static/global addressing. */''','''/* Persistent hot-path context. Measured winner: only the changing column is\n * passed (A under Z80 __sdcccall(1)); related state stays persistent. The SDCC\n * hot kernel also uses fixed RAM scratch to avoid a large IX-relative frame. */''')

p.write_text(s)
print('Materialized persistent-context + static-scratch raster experiment.')
