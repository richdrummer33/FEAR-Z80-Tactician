#!/usr/bin/env python3
from pathlib import Path

p = Path("src/tilesector_ntstate_gg.s")
s = p.read_text()

if "STAGE28_EXCEPTION_SPARSE_LIFETIME" in s:
    print("Stage 28 sparse exception lifetime already applied")
    raise SystemExit(0)
if "STAGE24_EXCEPTION_COVERAGE_ONLY" not in s:
    raise SystemExit("apply Stage 24 retained lifetime before Stage 28")
if "STAGE20_ROW_EXTENTS" not in s:
    raise SystemExit("Stage 20 row extents missing")

s = s.replace(
    "; STAGE24_EXCEPTION_COVERAGE_ONLY\n",
    "; STAGE24_EXCEPTION_COVERAGE_ONLY\n; STAGE28_EXCEPTION_SPARSE_LIFETIME\n",
    1,
)

a = s.index("_ts_nt_begin_frame::")
b = s.index("; Coverage ABI inherited from Stage 14:", a)
new_begin = r"""_ts_nt_begin_frame::
        push    af
        push    hl
        xor     a
        ld      hl, #_g_nt_exc_cur_active
        ld      (hl), a
        inc     hl
        ld      (hl), a
        inc     hl
        ld      (hl), a
        pop     hl
        pop     af
        ret

; A=column 0..19. Return E=active-mask byte index and C=bit mask.
nte_exc_slot$:
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #nte_exc_mask_lut$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      c, (hl)
        ret

nte_exc_mask_lut$:
        .db 0,0x01, 0,0x02, 0,0x04, 0,0x08
        .db 0,0x10, 0,0x20, 0,0x40, 0,0x80
        .db 1,0x01, 1,0x02, 1,0x04, 1,0x08
        .db 1,0x10, 1,0x20, 1,0x40, 1,0x80
        .db 2,0x01, 2,0x02, 2,0x04, 2,0x08

"""
s = s[:a] + new_begin + s[b:]

a = s.index("_ts_nt_mark_span::")
b = s.index("; Cold compatibility store", a)
new_mark = r"""_ts_nt_mark_span::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ld      (#ntm_first$), a
        ld      a, c
        inc     a
        ld      (#ntm_after$), a
        ld      a, b
        ld      (#ntm_col$), a

        ; First exception mark for this column in the frame?
        ld      a, (#ntm_col$)
        call    nte_exc_slot$
        ld      d, #0
        ld      hl, #_g_nt_exc_cur_active
        add     hl, de
        ld      a, (hl)
        and     c
        jp      nz, ntm_exc_active_ready$

        ; Mark active and lazily clear only this column's current 18-bit mask.
        ld      a, (hl)
        or      c
        ld      (hl), a

        ld      a, (#ntm_col$)
        ld      e, a
        add     a, a
        add     a, e
        ld      e, a
        ld      d, #0
        ld      hl, #_g_nt_cov_cur
        add     hl, de
        xor     a
        ld      (hl), a
        inc     hl
        ld      (hl), a
        inc     hl
        ld      (hl), a

ntm_exc_active_ready$:
        ld      a, (#ntm_first$)
        ld      e, a
        add     a, a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #nt_prefix$
        add     hl, de
        push    hl
        pop     iy

        ld      a, (#ntm_after$)
        ld      e, a
        add     a, a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #nt_prefix$
        add     hl, de
        push    hl
        pop     ix

        ld      a, (#ntm_col$)
        ld      e, a
        add     a, a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #_g_nt_cov_cur
        add     hl, de

        ld      a, 0 (ix)
        xor     0 (iy)
        or      (hl)
        ld      (hl), a
        inc     hl
        ld      a, 1 (ix)
        xor     1 (iy)
        or      (hl)
        ld      (hl), a
        inc     hl
        ld      a, 2 (ix)
        xor     2 (iy)
        or      (hl)
        ld      (hl), a

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

"""
s = s[:a] + new_mark + s[b:]

a = s.index("_ts_nt_end_frame::")
b = s.index("_ts_nt_upload_dirty::", a)
new_end = r"""_ts_nt_end_frame::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        xor     a
        ld      (#nte_exc_group$), a
        ld      (#nte_exc_base$), a

nte_exc_group_loop$:
        ld      a, (#nte_exc_group$)
        ld      e, a
        ld      d, #0
        ld      hl, #_g_nt_exc_prev_active
        add     hl, de
        ld      a, (hl)
        ld      (#nte_exc_prev_mask$), a

        ld      a, (#nte_exc_group$)
        ld      e, a
        ld      d, #0
        ld      hl, #_g_nt_exc_cur_active
        add     hl, de
        ld      a, (hl)
        ld      (#nte_exc_cur_mask$), a
        ld      c, a
        ld      a, (#nte_exc_prev_mask$)
        or      c
        ld      (#nte_exc_union$), a
        jp      z, nte_exc_group_commit$

        ld      a, #1
        ld      (#nte_exc_bitmask$), a
        ld      a, (#nte_exc_base$)
        ld      (#nte_col$), a

nte_exc_col_loop$:
        ld      a, (#nte_exc_union$)
        srl     a
        ld      (#nte_exc_union$), a
        jp      nc, nte_exc_col_next$

        ld      a, (#nte_exc_bitmask$)
        ld      c, a
        ld      a, (#nte_exc_prev_mask$)
        and     c
        ld      (#nte_exc_prev_on$), a
        ld      a, (#nte_exc_cur_mask$)
        and     c
        ld      (#nte_exc_cur_on$), a

        ld      a, (#nte_col$)
        ld      e, a
        add     a, a
        add     a, e
        ld      e, a
        ld      d, #0
        ld      hl, #_g_nt_cov_prev
        add     hl, de
        push    hl
        pop     ix
        ld      hl, #_g_nt_cov_cur
        add     hl, de
        push    hl
        pop     iy

        xor     a
        ld      (#nte_exc_cov_group$), a

nte_exc_cov_loop$:
        ld      a, (#nte_exc_cur_on$)
        or      a
        jp      z, nte_exc_cur_zero$
        ld      c, 0 (iy)
        jp      nte_exc_cur_ready$
nte_exc_cur_zero$:
        ld      c, #0
nte_exc_cur_ready$:

        ld      a, (#nte_exc_prev_on$)
        or      a
        jp      z, nte_exc_prev_zero$
        ld      d, 0 (ix)
        jp      nte_exc_prev_ready$
nte_exc_prev_zero$:
        ld      d, #0
nte_exc_prev_ready$:

        ld      0 (ix), c
        ld      a, c
        cpl
        and     d
        ld      (#nte_stale$), a
        or      a
        jp      z, nte_exc_cov_next$

        ld      a, (#nte_exc_cov_group$)
        add     a, a
        add     a, a
        add     a, a
        ld      (#nte_row$), a
        ld      a, (#nte_stale$)
        ld      c, a
        ld      b, #8
        ld      a, (#nte_exc_cov_group$)
        cp      #2
        jp      nz, nte_exc_bits_ready$
        ld      b, #2
nte_exc_bits_ready$:

nte_exc_bit_loop$:
        srl     c
        jp      nc, nte_exc_bit_next$

        ld      a, (#nte_col$)
        ld      e, a
        ld      a, (#nte_row$)
        call    _ts_retained_full_owns_cell
        or      a
        jp      nz, nte_exc_bit_next$

        ld      a, (#nte_row$)
        ld      (#nte_restore_row$), a
        cp      #9
        jp      c, nte_exc_base_ceil$
        jp      z, nte_exc_base_horizon$
        ld      e, #1
        jp      nte_exc_base_ready$
nte_exc_base_horizon$:
        ld      e, #2
        jp      nte_exc_base_ready$
nte_exc_base_ceil$:
        ld      e, #0
nte_exc_base_ready$:
        ld      d, #0

        ld      a, (#nte_restore_row$)
        add     a, a
        ld      l, a
        ld      h, #0
        push    bc
        ld      bc, #nt_map_rows$
        add     hl, bc
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a
        ld      a, (#nte_col$)
        add     a, a
        ld      c, a
        ld      b, #0
        add     hl, bc
        pop     bc

        ld      (hl), e
        inc     hl
        ld      (hl), d

        ld      d, b
        ld      a, (#nte_col$)
        ld      b, a
        ld      a, (#nte_restore_row$)
        call    _ts_nt_mark_dirty
        ld      b, d

nte_exc_bit_next$:
        ld      a, (#nte_row$)
        inc     a
        ld      (#nte_row$), a
        dec     b
        jp      nz, nte_exc_bit_loop$

nte_exc_cov_next$:
        inc     ix
        inc     iy
        ld      a, (#nte_exc_cov_group$)
        inc     a
        ld      (#nte_exc_cov_group$), a
        cp      #3
        jp      c, nte_exc_cov_loop$

nte_exc_col_next$:
        ld      a, (#nte_exc_bitmask$)
        add     a, a
        ld      (#nte_exc_bitmask$), a
        ld      a, (#nte_col$)
        inc     a
        ld      (#nte_col$), a
        ld      a, (#nte_exc_union$)
        or      a
        jp      nz, nte_exc_col_loop$

nte_exc_group_commit$:
        ld      a, (#nte_exc_group$)
        ld      e, a
        ld      d, #0
        ld      hl, #_g_nt_exc_prev_active
        add     hl, de
        ld      a, (#nte_exc_cur_mask$)
        ld      (hl), a

        ld      a, (#nte_exc_base$)
        add     a, #8
        ld      (#nte_exc_base$), a
        ld      a, (#nte_exc_group$)
        inc     a
        ld      (#nte_exc_group$), a
        cp      #3
        jp      c, nte_exc_group_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; Upload routine follows unchanged.
"""
s = s[:a] + new_end + s[b:]

marker = "        .area   _BSS\n"
if marker not in s:
    raise SystemExit("ntstate BSS marker not found")
bss = r"""        .area _BSS
_g_nt_exc_prev_active:: .ds 3
_g_nt_exc_cur_active::  .ds 3
nte_exc_group$:         .ds 1
nte_exc_base$:          .ds 1
nte_exc_prev_mask$:     .ds 1
nte_exc_cur_mask$:      .ds 1
nte_exc_union$:         .ds 1
nte_exc_bitmask$:       .ds 1
nte_exc_prev_on$:       .ds 1
nte_exc_cur_on$:        .ds 1
nte_exc_cov_group$:     .ds 1
"""
s = s.replace(marker, bss, 1)

p.write_text(s)
print("Applied Stage 28 sparse exception-only lifetime runtime.")
