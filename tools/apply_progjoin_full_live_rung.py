#!/usr/bin/env python3
"""Inject the first playable FULL run-edge PROGJOIN rung.

Experimental only. The heavy selector/setup bridge lives in
src/tilesector_polar_progjoin_runtime.c so this splice adds only a call and flag
to the already-near-16KiB renderer translation unit.
"""
from pathlib import Path
R=Path('src/tilesector_polar_renderer.c'); A=Path('src/tilesector_polar_materialize_gg.s')
r=R.read_text(); a=A.read_text()
old='''#ifndef TSPF_SCREEN_DEPTH_PLANE\n#define TSPF_SCREEN_DEPTH_PLANE 0\n#endif\n'''
new=old+'''#ifndef TSPF_PROGJOIN_FULL\n#define TSPF_PROGJOIN_FULL 0\n#endif\n#if defined(__SDCC) && TSPF_PROGJOIN_FULL\n#include "tilesector_polar_progjoin_runtime.h"\n#endif\n'''
if old not in r: raise SystemExit('renderer feature-macro anchor missing')
r=r.replace(old,new,1)
old='''int16_t g_polar_run_iq;\nint16_t g_polar_run_step;\n#endif\n'''
new='''int16_t g_polar_run_iq;\nint16_t g_polar_run_step;\nuint8_t g_polar_progjoin_edges_done;\n#endif\n'''
if old not in r: raise SystemExit('renderer bridge-global anchor missing')
r=r.replace(old,new,1)
old='''    if (g_tspf_appearance_mode == 0u)\n    {\n        g_polar_run_c0 = c0;\n        g_polar_run_c1 = c1;\n        g_polar_run_left_real = r->left_real;\n        g_polar_run_right_real = r->right_real;\n        g_polar_run_iq = iq;\n        g_polar_run_step = step;\n        tsp_polar_run_geometry_fast();\n        return;\n    }\n'''
new='''    if (g_tspf_appearance_mode == 0u)\n    {\n        g_polar_progjoin_edges_done = 0u;\n#if TSPF_PROGJOIN_FULL\n        if (profile == TSP_PROFILE_FULL)\n            g_polar_progjoin_edges_done = tsp_progjoin_try_full_edges(out, c0, n, iq, step);\n#endif\n        g_polar_run_c0 = c0;\n        g_polar_run_c1 = c1;\n        g_polar_run_left_real = r->left_real;\n        g_polar_run_right_real = r->right_real;\n        g_polar_run_iq = iq;\n        g_polar_run_step = step;\n        tsp_polar_run_geometry_fast();\n        g_polar_progjoin_edges_done = 0u;\n        return;\n    }\n'''
if old not in r: raise SystemExit('mode0 run anchor missing')
r=r.replace(old,new,1)
old='''        .globl  _g_polar_nt_row_max\n        .globl  _g_map\n'''
new='''        .globl  _g_polar_nt_row_max\n        .globl  _g_polar_progjoin_edges_done\n        .globl  _g_map\n'''
if old not in a: raise SystemExit('assembly global anchor missing')
a=a.replace(old,new,1)
old='''polar_draw_symfull$:\n        call    prepare_symfull_edges$\n        call    draw_plain_interior$\n        jr      raster_done$\n'''
new='''polar_draw_symfull$:\n        ; PROGJOIN_FULL_LIVE_A: compiled top+bottom run-edge programs execute\n        ; before this surface claims ownership. Suppress only legacy FULL edges.\n        ld      a, (#_g_polar_progjoin_edges_done)\n        or      a\n        jr      nz, polar_symfull_edges_done$\n        call    prepare_symfull_edges$\npolar_symfull_edges_done$:\n        call    draw_plain_interior$\n        jr      raster_done$\n'''
if old not in a: raise SystemExit('assembly FULL edge anchor missing')
a=a.replace(old,new,1)
R.write_text(r); A.write_text(a)
print('PROGJOIN_FULL_LIVE_RUNG_APPLIED')
