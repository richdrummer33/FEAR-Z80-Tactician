#!/usr/bin/env python3
from pathlib import Path

p=Path("src/tilesector_ntstate_gg.s")
s=p.read_text()

if "STAGE28C_OWNERSHIP_GATE" in s:
    print("Stage 28C ownership gate already applied")
    raise SystemExit(0)
if "STAGE28_EXCEPTION_SPARSE_LIFETIME" not in s:
    raise SystemExit("apply Stage 28 sparse exception lifetime first")

s=s.replace(
    "; STAGE28_EXCEPTION_SPARSE_LIFETIME\n",
    "; STAGE28_EXCEPTION_SPARSE_LIFETIME\n; STAGE28C_OWNERSHIP_GATE\n",1)

anchor="""        .globl  _g_ts_dirty_words
        .globl  _ts_retained_full_owns_cell
"""
repl="""        .globl  _g_ts_dirty_words
        .globl  _ts_retained_full_owns_cell
        .globl  _g_ts_ret_full_cur_any
"""
if anchor not in s:
    raise SystemExit("ntstate retained owns glob anchor missing")
s=s.replace(anchor,repl,1)

anchor="""        ld      a, (#nte_col$)
        ld      e, a
        ld      a, (#nte_row$)
        call    _ts_retained_full_owns_cell
        or      a
        jp      nz, nte_exc_bit_next$

        ld      a, (#nte_row$)
"""
repl="""        ; Only pay the retained-FULL ownership query when any retained FULL
        ; exists in the current frame. Turn/raised-only scenes skip it entirely.
        ld      a, (#_g_ts_ret_full_cur_any)
        or      a
        jp      z, nte_exc_not_protected$
        ld      a, (#nte_col$)
        ld      e, a
        ld      a, (#nte_row$)
        call    _ts_retained_full_owns_cell
        or      a
        jp      nz, nte_exc_bit_next$
nte_exc_not_protected$:

        ld      a, (#nte_row$)
"""
if anchor not in s:
    raise SystemExit("stale ownership block missing")
s=s.replace(anchor,repl,1)

p.write_text(s)
print("Applied Stage 28C retained-ownership stale-cell gate.")
