        .title  "Polar authoritative name-table state"
        .module tilesector_polar_ntstate_gg
        .area   _HOME

        .globl  _g_map

; Polar carry-over of the mature Stage18 lifetime architecture.
; The authoritative RAM name table stores ONLY visible VDP words.
; Geometry lifetime is tracked separately in two tiny column-major masks:
;   20 columns * 18 row bits = 60 bytes per frame.
; A whole contiguous wall span is marked with three ORs, not a vertical walk.

_tsp_polar_nt_init::
        push    af
        push    bc
        push    de
        push    hl

        ; Static visible base: 9 ceiling rows, 1 horizon row, 8 floor rows.
        ld      hl, #_g_map
        ld      b, #180
        xor     a
nti_ceil$:
        ld      (hl), a
        inc     hl
        ld      (hl), #0
        inc     hl
        djnz    nti_ceil$
        ld      b, #20
        ld      a, #2
nti_horizon$:
        ld      (hl), a
        inc     hl
        ld      (hl), #0
        inc     hl
        djnz    nti_horizon$
        ld      b, #160
        ld      a, #1
nti_floor$:
        ld      (hl), a
        inc     hl
        ld      (hl), #0
        inc     hl
        djnz    nti_floor$

        ; No previous/current geometry at boot.
        xor     a
        ld      hl, #_g_polar_nt_cov_cur
        ld      (hl), a
        ld      de, #_g_polar_nt_cov_cur+1
        ld      bc, #59
        ldir
        ld      hl, #_g_polar_nt_cov_prev
        ld      (hl), a
        ld      de, #_g_polar_nt_cov_prev+1
        ld      bc, #59
        ldir

        ; First visible frame uploads the whole 20x18 name table once.
        ld      hl, #_g_polar_nt_dirty
        ld      b, #18
nti_dirty_rows$:
        ld      (hl), #0xff
        inc     hl
        ld      (hl), #0xff
        inc     hl
        ld      (hl), #0x0f
        inc     hl
        djnz    nti_dirty_rows$

        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; Clear only sixty coverage bytes. The rendered name-table words persist.
_tsp_polar_nt_begin_frame::
        push    af
        push    bc
        push    de
        push    hl
        xor     a
        ld      hl, #_g_polar_nt_cov_cur
        ld      (hl), a
        ld      de, #_g_polar_nt_cov_cur+1
        ld      bc, #59
        ldir
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; Coverage ABI inherited from Stage 14:
;   A = first tile row (0..17)
;   C = last tile row  (0..17)
;   B = screen column  (0..19)
;
; span = prefix[last+1] XOR prefix[first].
; Three bytes are ORed into the current column mask. Runtime cost is constant
; regardless of wall height.
_tsp_polar_nt_mark_span::
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

        ; IY = prefix[first]
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

        ; IX = prefix[last+1]
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

        ; HL = &cov_cur[col*3]
        ld      a, (#ntm_col$)
        ld      e, a
        add     a, a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #_g_polar_nt_cov_cur
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

; Cold compatibility store for the generic portal/special-profile raster.
; ABI: A=row, B=column, DE=visible word, HL=&authoritative name-table word.
; Stage-18 opaque FULL/RAISED paths do not call this routine.
_tsp_polar_nt_store_word::
        push    af
        push    bc
        push    de
        push    hl
        ld      (#nts_row$), a

        ld      a, (hl)
        cp      e
        jr      nz, nts_changed$
        inc     hl
        ld      a, (hl)
        cp      d
        jr      z, nts_done$

nts_changed$:
        pop     hl
        push    hl
        ld      (hl), e
        inc     hl
        ld      (hl), d
        ld      a, (#nts_row$)
        call    _tsp_polar_nt_mark_dirty

nts_done$:
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; A=row 0..17, B=column 0..19. Called only when a visible word changed.
_tsp_polar_nt_mark_dirty::
        push    af
        push    bc
        push    de
        push    hl
        ld      c, a
        ld      a, b
        and     #7
        ld      e, a
        ld      d, #0
        ld      hl, #nt_mask_lut$
        add     hl, de
        ld      a, (hl)
        ld      (#ntd_mask$), a

        ld      a, c
        add     a, a
        add     a, c
        ld      e, a
        ld      a, b
        srl     a
        srl     a
        srl     a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #_g_polar_nt_dirty
        add     hl, de
        ld      a, (#ntd_mask$)
        or      (hl)
        ld      (hl), a

        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; Reconcile only geometry that existed last frame and is absent this frame.
; The unconditional scan is sixty compact bytes, not 360 name-table cells.
_tsp_polar_nt_end_frame::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ld      ix, #_g_polar_nt_cov_prev
        ld      iy, #_g_polar_nt_cov_cur
        xor     a
        ld      (#nte_col$), a

nte_col_loop$:
        xor     a
        ld      (#nte_group$), a

nte_group_loop$:
        ld      a, 0 (iy)
        ld      c, a                    ; C=current coverage byte
        ld      a, 0 (ix)
        ld      b, a                    ; B=previous coverage byte
        ld      a, c
        ld      0 (ix), a               ; previous <- current for next frame
        cpl
        and     b                       ; A = stale bits
        ld      (#nte_stale$), a

        or      a
        jr      z, nte_group_done$

        ; Row base is group*8: 0,8,16.
        ld      a, (#nte_group$)
        add     a, a
        add     a, a
        add     a, a
        ld      (#nte_row$), a
        ld      b, #8
        ld      a, (#nte_group$)
        cp      #2
        jr      nz, nte_bits_ready$
        ld      b, #2
nte_bits_ready$:
        ld      a, (#nte_stale$)
        ld      c, a

nte_bit_loop$:
        srl     c
        jr      nc, nte_next_bit$

        ; Restore static base tile for this exact stale cell.
        ld      a, (#nte_row$)
        ld      (#nte_restore_row$), a
        cp      #9
        jr      c, nte_base_ceil$
        jr      z, nte_base_horizon$
        ld      e, #1
        jr      nte_base_ready$
nte_base_horizon$:
        ld      e, #2
        jr      nte_base_ready$
nte_base_ceil$:
        ld      e, #0
nte_base_ready$:
        ld      d, #0

        ; HL = map row base + column*2.
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

        ; Dirty mark only this stale word.
        ld      d, b                    ; preserve bit-loop count
        ld      a, (#nte_col$)
        ld      b, a
        ld      a, (#nte_restore_row$)
        call    _tsp_polar_nt_mark_dirty
        ld      b, d

nte_next_bit$:
        ld      a, (#nte_row$)
        inc     a
        ld      (#nte_row$), a
        dec     b
        jp      nz, nte_bit_loop$

nte_group_done$:
        inc     ix
        inc     iy
        ld      a, (#nte_group$)
        inc     a
        ld      (#nte_group$), a
        cp      #3
        jp      c, nte_group_loop$

        ld      a, (#nte_col$)
        inc     a
        ld      (#nte_col$), a
        cp      #20
        jp      c, nte_col_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; Polar upload is deliberately a separate module so raw builds can consume the
; same dirty bitset without profiler-visible counter bookkeeping.

        .area _CODE
nt_mask_lut$:
        .db 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80

nt_prefix$:
        .db 0x00,0x00,0x00 ; rows < 0
        .db 0x01,0x00,0x00 ; rows < 1
        .db 0x03,0x00,0x00 ; rows < 2
        .db 0x07,0x00,0x00 ; rows < 3
        .db 0x0f,0x00,0x00 ; rows < 4
        .db 0x1f,0x00,0x00 ; rows < 5
        .db 0x3f,0x00,0x00 ; rows < 6
        .db 0x7f,0x00,0x00 ; rows < 7
        .db 0xff,0x00,0x00 ; rows < 8
        .db 0xff,0x01,0x00 ; rows < 9
        .db 0xff,0x03,0x00 ; rows < 10
        .db 0xff,0x07,0x00 ; rows < 11
        .db 0xff,0x0f,0x00 ; rows < 12
        .db 0xff,0x1f,0x00 ; rows < 13
        .db 0xff,0x3f,0x00 ; rows < 14
        .db 0xff,0x7f,0x00 ; rows < 15
        .db 0xff,0xff,0x00 ; rows < 16
        .db 0xff,0xff,0x01 ; rows < 17
        .db 0xff,0xff,0x03 ; rows < 18

nt_map_rows$:
        .dw _g_map+0,   _g_map+40,  _g_map+80,  _g_map+120, _g_map+160, _g_map+200
        .dw _g_map+240, _g_map+280, _g_map+320, _g_map+360, _g_map+400, _g_map+440
        .dw _g_map+480, _g_map+520, _g_map+560, _g_map+600, _g_map+640, _g_map+680

nt_vdp_rows$:
        .dw 0x18CC,0x190C,0x194C,0x198C,0x19CC,0x1A0C
        .dw 0x1A4C,0x1A8C,0x1ACC,0x1B0C,0x1B4C,0x1B8C
        .dw 0x1BCC,0x1C0C,0x1C4C,0x1C8C,0x1CCC,0x1D0C

        .area _BSS
_g_polar_nt_cov_cur::    .ds 60
_g_polar_nt_cov_prev::   .ds 60
_g_polar_nt_dirty::      .ds 54
ntm_first$:        .ds 1
ntm_after$:        .ds 1
ntm_col$:          .ds 1
nts_row$:          .ds 1
ntd_mask$:         .ds 1
nte_col$:          .ds 1
nte_group$:        .ds 1
nte_stale$:        .ds 1
nte_row$:          .ds 1
nte_restore_row$:  .ds 1
ntu_row$:          .ds 1
ntu_byte$:         .ds 1
ntu_mask$:         .ds 1
