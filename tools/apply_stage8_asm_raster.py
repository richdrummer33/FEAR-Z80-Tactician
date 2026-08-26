#!/usr/bin/env python3
from pathlib import Path

p = Path('src/tilesector_core.c')
s = p.read_text()
if 'ts_raster_surface_column_fast' in s:
    print('Stage 8 assembly raster bridge already materialized')
    raise SystemExit(0)

def repl(old, new, label):
    global s
    if old not in s:
        raise SystemExit(f'pattern not found: {label}')
    s = s.replace(old, new, 1)

# The GG assembly kernel needs only the values that describe this already-
# projected surface. Host-only diagnostics retain the C pointers/IDs after the
# common prefix, so the first 15 bytes are a deliberately stable GG ABI.
old = '''typedef struct TSRasterCtx {
    uint16_t *out_map;
#ifndef __SDCC
    TSColumn *cols;
#endif
    uint8_t seg_id;
    uint8_t inv_mid;
    int16_t top_l, top_r, bot_l, bot_r;
    uint8_t snap_top, snap_bottom, border;
    uint8_t *clip_top, *clip_bottom;
} TSRasterCtx;

/* Persistent hot-path context. Measured winner: only the changing column is
 * passed (A under Z80 __sdcccall(1)); related state stays persistent. The SDCC
 * hot kernel also uses fixed RAM scratch to avoid a large IX-relative frame. */
static TSRasterCtx g_raster_ctx;
'''
new = '''typedef struct TSRasterCtx {
    /* GG assembly ABI -- keep these offsets stable:
     *  0 profile, 1 shade, 2 top_l, 4 top_r, 6 bot_l, 8 bot_r,
     * 10 border, 11 clip_top, 13 clip_bottom. */
    uint8_t profile;
    uint8_t shade;
    int16_t top_l, top_r, bot_l, bot_r;
    uint8_t border;
    uint8_t *clip_top, *clip_bottom;
#ifndef __SDCC
    uint16_t *out_map;
    TSColumn *cols;
    uint8_t seg_id;
    uint8_t inv_mid;
#endif
} TSRasterCtx;

/* Externally visible because the GG hand-written Z80 rasterizer consumes this
 * persistent context directly. No argument block or automatic scratch frame. */
TSRasterCtx g_raster_ctx;

#ifdef __SDCC
void ts_raster_surface_column_fast(uint8_t col);
#endif
'''
repl(old, new, 'raster context ABI')

# Keep the reference C rasterizer for host tests. The GG build links the assembly
# implementation instead, so none of the large generic C kernel is emitted.
start = s.index('/* Raster one already-visible surface column.')
end = s.index('/* Quantize one tile', start)
block = s[start:end]
block = block.replace('static void raster_surface_column(uint8_t col) {',
                      '#ifndef __SDCC\nstatic void raster_surface_column(uint8_t col) {', 1)
block = block.rstrip() + '\n#endif /* !__SDCC: GG uses tilesector_raster_gg.s */\n\n'

block = block.replace('''    TS_FAST_LOCAL uint8_t seg_id,inv_mid;
    TS_FAST_LOCAL int16_t top_l,top_r,bot_l,bot_r;
    TS_FAST_LOCAL uint8_t snap_top,snap_bottom,border;
''','''    TS_FAST_LOCAL uint8_t seg_id,inv_mid,profile;
    TS_FAST_LOCAL int16_t top_l,top_r,bot_l,bot_r;
    TS_FAST_LOCAL uint8_t snap_top,snap_bottom,border;
''',1)
block = block.replace('''    out_map=g_raster_ctx.out_map;
#ifndef __SDCC
    cols=g_raster_ctx.cols;
#endif
    seg_id=g_raster_ctx.seg_id;
    inv_mid=g_raster_ctx.inv_mid;
''','''    out_map=g_raster_ctx.out_map;
    cols=g_raster_ctx.cols;
    seg_id=g_raster_ctx.seg_id;
    inv_mid=g_raster_ctx.inv_mid;
    profile=g_raster_ctx.profile;
''',1)
block = block.replace('''    snap_top=g_raster_ctx.snap_top; snap_bottom=g_raster_ctx.snap_bottom;
    border=g_raster_ctx.border;
''','''    snap_top=(uint8_t)(profile==TS_PROFILE_RISER);
    snap_bottom=(uint8_t)(profile==TS_PROFILE_LINTEL);
    border=g_raster_ctx.border;
''',1)
block = block.replace('''    seg=&k_segments[seg_id];
    shade=shade_for(inv_mid,seg->shade_bias);
''','''    seg=&k_segments[seg_id];
    shade=g_raster_ctx.shade;
''',1)
block = block.replace('''    if (seg->profile == TS_PROFILE_FULL || seg->profile == TS_PROFILE_RAISED_FULL) {
''','''    if (profile == TS_PROFILE_FULL || profile == TS_PROFILE_RAISED_FULL) {
''',1)
block = block.replace('''    } else if (seg->profile == TS_PROFILE_LINTEL) {
''','''    } else if (profile == TS_PROFILE_LINTEL) {
''',1)
block = block.replace('''    } else if (seg->profile == TS_PROFILE_RISER) {
''','''    } else if (profile == TS_PROFILE_RISER) {
''',1)
s = s[:start] + block + s[end:]

# The ROM assembly path hardcodes g_map. Host still needs the generic output-map
# pointer and diagnostics array.
s = s.replace('''    g_raster_ctx.out_map=out_map;
#ifndef __SDCC
    g_raster_ctx.cols=cols;
#else
    (void)cols;
#endif
''','''#ifndef __SDCC
    g_raster_ctx.out_map=out_map;
    g_raster_ctx.cols=cols;
#else
    (void)out_map;
    (void)cols;
#endif
''',2)

# Solid-sector call site: push only the compact surface descriptor to RAM, then
# hand the changing column byte to the Z80 routine in A.
old = '''        g_raster_ctx.seg_id=seg_id;
        g_raster_ctx.inv_mid=g_best_inv[c];
        g_raster_ctx.top_l=top_l; g_raster_ctx.top_r=top_r;
        g_raster_ctx.bot_l=bot_l; g_raster_ctx.bot_r=bot_r;
        g_raster_ctx.snap_top=0u;
        g_raster_ctx.snap_bottom=0u;
        g_raster_ctx.border=g_best_border[c];
        g_raster_ctx.clip_top=&g_clip_top[depth][c];
        g_raster_ctx.clip_bottom=&g_clip_bottom[depth][c];
        raster_surface_column(c);
'''
new = '''        g_raster_ctx.profile=profile;
        g_raster_ctx.shade=shade_for(g_best_inv[c],k_segments[seg_id].shade_bias);
        g_raster_ctx.top_l=top_l; g_raster_ctx.top_r=top_r;
        g_raster_ctx.bot_l=bot_l; g_raster_ctx.bot_r=bot_r;
        g_raster_ctx.border=g_best_border[c];
        g_raster_ctx.clip_top=&g_clip_top[depth][c];
        g_raster_ctx.clip_bottom=&g_clip_bottom[depth][c];
#ifdef __SDCC
        ts_raster_surface_column_fast(c);
#else
        g_raster_ctx.seg_id=seg_id;
        g_raster_ctx.inv_mid=g_best_inv[c];
        raster_surface_column(c);
#endif
'''
repl(old, new, 'solid raster call')

# Portal-face call site. The direct-pixel pass already has the profile-specific
# geometry; the assembly kernel only materializes it.
old = '''            g_raster_ctx.seg_id=seg_id;
            g_raster_ctx.inv_mid=(uint8_t)(((inv_q6+next_q6)>>1)>>6);
            g_raster_ctx.top_l=top_l; g_raster_ctx.top_r=top_r;
            g_raster_ctx.bot_l=bot_l; g_raster_ctx.bot_r=bot_r;
            g_raster_ctx.snap_top=snap_top;
            g_raster_ctx.snap_bottom=snap_bottom;
            g_raster_ctx.border=border;
            g_raster_ctx.clip_top=&g_clip_top[depth][uc];
            g_raster_ctx.clip_bottom=&g_clip_bottom[depth][uc];
            raster_surface_column(uc);
'''
new = '''            g_raster_ctx.profile=k_segments[seg_id].profile;
            g_raster_ctx.shade=shade_for((uint8_t)(((inv_q6+next_q6)>>1)>>6),
                                          k_segments[seg_id].shade_bias);
            g_raster_ctx.top_l=top_l; g_raster_ctx.top_r=top_r;
            g_raster_ctx.bot_l=bot_l; g_raster_ctx.bot_r=bot_r;
            g_raster_ctx.border=border;
            g_raster_ctx.clip_top=&g_clip_top[depth][uc];
            g_raster_ctx.clip_bottom=&g_clip_bottom[depth][uc];
#ifdef __SDCC
            ts_raster_surface_column_fast(uc);
#else
            g_raster_ctx.seg_id=seg_id;
            g_raster_ctx.inv_mid=(uint8_t)(((inv_q6+next_q6)>>1)>>6);
            (void)snap_top; (void)snap_bottom;
            raster_surface_column(uc);
#endif
'''
repl(old, new, 'portal raster call')

p.write_text(s)
print('Materialized Stage 8 compact raster ABI + GG assembly bridge.')
