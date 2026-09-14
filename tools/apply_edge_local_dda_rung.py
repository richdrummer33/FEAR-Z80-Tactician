#!/usr/bin/env python3
"""Port the DDA_G local-coordinate carry into the actual GG materializer.

The current ROM already computes edge row extents once per column, but each
edge row still rebuilds `local = left_y - row*8` with three doublings.  DDA_G
showed that this is an affine walk: compute local once at the first row, then
subtract 8 for each following row.

This patch applies that exact recurrence to both the generic top/bottom edge
walker and the FULL symmetric edge walker.  No ordering, coverage, LUT, dirty,
or store semantics change.
"""
from pathlib import Path
import sys

P = Path('src/tilesector_polar_materialize_gg.s')


def replace1(t: str, old: str, new: str, name: str) -> str:
    if old not in t:
        raise RuntimeError(f'{name} anchor not found')
    return t.replace(old, new, 1)


def main() -> int:
    try:
        t=P.read_text()

        old='''sym_slope_store$:\n        ld      (#r_edge_slope$), a\n        ld      a, (#r_top_min$)\nsym_edge_rows_loop$:\n        ld      (#r_edge_iter$), a\n        call    draw_symfull_edge_pair$\n        ld      a, (#r_edge_iter$)\n        ld      c, a\n        ld      a, (#r_top_max$)\n        cp      c\n        ret     z\n        ld      a, c\n        inc     a\n        jr      sym_edge_rows_loop$'''
        new='''sym_slope_store$:\n        ld      (#r_edge_slope$), a\n        ; DDA_LOCAL: local = left_y - first_row*8, once per edge.\n        ld      a, (#r_top_min$)\n        ld      c, a\n        add     a, a\n        add     a, a\n        add     a, a\n        ld      e, a\n        ld      a, (#r_edge_left$)\n        sub     e\n        ld      (#r_edge_local$), a\n        ld      a, c\nsym_edge_rows_loop$:\n        ld      (#r_edge_iter$), a\n        call    draw_symfull_edge_pair$\n        ld      a, (#r_edge_iter$)\n        ld      c, a\n        ld      a, (#r_top_max$)\n        cp      c\n        ret     z\n        ld      a, (#r_edge_local$)\n        sub     #8\n        ld      (#r_edge_local$), a\n        ld      a, c\n        inc     a\n        jr      sym_edge_rows_loop$'''
        t=replace1(t,old,new,'sym loop')

        old='''        ; Top local coordinate and canonical top-edge LUT index.\n        ld      a, (#r_row$)\n        add     a, a\n        add     a, a\n        add     a, a\n        ld      e, a\n        ld      a, (#r_edge_left$)\n        sub     e\n        cp      #0x80'''
        new='''        ; DDA_LOCAL: caller carries left_y-row*8 down the edge rows.\n        ld      a, (#r_edge_local$)\n        cp      #0x80'''
        t=replace1(t,old,new,'sym local')

        old='''slope_store$:\n        ld      (#r_edge_slope$), a\n        ; Polar path may cross more than two tile rows at steep/near\n        ; perspective. Match the C oracle: draw every row from min..max while\n        ; using the clamped [-7,+7] edge slope for tile selection.\n        ld      a, (#r_edge_min$)\nedge_rows_loop$:\n        ld      (#r_edge_iter$), a\n        call    draw_edge_row$\n        ld      a, (#r_edge_iter$)\n        ld      c, a\n        ld      a, (#r_edge_max$)\n        cp      c\n        ret     z\n        ld      a, c\n        inc     a\n        jr      edge_rows_loop$'''
        new='''slope_store$:\n        ld      (#r_edge_slope$), a\n        ; DDA_LOCAL: local = left_y - first_row*8, once per edge.\n        ld      a, (#r_edge_min$)\n        ld      c, a\n        add     a, a\n        add     a, a\n        add     a, a\n        ld      e, a\n        ld      a, (#r_edge_left$)\n        sub     e\n        ld      (#r_edge_local$), a\n        ; Polar path may cross more than two tile rows at steep/near\n        ; perspective. Match the C oracle: draw every row from min..max while\n        ; using the clamped [-7,+7] edge slope for tile selection.\n        ld      a, c\nedge_rows_loop$:\n        ld      (#r_edge_iter$), a\n        call    draw_edge_row$\n        ld      a, (#r_edge_iter$)\n        ld      c, a\n        ld      a, (#r_edge_max$)\n        cp      c\n        ret     z\n        ld      a, (#r_edge_local$)\n        sub     #8\n        ld      (#r_edge_local$), a\n        ld      a, c\n        inc     a\n        jr      edge_rows_loop$'''
        t=replace1(t,old,new,'generic loop')

        old='''        ; local = left_y - row*8; low-byte arithmetic is exact in this range.\n        ld      a, c\n        add     a, a\n        add     a, a\n        add     a, a\n        ld      e, a\n        ld      a, (#r_edge_left$)\n        sub     e\n        ; Conservative clamp into LUT local domain [-15,+15].'''
        new='''        ; DDA_LOCAL: caller carries left_y-row*8 down the edge rows.\n        ld      a, (#r_edge_local$)\n        ; Conservative clamp into LUT local domain [-15,+15].'''
        t=replace1(t,old,new,'generic local')

        old='''r_edge_iter$:\n        .ds     1\nr_sym_bottom_row$:'''
        new='''r_edge_iter$:\n        .ds     1\nr_edge_local$:\n        .ds     1\nr_sym_bottom_row$:'''
        t=replace1(t,old,new,'scratch')

        P.write_text(t)
    except Exception as e:
        print(f'DDA local patch failed: {e}',file=sys.stderr)
        return 2
    print('INTEGRATED_MATERIALIZER_RUNG=DDA_EDGE_LOCAL_CARRY')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
