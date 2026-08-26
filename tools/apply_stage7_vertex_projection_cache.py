#!/usr/bin/env python3
from pathlib import Path

p=Path('src/tilesector_core.c')
s=p.read_text()

if 'g_proj_state[TS_VERTICES]' in s:
    print('shared-vertex projection cache already materialized')
    raise SystemExit(0)


def repl(old,new,label):
    global s
    if old not in s:
        raise SystemExit(f'pattern not found: {label}')
    s=s.replace(old,new,1)

repl('''static int16_t g_cam_x_q4[TS_VERTICES];\nstatic int16_t g_cam_z_q4[TS_VERTICES];\n\n/* Projection is invariant for a world segment during one rendered frame. Cache\n''','''static int16_t g_cam_x_q4[TS_VERTICES];\nstatic int16_t g_cam_z_q4[TS_VERTICES];\n\n/* Segment endpoints share authored vertices. Lazily project each touched\n * camera-space vertex at most once per frame; near-plane-generated endpoints\n * remain one-off because their X is segment-specific after clipping. */\nstatic int16_t g_proj_x[TS_VERTICES];\nstatic uint8_t g_proj_inv[TS_VERTICES];\nstatic uint8_t g_proj_state[TS_VERTICES];\n\n/* Projection is invariant for a world segment during one rendered frame. Cache\n''','projection cache arrays')

old='''/* Exact positive quotient decomposition avoids a 32-bit multiply:\n * px ~= cam_x_q4 * inv / 512. */\nstatic int16_t project_x_q4(int16_t cam_x_q4, int16_t cam_z_q4) {\n    uint8_t inv;\n    uint16_t ax, xi, xf, p, rem, extra;\n    int16_t px;\n    uint8_t neg = 0u;\n\n    if (cam_x_q4 < 0) { neg = 1u; ax = (uint16_t)(-cam_x_q4); }\n    else ax = (uint16_t)cam_x_q4;\n    if (ax > (uint16_t)(127u << 4)) ax = (uint16_t)(127u << 4);\n\n    inv = inv_for_zq4(cam_z_q4);\n    xi = (uint16_t)(ax >> 4);\n'''
new='''/* Exact positive quotient decomposition avoids a 32-bit multiply:\n * px ~= cam_x_q4 * inv / 512. The reciprocal is supplied by the caller so a\n * projected endpoint never computes inv_for_zq4() twice. */\nstatic int16_t project_x_inv_q4(int16_t cam_x_q4, uint8_t inv) {\n    uint16_t ax, xi, xf, p, rem, extra;\n    int16_t px;\n    uint8_t neg = 0u;\n\n    if (cam_x_q4 < 0) { neg = 1u; ax = (uint16_t)(-cam_x_q4); }\n    else ax = (uint16_t)cam_x_q4;\n    if (ax > (uint16_t)(127u << 4)) ax = (uint16_t)(127u << 4);\n\n    xi = (uint16_t)(ax >> 4);\n'''
repl(old,new,'projection helper accepts reciprocal')

old='''static uint8_t project_segment_span_uncached(uint8_t seg_id, TSProjectedSpan *p) {\n    const TSSegment *seg = &k_segments[seg_id];\n    int16_t x0 = g_cam_x_q4[seg->a], z0 = g_cam_z_q4[seg->a];\n    int16_t x1 = g_cam_x_q4[seg->b], z1 = g_cam_z_q4[seg->b];\n    int16_t sx0, sx1;\n    uint8_t inv0, inv1, span;\n    int8_t c0, c1;\n\n    if (z0 < TS_NEAR_Z_Q4 && z1 < TS_NEAR_Z_Q4) return 0u;\n    if (z0 < TS_NEAR_Z_Q4) { x0 = clip_x_near_q4(x0,z0,x1,z1); z0 = TS_NEAR_Z_Q4; }\n    if (z1 < TS_NEAR_Z_Q4) { x1 = clip_x_near_q4(x1,z1,x0,z0); z1 = TS_NEAR_Z_Q4; }\n\n    /* Cheap ~90-degree horizontal frustum reject before projection. */\n    if (x0 < -z0 && x1 < -z1) return 0u;\n    if (x0 >  z0 && x1 >  z1) return 0u;\n\n    sx0 = project_x_q4(x0,z0);\n    sx1 = project_x_q4(x1,z1);\n    inv0 = inv_for_zq4(z0);\n    inv1 = inv_for_zq4(z1);\n'''
new='''static uint8_t project_segment_span_uncached(uint8_t seg_id, TSProjectedSpan *p) {\n    const TSSegment *seg = &k_segments[seg_id];\n    uint8_t va=seg->a, vb=seg->b;\n    int16_t x0 = g_cam_x_q4[va], z0 = g_cam_z_q4[va];\n    int16_t x1 = g_cam_x_q4[vb], z1 = g_cam_z_q4[vb];\n    int16_t sx0, sx1;\n    uint8_t inv0, inv1, span;\n    uint8_t clipped0=0u, clipped1=0u;\n    int8_t c0, c1;\n\n    if (z0 < TS_NEAR_Z_Q4 && z1 < TS_NEAR_Z_Q4) return 0u;\n    if (z0 < TS_NEAR_Z_Q4) {\n        x0 = clip_x_near_q4(x0,z0,x1,z1); z0 = TS_NEAR_Z_Q4; clipped0=1u;\n    }\n    if (z1 < TS_NEAR_Z_Q4) {\n        x1 = clip_x_near_q4(x1,z1,x0,z0); z1 = TS_NEAR_Z_Q4; clipped1=1u;\n    }\n\n    /* Cheap ~90-degree horizontal frustum reject before projection. */\n    if (x0 < -z0 && x1 < -z1) return 0u;\n    if (x0 >  z0 && x1 >  z1) return 0u;\n\n    if (clipped0) {\n        inv0=k_invz[TS_NEAR_Z];\n        sx0=project_x_inv_q4(x0,inv0);\n    } else {\n        if (!g_proj_state[va]) {\n            inv0=inv_for_zq4(z0);\n            g_proj_inv[va]=inv0;\n            g_proj_x[va]=project_x_inv_q4(x0,inv0);\n            g_proj_state[va]=1u;\n        }\n        inv0=g_proj_inv[va]; sx0=g_proj_x[va];\n    }\n    if (clipped1) {\n        inv1=k_invz[TS_NEAR_Z];\n        sx1=project_x_inv_q4(x1,inv1);\n    } else {\n        if (!g_proj_state[vb]) {\n            inv1=inv_for_zq4(z1);\n            g_proj_inv[vb]=inv1;\n            g_proj_x[vb]=project_x_inv_q4(x1,inv1);\n            g_proj_state[vb]=1u;\n        }\n        inv1=g_proj_inv[vb]; sx1=g_proj_x[vb];\n    }\n'''
repl(old,new,'shared endpoint projection')

repl('''    transform_vertices_q4(s);\n    memset(g_span_state,0,sizeof(g_span_state));\n\n    g_ts_render_stage=3u;\n''','''    transform_vertices_q4(s);\n    memset(g_span_state,0,sizeof(g_span_state));\n    memset(g_proj_state,0,sizeof(g_proj_state));\n\n    g_ts_render_stage=3u;\n''','reset projection states')

p.write_text(s)
print('Materialized lazy shared-vertex projection cache.')
