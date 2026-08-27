#!/usr/bin/env python3
from pathlib import Path
import re

p = Path('src/tilesector_core.c')
s = p.read_text()

if 'STAGE17_RUN_ENVELOPE' in s:
    print('Stage 17 run-envelope visibility already applied')
    raise SystemExit(0)
if 'TSNameRunCtx' not in s:
    raise SystemExit('apply Stage 12 compact run bridge first')

marker = '''#ifdef __SDCC
void ts_candidate_span_lite_fast(void);
void ts_raster_surface_run_fast(void);
#endif
'''
if marker not in s:
    raise SystemExit('Stage 12 GG declaration marker not found')

insert = marker + r'''
#ifdef __SDCC
/* STAGE17_RUN_ENVELOPE
 *
 * The nearest-wall result is represented directly as a left-to-right list of
 * horizontal runs instead of twenty independent winner columns.
 *
 * c0 is implicit: first run starts at g_vis_view_c0, every following run starts
 * one column after the previous run's inclusive c1. inv_q6 is reciprocal depth
 * at that implicit left boundary. A normal view therefore costs a handful of
 * six-byte descriptors instead of repeatedly touching 20-column winner arrays.
 */
typedef struct TSVisibleRun {
    uint8_t c1;
    uint8_t seg_id;
    int16_t inv_q6;
    int16_t step_q6;
} TSVisibleRun;

static TSVisibleRun g_vis_runs[TS_COLS];
static TSVisibleRun g_vis_tmp[TS_COLS];
static uint8_t g_vis_run_count;
static uint8_t g_vis_tmp_count;
static uint8_t g_vis_view_c0;

/* Advance an affine reciprocal line by a tiny screen-column count. Keeping this
 * as repeated 16-bit adds avoids SDCC's general multiply helper; the count is
 * at most nineteen and is normally only a boundary-sized adjustment. */
static int16_t vis_advance_q6(int16_t v, int16_t step, uint8_t count) {
    while (count) {
        v = (int16_t)(v + step);
        --count;
    }
    return v;
}

static int16_t vis_mid_q6(int16_t inv_q6, int16_t step_q6) {
    return (int16_t)(inv_q6 + (step_q6 >> 1));
}

static uint8_t vis_inv8(int16_t inv_q6, int16_t step_q6) {
    return (uint8_t)(vis_mid_q6(inv_q6,step_q6) >> 6);
}

/* Append to the temporary partition. Adjacent pieces owned by the same world
 * segment collapse immediately, so the theoretical maximum is twenty runs and
 * the normal case remains only a few descriptors. */
static void vis_append(uint8_t c1, uint8_t seg_id, int16_t inv_q6, int16_t step_q6) {
    uint8_t n = g_vis_tmp_count;
    if (n && g_vis_tmp[(uint8_t)(n-1u)].seg_id == seg_id) {
        g_vis_tmp[(uint8_t)(n-1u)].c1 = c1;
        return;
    }
    if (n >= TS_COLS) return; /* impossible for a 20-column integer partition */
    g_vis_tmp[n].c1 = c1;
    g_vis_tmp[n].seg_id = seg_id;
    g_vis_tmp[n].inv_q6 = inv_q6;
    g_vis_tmp[n].step_q6 = step_q6;
    g_vis_tmp_count = (uint8_t)(n + 1u);
}

/* Merge one projected candidate line into the existing front envelope.
 *
 * Reciprocal depth is affine across screen X. Therefore when the candidate is
 * at least one full Q6 reciprocal unit nearer at both ends of an overlap, it
 * wins the entire overlap without looking at interior columns. If it is <= the
 * current line at both ends, the existing run wins (ties intentionally keep the
 * earlier segment, matching the old per-column candidate rule).
 *
 * Only an actual crossing or sub-one-unit quantization ambiguity falls back to
 * exact column comparisons. This preserves the old winner semantics while
 * making the overwhelmingly common separated-wall case run-granular.
 */
static void vis_merge_candidate(uint8_t seg_id, const TSProjectedSpan *p,
                                uint8_t view_c0, uint8_t view_c1) {
    int8_t cand_c0 = p->c0;
    int8_t cand_c1 = p->c1;
    int16_t cand_base = p->inv_q6;
    int16_t cand_step = p->step_q6;
    uint8_t i;
    uint8_t run_start;

    while (cand_c0 < (int8_t)view_c0) {
        cand_base = (int16_t)(cand_base + cand_step);
        ++cand_c0;
    }
    if (cand_c1 > (int8_t)view_c1) cand_c1 = (int8_t)view_c1;
    if (cand_c0 > cand_c1) return;

    g_vis_tmp_count = 0u;
    run_start = g_vis_view_c0;

    for (i=0u; i<g_vis_run_count; ++i) {
        const TSVisibleRun *r = &g_vis_runs[i];
        uint8_t run_end = r->c1;

        if (run_end < (uint8_t)cand_c0 || run_start > (uint8_t)cand_c1) {
            vis_append(run_end,r->seg_id,r->inv_q6,r->step_q6);
            run_start = (uint8_t)(run_end + 1u);
            continue;
        }

        {
            uint8_t ov0 = run_start > (uint8_t)cand_c0 ? run_start : (uint8_t)cand_c0;
            uint8_t ov1 = run_end < (uint8_t)cand_c1 ? run_end : (uint8_t)cand_c1;
            int16_t old_inv;
            int16_t new_inv;

            if (run_start < ov0)
                vis_append((uint8_t)(ov0-1u),r->seg_id,r->inv_q6,r->step_q6);

            old_inv = vis_advance_q6(r->inv_q6,r->step_q6,(uint8_t)(ov0-run_start));
            new_inv = vis_advance_q6(cand_base,cand_step,(uint8_t)(ov0-(uint8_t)cand_c0));

            if (r->seg_id == TS_NO_WALL) {
                vis_append(ov1,seg_id,new_inv,cand_step);
            } else {
                uint8_t width = (uint8_t)(ov1-ov0);
                int16_t old_end = vis_advance_q6(old_inv,r->step_q6,width);
                int16_t new_end = vis_advance_q6(new_inv,cand_step,width);
                int16_t d0 = (int16_t)(vis_mid_q6(new_inv,cand_step) -
                                       vis_mid_q6(old_inv,r->step_q6));
                int16_t d1 = (int16_t)(vis_mid_q6(new_end,cand_step) -
                                       vis_mid_q6(old_end,r->step_q6));

                if (d0 >= 64 && d1 >= 64) {
                    vis_append(ov1,seg_id,new_inv,cand_step);
                } else if (d0 <= 0 && d1 <= 0) {
                    vis_append(ov1,r->seg_id,old_inv,r->step_q6);
                } else {
                    /* Rare exact fallback around a depth crossing or a Q6
                     * quantization tie. Never scans columns that are clearly
                     * ordered at run scale. */
                    uint8_t col;
                    int16_t oi = old_inv;
                    int16_t ni = new_inv;
                    for (col=ov0; col<=ov1; ++col) {
                        if (vis_inv8(ni,cand_step) > vis_inv8(oi,r->step_q6))
                            vis_append(col,seg_id,ni,cand_step);
                        else
                            vis_append(col,r->seg_id,oi,r->step_q6);
                        oi = (int16_t)(oi + r->step_q6);
                        ni = (int16_t)(ni + cand_step);
                    }
                }
            }

            if (run_end > ov1) {
                int16_t after = vis_advance_q6(r->inv_q6,r->step_q6,
                                               (uint8_t)((ov1+1u)-run_start));
                vis_append(run_end,r->seg_id,after,r->step_q6);
            }
        }
        run_start = (uint8_t)(run_end + 1u);
    }

    g_vis_run_count = g_vis_tmp_count;
    for (i=0u; i<g_vis_run_count; ++i) {
        g_vis_runs[i].c1 = g_vis_tmp[i].c1;
        g_vis_runs[i].seg_id = g_vis_tmp[i].seg_id;
        g_vis_runs[i].inv_q6 = g_vis_tmp[i].inv_q6;
        g_vis_runs[i].step_q6 = g_vis_tmp[i].step_q6;
    }
}
#endif
'''
s = s.replace(marker, insert, 1)

old_reset = '''static void candidate_reset(uint8_t view_c0,uint8_t view_c1) {
    uint8_t c;
    for (c=view_c0;c<=view_c1;++c) g_best_seg[c]=TS_NO_WALL;
}'''
new_reset = '''static void candidate_reset(uint8_t view_c0,uint8_t view_c1) {
#ifdef __SDCC
    g_vis_view_c0=view_c0;
    g_vis_run_count=1u;
    g_vis_runs[0].c1=view_c1;
    g_vis_runs[0].seg_id=TS_NO_WALL;
    g_vis_runs[0].inv_q6=0;
    g_vis_runs[0].step_q6=0;
#else
    uint8_t c;
    for (c=view_c0;c<=view_c1;++c) g_best_seg[c]=TS_NO_WALL;
#endif
}'''
if old_reset not in s:
    raise SystemExit('candidate_reset anchor not found')
s = s.replace(old_reset,new_reset,1)

# Stage 12 already isolates the GG candidate call under this exact branch.
old_candidate = '''#ifdef __SDCC
    g_candidate_lite_ctx.seg_id=seg_id;
    g_candidate_lite_ctx.view_c0=view_c0;
    g_candidate_lite_ctx.view_c1=view_c1;
    g_candidate_lite_ctx.span=p;
    ts_candidate_span_lite_fast();
#else'''
new_candidate = '''#ifdef __SDCC
    vis_merge_candidate(seg_id,p,view_c0,view_c1);
#else'''
if old_candidate not in s:
    raise SystemExit('Stage 12 candidate-lite branch not found')
s = s.replace(old_candidate,new_candidate,1)

# Replace only the Stage-12 GG renderer body. The host/reference function that
# Stage 12 preserved remains unchanged.
m = re.search(
    r'static void render_sector_candidates\(uint8_t depth,uint8_t view_c0,uint8_t view_c1,\n'
    r'                                     uint16_t out_map\[TS_MAP_CELLS\], TSColumn cols\[TS_COLS\]\) \{'
    r'.*?\n\}\n\nstatic uint8_t portal_other_sector',
    s,re.S)
if not m:
    raise SystemExit('Stage 12 render_sector_candidates function not found')

replacement = r'''static void render_sector_candidates(uint8_t depth,uint8_t view_c0,uint8_t view_c1,
                                     uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {
#ifdef __SDCC
    uint8_t i;
    uint8_t c0=g_vis_view_c0;
    (void)view_c0;
    (void)view_c1;
    (void)out_map;
    (void)cols;

    /* Visibility already IS the run list. There is no twenty-column winner
     * array to rescan/compress before materialization. */
    for (i=0u;i<g_vis_run_count;++i) {
        const TSVisibleRun *r=&g_vis_runs[i];
        uint8_t c1=r->c1;
        uint8_t seg_id=r->seg_id;
        if (seg_id!=TS_NO_WALL) {
            const TSProjectedSpan *p=project_segment_span(seg_id);
            if (p) {
                g_name_run_ctx.c0=c0;
                g_name_run_ctx.c1=c1;
                g_name_run_ctx.profile=k_segments[seg_id].profile;
                g_name_run_ctx.shade_bias=k_segments[seg_id].shade_bias;
                g_name_run_ctx.original_c0=p->original_c0;
                g_name_run_ctx.original_c1=p->original_c1;
                g_name_run_ctx.inv_q6=r->inv_q6;
                g_name_run_ctx.step_q6=r->step_q6;
                g_name_run_ctx.clip_top=&g_clip_top[depth][c0];
                g_name_run_ctx.clip_bottom=&g_clip_bottom[depth][c0];
                ts_raster_surface_run_fast();
            }
        }
        c0=(uint8_t)(c1+1u);
    }
#else
    render_sector_candidates_ref(depth,view_c0,view_c1,out_map,cols);
#endif
}

static uint8_t portal_other_sector'''
s = s[:m.start()] + replacement + s[m.end():]

p.write_text(s)
print('Applied Stage 17 exact run-envelope visibility.')
