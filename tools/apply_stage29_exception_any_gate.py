#!/usr/bin/env python3
from pathlib import Path

p=Path("src/tilesector_ntstate_gg.s")
s=p.read_text()

if "STAGE29_EXCEPTION_ANY_GATE" in s:
    print("Stage 29 exception any-gate already applied")
    raise SystemExit(0)
if "STAGE24_EXCEPTION_COVERAGE_ONLY" not in s:
    raise SystemExit("apply Stage 24 retained lifetime before Stage 29")

s=s.replace(
    "; STAGE24_EXCEPTION_COVERAGE_ONLY\n",
    "; STAGE24_EXCEPTION_COVERAGE_ONLY\n; STAGE29_EXCEPTION_ANY_GATE\n",1)

# Dense Stage18 coverage is cheaper than sparse bookkeeping whenever exceptions
# are actually present. Keep it, but skip its 60-byte clear entirely when the
# previous frame had no exception coverage. Invariant: prev_any==0 implies
# g_nt_cov_cur is already zero (boot, or the prior 1->0 transition cleared it).
a=s.index("_ts_nt_begin_frame::")
b=s.index("; Coverage ABI inherited from Stage 14:",a)
new_begin=r"""_ts_nt_begin_frame::
        push    af
        xor     a
        ld      (#_g_nt_exc_cur_any), a
        ld      a, (#_g_nt_exc_prev_any)
        or      a
        jp      nz, ntb_dense_clear$
        pop     af
        ret

ntb_dense_clear$:
        pop     af
        push    af
        push    bc
        push    de
        push    hl
        xor     a
        ld      hl, #_g_nt_cov_cur
        ld      (hl), a
        ld      de, #_g_nt_cov_cur+1
        ld      bc, #59
        ldir
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

"""
s=s[:a]+new_begin+s[b:]

# One cheap byte records that this frame really used exception coverage. We do
# NOT replace the dense three-byte-per-column mark path: traces show the dense
# B-turn case is exactly one mark per all 20 columns, where sparse first-touch
# machinery is pure overhead.
anchor="""        ld      (#ntm_first$), a
        ld      a, c
"""
repl="""        ld      (#ntm_first$), a
        ld      a, #1
        ld      (#_g_nt_exc_cur_any), a
        ld      a, c
"""
if anchor not in s:
    raise SystemExit("mark-span first-row anchor missing")
s=s.replace(anchor,repl,1)

# Fast exit the dense 20-column/60-byte reconciliation when both adjacent frames
# have no exception coverage. If either side is active, run the proven Stage24
# dense lifetime path unchanged, then publish current-any for the next frame.
a=s.index("_ts_nt_end_frame::")
b=s.index("_ts_nt_upload_dirty::",a)
end=s[a:b]
head="""_ts_nt_end_frame::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ld      ix, #_g_nt_cov_prev
"""
head_repl="""_ts_nt_end_frame::
        push    af
        ld      a, (#_g_nt_exc_prev_any)
        or      a
        jp      nz, nte_dense_enter$
        ld      a, (#_g_nt_exc_cur_any)
        or      a
        jp      nz, nte_dense_enter$
        pop     af
        ret

nte_dense_enter$:
        pop     af
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ld      ix, #_g_nt_cov_prev
"""
if head not in end:
    raise SystemExit("dense end-frame entry anchor missing")
end=end.replace(head,head_repl,1)

tail="""        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

"""
tail_repl="""        ld      a, (#_g_nt_exc_cur_any)
        ld      (#_g_nt_exc_prev_any), a
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

"""
if tail not in end:
    raise SystemExit("dense end-frame tail anchor missing")
end=end.replace(tail,tail_repl,1)
s=s[:a]+end+s[b:]

# Two bytes buy the zero-exception fast path. No per-column sparse metadata.
bss_anchor="""_g_nt_cov_cur::    .ds 60
_g_nt_cov_prev::   .ds 60
"""
bss_repl="""_g_nt_cov_cur::    .ds 60
_g_nt_cov_prev::   .ds 60
_g_nt_exc_prev_any:: .ds 1
_g_nt_exc_cur_any::  .ds 1
"""
if bss_anchor not in s:
    raise SystemExit("coverage BSS anchor missing")
s=s.replace(bss_anchor,bss_repl,1)

# Representation guards.
assert "nte_col_loop$:" in s, "Stage29 must retain dense Stage18 reconciliation"
assert "_g_nt_exc_prev_active" not in s, "Stage29 must not inherit Stage28 sparse masks"
assert "ld      bc, #59\n        ldir" in s, "dense clear disappeared"
assert "jp      nz, nte_dense_enter$" in s

p.write_text(s)
print("Applied Stage 29 dense exception lifetime with zero-exception fast gate.")
