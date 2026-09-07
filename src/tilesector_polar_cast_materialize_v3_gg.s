        .title  "Polar cast-light v3 fast name-table materializer"
        .module tilesector_polar_cast_materialize_v3_gg

        .area   _HOME

        .globl  _g_cast_mat_col
        .globl  _g_cast_mat_row
        .globl  _g_cast_mat_first
        .globl  _g_cast_mat_last
        .globl  _g_cast_mat_word
        .globl  _g_polar_nt_cov_cur
        .globl  _g_polar_nt_row_min
        .globl  _g_polar_nt_row_max
        .globl  _g_map

; Hardware-shaped lighting output.  Normal geometry has already claimed its
; visible cells in g_polar_nt_cov_cur.  Light may write only unclaimed cells;
; an accepted light cell then claims the same bit so receiver order is stable.
;
; The Z80 is not painting pixels.  It is updating the authoritative 20x18 WRAM
; array of two-byte VDP name-table words.  The existing VBlank uploader later
; copies only the dirty row intervals into the VRAM name table at 0x3800.

_tsp_polar_cast_store_word_fast::
        push    bc
        push    de
        push    hl
        ld      a, (#_g_cast_mat_row)
        ld      de, (#_g_cast_mat_word)
        call    cast_store_a_de$
        pop     hl
        pop     de
        pop     bc
        ret

; Fill [first,last] inclusive.  Floor lighting is bounded to rows 10..17, so
; this is at most eight iterations.  Keep the loop row in BSS because the
; inner store deliberately treats BC as scratch.
_tsp_polar_cast_fill_fast::
        push    bc
        push    de
        push    hl
        ld      a, (#_g_cast_mat_first)
        ld      (#cast_fill_row$), a
        ld      c, a
        ld      a, (#_g_cast_mat_last)
        cp      c
        jr      c, cast_fill_done$
cast_fill_loop$:
        ld      a, (#cast_fill_row$)
        ld      de, (#_g_cast_mat_word)
        call    cast_store_a_de$
        ld      a, (#cast_fill_row$)
        ld      c, a
        ld      a, (#_g_cast_mat_last)
        cp      c
        jr      z, cast_fill_done$
        ld      a, c
        inc     a
        ld      (#cast_fill_row$), a
        jr      cast_fill_loop$
cast_fill_done$:
        pop     hl
        pop     de
        pop     bc
        ret

; A=row, DE=final word.  Reject occupied cells, claim accepted cells, write
; only changed words, and expand the row's dirty column interval.
cast_store_a_de$:
        ld      (#cast_row$), a
        ld      (#cast_word$), de

        ; HL = &coverage[col*3 + row/8].
        ld      a, (#_g_cast_mat_col)
        ld      c, a
        add     a, a
        add     a, c
        ld      c, a
        ld      b, #0
        ld      hl, #_g_polar_nt_cov_cur
        add     hl, bc
        ld      a, (#cast_row$)
        srl     a
        srl     a
        srl     a
        ld      c, a
        ld      b, #0
        add     hl, bc

        ; C = row bit.  If already owned, return before map/dirty work.
        ld      a, (#cast_row$)
        and     #7
        ld      c, a
        ld      b, #0
        push    hl
        ld      hl, #cast_mask8$
        add     hl, bc
        ld      c, (hl)
        pop     hl
        ld      a, (hl)
        and     c
        ret     nz
        ld      a, (hl)
        or      c
        ld      (hl), a

        ; HL = &g_map[row*40 + col*2].
        ld      a, (#cast_row$)
        add     a, a
        ld      c, a
        ld      b, #0
        ld      hl, #cast_map_rows$
        add     hl, bc
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ex      de, hl
        ld      a, (#_g_cast_mat_col)
        add     a, a
        ld      c, a
        ld      b, #0
        add     hl, bc

        ld      de, (#cast_word$)
        ld      a, (hl)
        cp      e
        jr      nz, cast_changed$
        inc     hl
        ld      a, (hl)
        cp      d
        ret     z
        dec     hl
cast_changed$:
        ld      (hl), e
        inc     hl
        ld      (hl), d

        ; Dirty min[row].  0xff means this row was previously clean.
        ld      a, (#cast_row$)
        ld      c, a
        ld      b, #0
        ld      hl, #_g_polar_nt_row_min
        add     hl, bc
        ld      a, (hl)
        cp      #0xff
        jr      z, cast_set_min$
        ld      a, (#_g_cast_mat_col)
        cp      (hl)
        jr      nc, cast_min_done$
cast_set_min$:
        ld      a, (#_g_cast_mat_col)
        ld      (hl), a
cast_min_done$:
        ; Dirty max[row].
        ld      a, (#cast_row$)
        ld      c, a
        ld      b, #0
        ld      hl, #_g_polar_nt_row_max
        add     hl, bc
        ld      a, (#_g_cast_mat_col)
        cp      (hl)
        ret     c
        ret     z
        ld      (hl), a
        ret

        .area _CODE
cast_mask8$:
        .db 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80
cast_map_rows$:
        .dw _g_map+0,   _g_map+40,  _g_map+80,  _g_map+120, _g_map+160, _g_map+200
        .dw _g_map+240, _g_map+280, _g_map+320, _g_map+360, _g_map+400, _g_map+440
        .dw _g_map+480, _g_map+520, _g_map+560, _g_map+600, _g_map+640, _g_map+680

        .area _BSS
cast_row$:      .ds 1
cast_fill_row$: .ds 1
cast_word$:     .ds 2
