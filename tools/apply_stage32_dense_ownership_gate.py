#!/usr/bin/env python3
from pathlib import Path

p=Path("src/tilesector_ntstate_gg.s")
s=p.read_text()

if "STAGE32_DENSE_OWNERSHIP_GATE" in s:
    print("Stage 32 dense ownership gate already applied")
    raise SystemExit(0)
if "STAGE30_EXCEPTION_SENTINEL" not in s:
    raise SystemExit("apply Stage 30 dense exception sentinel first")

s=s.replace(
    "; STAGE30_EXCEPTION_SENTINEL\n",
    "; STAGE30_EXCEPTION_SENTINEL\n; STAGE32_DENSE_OWNERSHIP_GATE\n",1)

anchor="""        .globl  _g_ts_dirty_words
        .globl  _ts_retained_full_owns_cell
"""
repl="""        .globl  _g_ts_dirty_words
        .globl  _ts_retained_full_owns_cell
        .globl  _g_ts_ret_full_cur_any
"""
if anchor not in s:
    raise SystemExit("dense retained ownership glob anchor missing")
s=s.replace(anchor,repl,1)

anchor="""        ; Generic coverage now tracks exceptions only. A stale exception cell
        ; must not erase a CURRENT retained FULL wall occupying the same tile.
        ld      a, (#nte_col$)
        ld      e, a
        ld      a, (#nte_row$)
        call    _ts_retained_full_owns_cell
        or      a
        jp      nz, nte_next_bit$

        ; Restore static base tile for this exact stale exception cell.
"""
repl="""        ; Most stale exception cleanup happens when no retained FULL exists at
        ; all. Avoid the expensive per-cell descriptor/active-mask ownership
        ; query in that case; only pay it when a current retained FULL can
        ; actually protect this cell.
        ld      a, (#_g_ts_ret_full_cur_any)
        or      a
        jp      z, nte_not_retained_protected$
        ld      a, (#nte_col$)
        ld      e, a
        ld      a, (#nte_row$)
        call    _ts_retained_full_owns_cell
        or      a
        jp      nz, nte_next_bit$
nte_not_retained_protected$:

        ; Restore static base tile for this exact stale exception cell.
"""
if anchor not in s:
    raise SystemExit("dense stale ownership block missing")
s=s.replace(anchor,repl,1)

p.write_text(s)
print("Applied Stage 32 dense stale-ownership fast gate.")
