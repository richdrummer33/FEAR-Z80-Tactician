#!/usr/bin/env python3
from pathlib import Path

p=Path("src/tilesector_ntstate_gg.s")
s=p.read_text()

if "STAGE30_EXCEPTION_SENTINEL" in s:
    print("Stage 30 exception sentinel already applied")
    raise SystemExit(0)
if "STAGE29_EXCEPTION_ANY_GATE" not in s:
    raise SystemExit("apply Stage 29 exception gate first")

s=s.replace(
    "; STAGE29_EXCEPTION_ANY_GATE\n",
    "; STAGE29_EXCEPTION_ANY_GATE\n; STAGE30_EXCEPTION_SENTINEL\n",1)

# ntm_col$ is already written by every real mark-span call. Reset it to FF at
# begin-frame and it becomes a free current-any sentinel: no per-mark flag store.
old="""_ts_nt_begin_frame::
        push    af
        xor     a
        ld      (#_g_nt_exc_cur_any), a
        ld      a, (#_g_nt_exc_prev_any)
"""
new="""_ts_nt_begin_frame::
        push    af
        ld      a, #0xff
        ld      (#ntm_col$), a
        ld      a, (#_g_nt_exc_prev_any)
"""
if old not in s:
    raise SystemExit("Stage29 begin-frame flag anchor missing")
s=s.replace(old,new,1)

old="""        ld      (#ntm_first$), a
        ld      a, #1
        ld      (#_g_nt_exc_cur_any), a
        ld      a, c
"""
new="""        ld      (#ntm_first$), a
        ld      a, c
"""
if old not in s:
    raise SystemExit("Stage29 per-mark any-store anchor missing")
s=s.replace(old,new,1)

old="""        ld      a, (#_g_nt_exc_prev_any)
        or      a
        jp      nz, nte_dense_enter$
        ld      a, (#_g_nt_exc_cur_any)
        or      a
        jp      nz, nte_dense_enter$
        pop     af
        ret
"""
new="""        ld      a, (#_g_nt_exc_prev_any)
        or      a
        jp      nz, nte_dense_enter$
        ; ntm_col==FF means no mark_span call occurred this frame.
        ld      a, (#ntm_col$)
        inc     a
        jp      nz, nte_dense_enter$
        pop     af
        ret
"""
if old not in s:
    raise SystemExit("Stage29 end-frame current-any gate anchor missing")
s=s.replace(old,new,1)

old="""        ld      a, (#_g_nt_exc_cur_any)
        ld      (#_g_nt_exc_prev_any), a
        pop     iy
"""
new="""        ; Publish current-any from the mark-span sentinel for the next frame.
        ld      a, (#ntm_col$)
        inc     a
        jp      z, nte_dense_publish_none$
        ld      a, #1
        jp      nte_dense_publish_ready$
nte_dense_publish_none$:
        xor     a
nte_dense_publish_ready$:
        ld      (#_g_nt_exc_prev_any), a
        pop     iy
"""
if old not in s:
    raise SystemExit("Stage29 end-frame publish anchor missing")
s=s.replace(old,new,1)

s=s.replace("_g_nt_exc_cur_any::  .ds 1\n","",1)

assert "_g_nt_exc_cur_any" not in s
assert "ld      a, #0xff\n        ld      (#ntm_col$), a" in s
assert "ld      (#ntm_col$), a" in s  # normal mark-span column scratch remains

p.write_text(s)
print("Applied Stage 30 zero-cost current-exception sentinel.")
