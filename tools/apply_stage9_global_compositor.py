#!/usr/bin/env python3
from pathlib import Path

p=Path('src/tilesector_core.c')
s=p.read_text()
if 'render_global_compositor' in s:
    print('Stage 9 global compositor already applied')
    raise SystemExit(0)

def repl(old,new,label):
    global s
    if old not in s:
        raise SystemExit(f'pattern not found: {label}')
    s=s.replace(old,new,1)

# Global compositor uses g_best_inv as a coarse opaque depth buffer. Portal
# faces are only overlaid when they are in front of that opaque surface.
old='''        uint8_t uc=(uint8_t)c;
        if (g_clip_top[depth][uc]<=g_clip_bottom[depth][uc]) {
            hl=(int16_t)(inv_q6>>1);
'''
new='''        uint8_t uc=(uint8_t)c;
        uint8_t face_inv=(uint8_t)(((inv_q6+next_q6)>>1)>>6);
        if (g_clip_top[depth][uc]<=g_clip_bottom[depth][uc] && face_inv>g_best_inv[uc]) {
            hl=(int16_t)(inv_q6>>1);
'''
repl(old,new,'portal opaque-depth guard')
repl('''            g_raster_ctx.shade=shade_for((uint8_t)(((inv_q6+next_q6)>>1)>>6),
                                          k_segments[seg_id].shade_bias);
''','''            g_raster_ctx.shade=shade_for(face_inv,k_segments[seg_id].shade_bias);
''','portal shade reuse')
repl('''            g_raster_ctx.inv_mid=(uint8_t)(((inv_q6+next_q6)>>1)>>6);
''','''            g_raster_ctx.inv_mid=face_inv;
''','host portal inv reuse')

anchor='''static uint8_t current_sector(const TSState *s) {
'''
if anchor not in s:
    raise SystemExit('current_sector anchor not found')

insert=r'''/* Stage 9 experiment: collapse the recursive sector/portal traversal into a
 * single coarse-column opaque compositor. All fourteen solid wall segments
 * compete once for the nearest surface in each 8px screen column. Portal
 * lintel/riser faces then overlay that base only where their reciprocal depth
 * is nearer. This trades general recursive topology for dramatically fewer
 * candidate resets, raster passes and portal-control loops in this static demo. */
static void reset_open_clip(uint8_t depth) {
    uint8_t c;
    for(c=0u;c<TS_COLS;++c) {
        g_clip_top[depth][c]=0u;
        g_clip_bottom[depth][c]=143u;
    }
}

static uint16_t span_near_key(const TSProjectedSpan *p) {
    int16_t q;
    int8_t n;
    if(!p) return 0u;
    n=(int8_t)(p->c1-p->c0);
    q=(int16_t)(p->inv_q6 + (int16_t)((p->step_q6*n)>>1));
    if(q<0) q=0;
    return (uint16_t)q;
}

static void overlay_portal(uint8_t portal,uint16_t out_map[TS_MAP_CELLS],TSColumn cols[TS_COLS]) {
    const TSProjectedSpan *p=project_segment_span(k_portals[portal].lintel_seg);
    if(!p) return;
    reset_open_clip(0u);
    g_ts_render_stage=6u;
    raster_portal_face(k_portals[portal].lintel_seg,p,0u,0u,(uint8_t)(TS_COLS-1u),out_map,cols);
    if(k_portals[portal].riser_seg!=TS_NO_WALL) {
        reset_open_clip(0u);
        raster_portal_face(k_portals[portal].riser_seg,p,0u,0u,(uint8_t)(TS_COLS-1u),out_map,cols);
    }
}

static void render_global_compositor(uint16_t out_map[TS_MAP_CELLS],TSColumn cols[TS_COLS]) {
    uint8_t seg,c;
    const TSProjectedSpan *p0,*p1;
    uint16_t k0,k1;

    g_ts_render_stage=3u;
    candidate_reset(0u,(uint8_t)(TS_COLS-1u));
    for(c=0u;c<TS_COLS;++c) g_best_inv[c]=0u;
    for(seg=0u;seg<14u;++seg) candidate_add_segment(seg,0u,(uint8_t)(TS_COLS-1u));

    /* Render exactly one opaque wall surface per open screen column. */
    reset_open_clip(0u);
    g_ts_render_stage=4u;
    render_sector_candidates(0u,0u,(uint8_t)(TS_COLS-1u),out_map,cols);

    /* Far partial faces first, near faces last. Their per-column depth guard
     * prevents a portal plane from painting over a nearer opaque wall. */
    p0=project_segment_span(k_portals[0].lintel_seg);
    p1=project_segment_span(k_portals[1].lintel_seg);
    k0=span_near_key(p0); k1=span_near_key(p1);
    if(k0 && k1) {
        if(k0<k1) { overlay_portal(0u,out_map,cols); overlay_portal(1u,out_map,cols); }
        else       { overlay_portal(1u,out_map,cols); overlay_portal(0u,out_map,cols); }
    } else if(k0) overlay_portal(0u,out_map,cols);
    else if(k1) overlay_portal(1u,out_map,cols);
}

'''
s=s.replace(anchor,insert+anchor,1)

old='''    g_ts_render_stage=3u;
    sector=current_sector(s);
    render_sector(sector,TS_NO_PORTAL,0u,0u,(uint8_t)(TS_COLS-1u),out_map,cols);

    g_ts_render_stage=0u;
'''
new='''    g_ts_render_stage=3u;
    sector=current_sector(s);
    (void)sector;
    render_global_compositor(out_map,cols);

    g_ts_render_stage=0u;
'''
repl(old,new,'build global compositor call')

p.write_text(s)
print('Applied Stage 9 single-pass global opaque compositor experiment.')
