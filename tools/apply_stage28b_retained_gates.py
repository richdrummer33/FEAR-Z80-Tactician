#!/usr/bin/env python3
from pathlib import Path

sym_p=Path("src/tilesector_symfull_gg.s")
run_p=Path("src/tilesector_run_gg.s")
sym=sym_p.read_text()
run=run_p.read_text()

if "STAGE28B_RETAINED_FAST_GATES" in sym:
    print("Stage 28B retained fast gates already applied")
    raise SystemExit(0)
if "STAGE25_ACTIVE_MASK" not in sym:
    raise SystemExit("apply Stage 25 active mask first")

sym=sym.replace(
    "; STAGE25_ACTIVE_MASK\n",
    "; STAGE25_ACTIVE_MASK\n; STAGE28B_RETAINED_FAST_GATES\n",1)

# Export a one-byte summary of previous/current retained FULL presence.
anchor="""        ld      (#sf_active_cur$+2), a
"""
repl=anchor+"""        ld      (#_g_ts_ret_full_cur_any), a
"""
if anchor not in sym:
    raise SystemExit("begin active-clear anchor missing")
sym=sym.replace(anchor,repl,1)

anchor="""sf_mark_active_cur$:
        add     a, a
"""
repl="""sf_mark_active_cur$:
        push    af
        ld      a, #1
        ld      (#_g_ts_ret_full_cur_any), a
        pop     af
        add     a, a
"""
if anchor not in sym:
    raise SystemExit("mark-active anchor missing")
sym=sym.replace(anchor,repl,1)

# A frame with neither previous nor current retained FULL state has literally
# nothing for the retained finalizer to do.
anchor="""_ts_retained_full_finalize_unseen::
        push    af
"""
repl="""_ts_retained_full_finalize_unseen::
        push    af
        ld      a, (#_g_ts_ret_full_prev_any)
        or      a
        jp      nz, sf_active_finalize_needed$
        ld      a, (#_g_ts_ret_full_cur_any)
        or      a
        jp      nz, sf_active_finalize_needed$
        pop     af
        ret
sf_active_finalize_needed$:
        pop     af
        push    af
"""
if anchor not in sym:
    raise SystemExit("finalizer anchor missing")
sym=sym.replace(anchor,repl,1)

# Current becomes previous at the end of finalization.
anchor="""        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; A=first column, B=last column. Retire only descriptors belonging to the
"""
repl="""        ld      a, (#_g_ts_ret_full_cur_any)
        ld      (#_g_ts_ret_full_prev_any), a
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; A=first column, B=last column. Retire only descriptors belonging to the
"""
if anchor not in sym:
    raise SystemExit("finalizer tail anchor missing")
sym=sym.replace(anchor,repl,1)

anchor="""sf_active_prev$:          .ds 3
sf_active_cur$:           .ds 3
"""
repl="""sf_active_prev$:          .ds 3
sf_active_cur$:           .ds 3
_g_ts_ret_full_prev_any:: .ds 1
_g_ts_ret_full_cur_any::  .ds 1
"""
if anchor not in sym:
    raise SystemExit("active-mask BSS anchor missing")
sym=sym.replace(anchor,repl,1)

# Depth-0 non-FULL runs used to CALL the retained invalidator even when no
# retained FULL existed. That was poison in raised-geometry/turn-heavy scenes.
run=run.replace(
    "        .globl  _ts_retained_full_invalidate_range\n",
    "        .globl  _ts_retained_full_invalidate_range\n        .globl  _g_ts_ret_full_prev_any\n",1)

anchor="""        ld      a, (#nr_profile$)
        or      a
        jp      z, nr_ret_life_ready$
        ld      a, (#nr_end$)
"""
repl="""        ld      a, (#nr_profile$)
        or      a
        jp      z, nr_ret_life_ready$
        ld      a, (#_g_ts_ret_full_prev_any)
        or      a
        jp      z, nr_ret_life_ready$
        ld      a, (#nr_end$)
"""
if anchor not in run:
    raise SystemExit("non-FULL invalidation anchor missing")
run=run.replace(anchor,repl,1)

sym_p.write_text(sym)
run_p.write_text(run)
print("Applied Stage 28B retained-lifetime hot-path gates.")
