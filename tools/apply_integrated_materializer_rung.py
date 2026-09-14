#!/usr/bin/env python3
"""Patch the playable GG materializer with the first proven run-level rung.

This is deliberately an integration experiment, not a new benchmark model.
It modifies the actual source that is assembled into the .gg ROM.

Exact transformations from the Z80 materializer ladder:
  1. CARRY_EDGE_A analogue: the previous column's right inverse-depth and
     profile endpoints become the next column's left values.  Therefore only
     one Q6 decode and one profile_half call are required per additional
     coarse column.
  2. BORDERHOIST analogue: compute the c0 border once before the loop; interior
     columns get border=0, and only c1 may acquire the right border.

The existing surface-column emitter, coverage/lifetime machinery, edge LUT,
name-table storage and dirty tracking are untouched.  That makes this a clean
A/B of integration overhead versus the current ROM.
"""
from pathlib import Path
import sys

P = Path("src/tilesector_polar_materialize_gg.s")
START = "_tsp_polar_run_geometry_fast::\n"
END = "; HL = signed Q6-ish accumulator + rounding bias. Return the exact C\n"

NEW = r'''_tsp_polar_run_geometry_fast::
        push    bc
        push    de
        push    hl

        ld      a, (#_g_polar_run_c0)
        ld      (#r_run_col$), a
        ld      a, #1
        ld      (#_g_polar_mat_shade), a

        ; CARRY_EDGE_A: solve the left endpoint once for the whole run.
        ld      hl, (#_g_polar_run_iq)
        ld      de, #32
        add     hl, de
        call    q6_round_u8$
        ld      (#r_run_invl$), a
        srl     a
        call    profile_half$
        ld      (#_g_polar_mat_top_l), hl
        ld      (#_g_polar_mat_bot_l), de

        ; BORDERHOIST: c0 is the only column that can carry a left border.
        xor     a
        ld      (#_g_polar_mat_border), a
        ld      a, (#_g_polar_run_left_real)
        or      a
        jr      z, run_opt_c0_right$
        ld      a, #1
        ld      (#_g_polar_mat_border), a
run_opt_c0_right$:
        ; A one-column run can carry both physical borders.
        ld      a, (#_g_polar_run_c0)
        ld      c, a
        ld      a, (#_g_polar_run_c1)
        cp      c
        jr      nz, run_opt_loop$
        ld      a, (#_g_polar_run_right_real)
        or      a
        jr      z, run_opt_loop$
        ld      a, (#_g_polar_mat_border)
        or      #2
        ld      (#_g_polar_mat_border), a

run_opt_loop$:
        ; Only the right endpoint is new.  It becomes next column's left.
        ld      hl, (#_g_polar_run_iq)
        ld      de, (#_g_polar_run_step)
        add     hl, de
        ld      de, #32
        add     hl, de
        call    q6_round_u8$
        ld      (#r_run_invr$), a
        srl     a
        call    profile_half$
        ld      (#_g_polar_mat_top_r), hl
        ld      (#_g_polar_mat_bot_r), de

        ld      a, (#r_run_col$)
        ld      (#_g_polar_mat_col), a
        call    _tsp_polar_surface_column_fast

        ; Last column is complete.  No need to advance state beyond the run.
        ld      a, (#r_run_col$)
        ld      c, a
        ld      a, (#_g_polar_run_c1)
        cp      c
        jp      z, run_opt_done$

        ; Carry exact endpoint state into the next column.
        ld      a, (#r_run_invr$)
        ld      (#r_run_invl$), a
        ld      hl, (#_g_polar_mat_top_r)
        ld      (#_g_polar_mat_top_l), hl
        ld      hl, (#_g_polar_mat_bot_r)
        ld      (#_g_polar_mat_bot_l), hl

        ld      hl, (#_g_polar_run_iq)
        ld      de, (#_g_polar_run_step)
        add     hl, de
        ld      (#_g_polar_run_iq), hl

        ld      a, (#r_run_col$)
        inc     a
        ld      (#r_run_col$), a

        ; Interior columns carry no border.  Only the new c1 can gain bit 1.
        xor     a
        ld      (#_g_polar_mat_border), a
        ld      a, (#_g_polar_run_c1)
        ld      c, a
        ld      a, (#r_run_col$)
        cp      c
        jr      nz, run_opt_loop$
        ld      a, (#_g_polar_run_right_real)
        or      a
        jr      z, run_opt_loop$
        ld      a, #2
        ld      (#_g_polar_mat_border), a
        jp      run_opt_loop$

run_opt_done$:
        pop     hl
        pop     de
        pop     bc
        ret

'''


def main() -> int:
    text = P.read_text()
    a = text.find(START)
    b = text.find(END, a)
    if a < 0 or b < 0:
        print("materializer patch anchors not found", file=sys.stderr)
        return 2
    old = text[a:b]
    if "run_geom_loop$" not in old or "call    _tsp_polar_surface_column_fast" not in old:
        print("unexpected materializer source shape", file=sys.stderr)
        return 3
    out = text[:a] + NEW + text[b:]
    P.write_text(out)
    print(f"patched {P}: run walker {len(old.splitlines())} -> {len(NEW.splitlines())} lines")
    print("INTEGRATED_MATERIALIZER_RUNG=CARRY_EDGE_A+BORDERHOIST")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
