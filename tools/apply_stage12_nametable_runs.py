#!/usr/bin/env python3
from pathlib import Path
import re

p = Path('src/tilesector_core.c')
s = p.read_text()
if 'TSNameRunCtx' in s:
    print('Stage 12 name-table run bridge already applied')
    raise SystemExit(0)

# The GG candidate kernel only needs the byte-wide winner/depth arrays. Keep the
# wider interpolation/border arrays solely for the host/reference renderer.
s2 = re.sub(r'^static uint8_t g_best_seg\[TS_COLS\];$',
            'uint8_t g_best_seg[TS_COLS];', s, flags=re.M)
s2 = re.sub(r'^static uint8_t g_best_inv\[TS_COLS\];$',
            'uint8_t g_best_inv[TS_COLS];', s2, flags=re.M)
if s2 == s:
    raise SystemExit('failed to export g_best_seg/g_best_inv')
s = s2
old_arrays = '''static uint8_t g_best_border[TS_COLS];
static int16_t g_best_inv_l_q6[TS_COLS];
static int16_t g_best_inv_r_q6[TS_COLS];'''
new_arrays = '''#ifndef __SDCC
static uint8_t g_best_border[TS_COLS];
static int16_t g_best_inv_l_q6[TS_COLS];
static int16_t g_best_inv_r_q6[TS_COLS];
#endif'''
if old_arrays not in s:
    raise SystemExit('host-only candidate arrays anchor not found')
s = s.replace(old_arrays, new_arrays, 1)

span_anchor = '''typedef struct {
    int8_t c0, c1;
    int16_t inv_q6;
    int16_t step_q6;
    int8_t original_c0, original_c1;
} TSProjectedSpan;
'''
if span_anchor not in s:
    raise SystemExit('TSProjectedSpan anchor not found')
bridge = span_anchor + r'''

/* Stage 12 GG ABI: visibility retains only winner segment + byte reciprocal per
 * coarse column. A contiguous winner run reconstructs reciprocal interpolation
 * once from the cached segment span, then the hand-written materializer walks
 * the whole run. No per-column left/right Q6 arrays survive on GG. */
typedef struct {
    uint8_t seg_id;
    uint8_t view_c0;
    uint8_t view_c1;
    const TSProjectedSpan *span;
} TSCandidateLiteCtx;
TSCandidateLiteCtx g_candidate_lite_ctx;

typedef struct TSNameRunCtx {
    uint8_t c0;
    uint8_t c1;
    uint8_t profile;
    int8_t shade_bias;
    int8_t original_c0;
    int8_t original_c1;
    int16_t inv_q6;
    int16_t step_q6;
    uint8_t *clip_top;
    uint8_t *clip_bottom;
} TSNameRunCtx;
TSNameRunCtx g_name_run_ctx;

#ifdef __SDCC
void ts_candidate_span_lite_fast(void);
void ts_raster_surface_run_fast(void);
#endif
'''
s = s.replace(span_anchor, bridge, 1)

# Candidate selection: host keeps the detailed reference path; GG winner writes
# are deliberately just two bytes per winning column.
m = re.search(r'static void candidate_add_segment\(uint8_t seg_id,uint8_t view_c0,uint8_t view_c1\) \{.*?\n\}\n\nstatic void build_sector_candidates', s, re.S)
if not m:
    raise SystemExit('candidate_add_segment function not found')
new_candidate = r'''static void candidate_add_segment(uint8_t seg_id,uint8_t view_c0,uint8_t view_c1) {
    const TSProjectedSpan *p=project_segment_span(seg_id);
    if (!p) return;
#ifdef __SDCC
    g_candidate_lite_ctx.seg_id=seg_id;
    g_candidate_lite_ctx.view_c0=view_c0;
    g_candidate_lite_ctx.view_c1=view_c1;
    g_candidate_lite_ctx.span=p;
    ts_candidate_span_lite_fast();
#else
    {
        int8_t c,c0=p->c0,c1=p->c1;
        int16_t inv_q6=p->inv_q6;
        while (c0<(int8_t)view_c0) { inv_q6=(int16_t)(inv_q6+p->step_q6); ++c0; }
        if (c1>(int8_t)view_c1) c1=(int8_t)view_c1;
        if (c0>c1) return;
        for (c=c0;c<=c1;++c) {
            uint8_t uc=(uint8_t)c;
            int16_t next_q6=(int16_t)(inv_q6+p->step_q6);
            uint8_t inv=(uint8_t)(((inv_q6+next_q6)>>1)>>6);
            if (g_best_seg[uc]==TS_NO_WALL || inv>g_best_inv[uc]) {
                uint8_t border=0u;
                if (c==p->original_c0 && p->original_c0>=0) border|=1u;
                if (c==p->original_c1 && p->original_c1<(int8_t)TS_COLS) border|=2u;
                g_best_seg[uc]=seg_id;
                g_best_inv[uc]=inv;
                g_best_border[uc]=border;
                g_best_inv_l_q6[uc]=inv_q6;
                g_best_inv_r_q6[uc]=next_q6;
            }
            inv_q6=next_q6;
        }
    }
#endif
}

static void build_sector_candidates'''
s = s[:m.start()] + new_candidate + s[m.end():]

# Replace the GG solid raster loop by one call per contiguous winning segment.
# The host remains byte-for-byte on the detailed C reference implementation.
m = re.search(r'static void render_sector_candidates\(uint8_t depth,uint8_t view_c0,uint8_t view_c1,\n                                     uint16_t out_map\[TS_MAP_CELLS\], TSColumn cols\[TS_COLS\]\) \{.*?\n\}\n\nstatic uint8_t portal_other_sector', s, re.S)
if not m:
    raise SystemExit('render_sector_candidates function not found')
old_func = m.group(0)
old_body = old_func[:old_func.rfind('\n\nstatic uint8_t portal_other_sector')]
# Reuse the original reference function verbatim under !SDCC by renaming it.
reference = old_body.replace('static void render_sector_candidates(', 'static void render_sector_candidates_ref(', 1)
new_render = r'''#ifndef __SDCC
''' + reference + r'''
#endif

static void render_sector_candidates(uint8_t depth,uint8_t view_c0,uint8_t view_c1,
                                     uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {
#ifdef __SDCC
    uint8_t c=view_c0;
    (void)out_map;
    (void)cols;
    while (c<=view_c1) {
        uint8_t seg_id=g_best_seg[c];
        uint8_t run_c0,run_c1;
        const TSProjectedSpan *p;
        int16_t inv_q6;
        int8_t pc;
        if (seg_id==TS_NO_WALL || g_clip_top[depth][c]>g_clip_bottom[depth][c]) {
            ++c;
            continue;
        }
        run_c0=c;
        run_c1=c;
        while (run_c1<view_c1 && g_best_seg[(uint8_t)(run_c1+1u)]==seg_id)
            ++run_c1;
        p=project_segment_span(seg_id);
        if (!p) { c=(uint8_t)(run_c1+1u); continue; }
        inv_q6=p->inv_q6;
        pc=p->c0;
        while (pc<(int8_t)run_c0) { inv_q6=(int16_t)(inv_q6+p->step_q6); ++pc; }
        g_name_run_ctx.c0=run_c0;
        g_name_run_ctx.c1=run_c1;
        g_name_run_ctx.profile=k_segments[seg_id].profile;
        g_name_run_ctx.shade_bias=k_segments[seg_id].shade_bias;
        g_name_run_ctx.original_c0=p->original_c0;
        g_name_run_ctx.original_c1=p->original_c1;
        g_name_run_ctx.inv_q6=inv_q6;
        g_name_run_ctx.step_q6=p->step_q6;
        g_name_run_ctx.clip_top=&g_clip_top[depth][run_c0];
        g_name_run_ctx.clip_bottom=&g_clip_bottom[depth][run_c0];
        ts_raster_surface_run_fast();
        c=(uint8_t)(run_c1+1u);
    }
#else
    render_sector_candidates_ref(depth,view_c0,view_c1,out_map,cols);
#endif
}

static uint8_t portal_other_sector'''
s = s[:m.start()] + new_render + s[m.end():]

# Portal faces are already one projected span. On GG, hand the entire clipped
# span to the same run materializer instead of constructing a raster context for
# every column in C. Host retains the original detailed loop.
m = re.search(r'static void raster_portal_face\(uint8_t seg_id,const TSProjectedSpan \*p,uint8_t depth,\n                               uint8_t view_c0,uint8_t view_c1,\n                               uint16_t out_map\[TS_MAP_CELLS\],TSColumn cols\[TS_COLS\]\) \{.*?\n\}\n\nstatic void render_sector', s, re.S)
if not m:
    raise SystemExit('raster_portal_face function not found')
portal_full = m.group(0)
portal_body = portal_full[:portal_full.rfind('\n\nstatic void render_sector')]
portal_ref = portal_body.replace('static void raster_portal_face(', 'static void raster_portal_face_ref(', 1)
new_portal = r'''#ifndef __SDCC
''' + portal_ref + r'''
#endif

static void raster_portal_face(uint8_t seg_id,const TSProjectedSpan *p,uint8_t depth,
                               uint8_t view_c0,uint8_t view_c1,
                               uint16_t out_map[TS_MAP_CELLS],TSColumn cols[TS_COLS]) {
#ifdef __SDCC
    int8_t c0=p->c0,c1=p->c1;
    int16_t inv_q6=p->inv_q6;
    (void)out_map;
    (void)cols;
    if (seg_id==TS_NO_WALL) return;
    while (c0<(int8_t)view_c0) { inv_q6=(int16_t)(inv_q6+p->step_q6); ++c0; }
    if (c1>(int8_t)view_c1) c1=(int8_t)view_c1;
    if (c0>c1) return;
    g_name_run_ctx.c0=(uint8_t)c0;
    g_name_run_ctx.c1=(uint8_t)c1;
    g_name_run_ctx.profile=k_segments[seg_id].profile;
    g_name_run_ctx.shade_bias=k_segments[seg_id].shade_bias;
    g_name_run_ctx.original_c0=p->original_c0;
    g_name_run_ctx.original_c1=p->original_c1;
    g_name_run_ctx.inv_q6=inv_q6;
    g_name_run_ctx.step_q6=p->step_q6;
    g_name_run_ctx.clip_top=&g_clip_top[depth][(uint8_t)c0];
    g_name_run_ctx.clip_bottom=&g_clip_bottom[depth][(uint8_t)c0];
    ts_raster_surface_run_fast();
#else
    raster_portal_face_ref(seg_id,p,depth,view_c0,view_c1,out_map,cols);
#endif
}

static void render_sector'''
s = s[:m.start()] + new_portal + s[m.end():]

p.write_text(s)
print('Applied Stage 12 compact winner + contiguous name-table run bridge.')
