#!/usr/bin/env python3
"""Inject the first playable FULL run-edge PROGJOIN rung.

Experimental only. The default checked-in renderer remains untouched unless this
script is explicitly run. It assumes the parity materializer rungs have already
been applied to src/tilesector_polar_materialize_gg.s.

Semantics:
  * mode 0 / FULL walls only (the baked bodies contain absolute shade-1 words)
  * preflight top family 0 and bottom family 2 before any writes
  * play both against pre-surface ownership using the shared gated runtime
  * then call the existing run materializer for span claim/interior/borders
  * suppress only its old FULL edge emission when compiled playback succeeded
  * any unsupported selector/saturation condition leaves the old path intact
"""
from pathlib import Path

R=Path('src/tilesector_polar_renderer.c')
A=Path('src/tilesector_polar_materialize_gg.s')
r=R.read_text(); a=A.read_text()

old='''#ifndef TSPF_SCREEN_DEPTH_PLANE\n#define TSPF_SCREEN_DEPTH_PLANE 0\n#endif\n'''
new=old+'''#ifndef TSPF_PROGJOIN_FULL\n#define TSPF_PROGJOIN_FULL 0\n#endif\n#if defined(__SDCC) && TSPF_PROGJOIN_FULL\n#include "tilesector_polar_progjoin_runtime.h"\n#endif\n'''
if old not in r: raise SystemExit('renderer feature-macro anchor missing')
r=r.replace(old,new,1)

old='''int16_t g_polar_run_iq;\nint16_t g_polar_run_step;\n#endif\n'''
new='''int16_t g_polar_run_iq;\nint16_t g_polar_run_step;\n/* Set for exactly one run call when compiled FULL top+bottom edges have already\n * been emitted against the pre-surface ownership state. Assembly still claims\n * spans and emits interiors/borders, but skips its legacy FULL edge walker. */\nuint8_t g_polar_progjoin_edges_done;\n#endif\n'''
if old not in r: raise SystemExit('renderer bridge-global anchor missing')
r=r.replace(old,new,1)

anchor='''static void draw_run(uint16_t *out, TSPColumn *cols, const PolarRun *r)\n{\n'''
helper='''#if defined(__SDCC) && TSPF_PROGJOIN_FULL\nstatic uint8_t progjoin_full_edges(uint16_t *out, uint8_t c0, uint8_t n,\n                                   int16_t iq, int16_t step)\n{\n    TSPProgjoinRunPlan top, bot;\n    int16_t a0, an, q0, qn;\n    uint8_t invl, invr, hl, hr;\n    int16_t tl, tr, bl, br;\n    int8_t top_row, bot_row;\n\n    /* The compiled selector assumes the linear inverse-depth model, so reject\n     * a run if either end of the monotonic C+1 endpoint sequence saturates. */\n    a0 = (int16_t)(iq + 32);\n    an = (int16_t)(iq + (int16_t)((int16_t)n * step) + 32);\n    q0 = (int16_t)(a0 >> 6);\n    qn = (int16_t)(an >> 6);\n    if (q0 < 0 || q0 > 255 || qn < 0 || qn > 255) return 0u;\n\n    if (!tsp_progjoin_preflight_run(step, 0u, n, iq, &top)) return 0u;\n    if (!tsp_progjoin_preflight_run(step, 2u, n, iq, &bot)) return 0u;\n\n    invl = (uint8_t)((iq + 32) >> 6);\n    invr = (uint8_t)((iq + step + 32) >> 6);\n    hl = (uint8_t)(invl >> 1);\n    hr = (uint8_t)(invr >> 1);\n    tl = (int16_t)(71 - hl); tr = (int16_t)(71 - hr);\n    bl = (int16_t)(72 + hl); br = (int16_t)(72 + hr);\n    top_row = row_floor(tl < tr ? tl : tr);\n    bot_row = row_floor(bl < br ? bl : br);\n\n    if (!tsp_progjoin_play_plan_gated(&top, out, g_polar_nt_cov_cur,\n                                       g_polar_nt_row_min, g_polar_nt_row_max,\n                                       top_row, c0)) return 0u;\n    if (!tsp_progjoin_play_plan_gated(&bot, out, g_polar_nt_cov_cur,\n                                       g_polar_nt_row_min, g_polar_nt_row_max,\n                                       bot_row, c0)) return 0u;\n    return 1u;\n}\n#endif\n\n'''+anchor
if anchor not in r: raise SystemExit('draw_run anchor missing')
r=r.replace(anchor,helper,1)

old='''    if (g_tspf_appearance_mode == 0u)\n    {\n        g_polar_run_c0 = c0;\n        g_polar_run_c1 = c1;\n        g_polar_run_left_real = r->left_real;\n        g_polar_run_right_real = r->right_real;\n        g_polar_run_iq = iq;\n        g_polar_run_step = step;\n        tsp_polar_run_geometry_fast();\n        return;\n    }\n'''
new='''    if (g_tspf_appearance_mode == 0u)\n    {\n        g_polar_progjoin_edges_done = 0u;\n#if TSPF_PROGJOIN_FULL\n        if (profile == TSP_PROFILE_FULL)\n            g_polar_progjoin_edges_done = progjoin_full_edges(out, c0, n, iq, step);\n#endif\n        g_polar_run_c0 = c0;\n        g_polar_run_c1 = c1;\n        g_polar_run_left_real = r->left_real;\n        g_polar_run_right_real = r->right_real;\n        g_polar_run_iq = iq;\n        g_polar_run_step = step;\n        tsp_polar_run_geometry_fast();\n        g_polar_progjoin_edges_done = 0u;\n        return;\n    }\n'''
if old not in r: raise SystemExit('mode0 run anchor missing')
r=r.replace(old,new,1)

old='''        .globl  _g_polar_nt_row_max\n        .globl  _g_map\n'''
new='''        .globl  _g_polar_nt_row_max\n        .globl  _g_polar_progjoin_edges_done\n        .globl  _g_map\n'''
if old not in a: raise SystemExit('assembly global anchor missing')
a=a.replace(old,new,1)

old='''polar_draw_symfull$:\n        call    prepare_symfull_edges$\n        call    draw_plain_interior$\n        jr      raster_done$\n'''
new='''polar_draw_symfull$:\n        ; PROGJOIN_FULL_LIVE_A: compiled top+bottom run-edge programs execute\n        ; once before this surface claims ownership. Preserve all span/interior\n        ; and border semantics, suppressing only the old per-column edge walker.\n        ld      a, (#_g_polar_progjoin_edges_done)\n        or      a\n        jr      nz, polar_symfull_edges_done$\n        call    prepare_symfull_edges$\npolar_symfull_edges_done$:\n        call    draw_plain_interior$\n        jr      raster_done$\n'''
if old not in a: raise SystemExit('assembly FULL edge anchor missing')
a=a.replace(old,new,1)

R.write_text(r); A.write_text(a)
print('PROGJOIN_FULL_LIVE_RUNG_APPLIED')
