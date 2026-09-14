#!/usr/bin/env python3
"""Second exact-ROM materializer integration rung: painter-style mode 0.

The standalone optimized materializer paints far->near; the current ROM sorts
mode 0 near->far and spends hot-path cycles asking whether each edge/fill row
was unclaimed. This patch tests that architectural difference directly while
retaining the persistent coverage masks for cross-frame lifetime restoration
and retaining change-time dirty tracking.

Mode 0 only:
  * sort far->near (same order as the reference C painter),
  * still mark each surface span into cov_cur,
  * do not early-out when a span was previously covered this frame,
  * make per-row ownership queries always drawable, so nearer surfaces simply
    overwrite farther surfaces in g_map.
"""
from pathlib import Path
import sys

C = Path('src/tilesector_polar_renderer.c')
S = Path('src/tilesector_polar_materialize_gg.s')


def patch_c() -> None:
    t=C.read_text()
    old='''    if (g_tspf_appearance_mode < 2u)\n    {\n        while (i > 0u && g_runs[g_run_order[i - 1u]].inv_mid <= g_runs[idx].inv_mid)'''
    new='''    /* PAINTER_A/B: mode 0 is the geometry-only integration target. Let it\n     * use the reference far->near painter order so the hot materializer can\n     * write without per-row ownership rejection. Mode 1 retains near->far. */\n    if (g_tspf_appearance_mode == 1u)\n    {\n        while (i > 0u && g_runs[g_run_order[i - 1u]].inv_mid <= g_runs[idx].inv_mid)'''
    if old not in t:
        raise RuntimeError('insert_run near-first anchor not found')
    C.write_text(t.replace(old,new,1))


def patch_s() -> None:
    t=S.read_text()
    old='''polar_cov_emit$:\n        call    polar_mark_span_fast$   ; returns A=OR of previously-unclaimed rows\n_tsp_polar_p_span::\n        or      a\n        jp      z, raster_done$         ; nearer geometry already owns whole span\npolar_cov_done$:'''
    new='''polar_cov_emit$:\n        ; PAINTER_A/B: retain coverage solely for cross-frame lifetime. Mode 0\n        ; is sorted far->near, so a nearer surface must be allowed to overwrite\n        ; this span even when a farther surface already marked it.\n        call    polar_mark_span_fast$\n_tsp_polar_p_span::\npolar_cov_done$:'''
    if old not in t:
        raise RuntimeError('span early-out anchor not found')
    t=t.replace(old,new,1)

    start=t.find('polar_row_unclaimed_fast$:\n')
    end=t.find('\n; A=row 0..17, B=column 0..19 -> HL=&g_map',start)
    if start<0 or end<0:
        raise RuntimeError('row ownership helper anchors not found')
    repl='''polar_row_unclaimed_fast$:\n        ; PAINTER_A/B: far->near painter makes every in-range row drawable.\n        ; cov_cur is still maintained by polar_mark_span_fast$ for lifetime.\n        ld      a, #1\n        or      a\n        ret\n'''
    S.write_text(t[:start]+repl+t[end:])


def main() -> int:
    try:
        patch_c(); patch_s()
    except Exception as e:
        print(f'painter patch failed: {e}',file=sys.stderr)
        return 2
    print('INTEGRATED_MATERIALIZER_RUNG=PAINTER_FAR_TO_NEAR_NO_ROW_OWNERSHIP')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
