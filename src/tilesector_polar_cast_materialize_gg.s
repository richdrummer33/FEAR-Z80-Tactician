        .title  "Polar cast-light fast name-table materializer"
        .module tilesector_polar_cast_materialize_gg

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

; Lighting runs after ordinary near->far geometry.  A lighting store is legal
; only when the geometry coverage bit for that screen tile is still clear.
; Accepted light cells then claim the bit themselves so the second receiver
; cannot overwrite the first one.  This is the same authoritative-name-table
; lifetime rule as the mature Polar wall materializer, but with a tiny ABI.
;
; Bridge globals:
;   g_cast_mat_col   0..19
;   g_cast_mat_row   0..17
;   g_cast_mat_first first row for a solid vertical lit span
;   g_cast_mat_last  last row for a solid vertical lit span
;   g_cast_mat_word  final two-byte GG name-table word
;
; No pixel framebuffer exists.  These routines modify the 20x18 WRAM name
; table and dirty-row extents; the existing VBlank uploader copies changed
; descriptors into VDP VRAM at the 0x3800 name table.

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

; Fill [first,last] inclusive with one word.  The common floor case is only
; rows 10..17, so this loop is bounded to at most eight iterations.
_tsp_polar_cast_fill_fast::
        push    bc
        push    de
        push    hl

        ld      a, (#_g_cast_mat_first)
        ld      b, a
        ld      a, (#_g_cast_mat_last)
        cp      b
        jr      c, cast_fill_done$

cast_fill_loop$:
        ld      a, b
        ld      de, (#_g_cast_mat_word)
        call    cast_store_a_de$
        ld      a, (#_g_cast_mat_last)
        cp      b
        jr      z, cast_fill_done$
        inc     b
        jr      cast_fill_loop$

cast_fill_done$:
        pop     hl
        pop     de
        pop     bc
        ret

; A=row, DE=final name-table word.  BC/HL are scratch.  Returns with registers
; unspecified.  If a nearer geometry/light owner already claimed the cell,
; nothing is written and no dirty interval is expanded.
cast_store_a_de$:
        ld      (#cast_row$), a
        ld      (#cast_word$), de

        ; HL = &coverage[col*3 + row/8].
        ld      a, (#_g_cast_mat_col)
        ld      c, a
        add     a, a
        add     a, c                     ; col*3
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

        ; C = one-bit row mask.  Reject an already-owned tile.
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

        ; HL = &g_map[row*40 + col*2] through a tiny row-base table.
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

        ; Dirty min[row] = min(current,col), with 0xff meaning clean.
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
        ; row_max is 18 bytes immediately after row_min in the public ABI, but
        ; use its symbol explicitly so layout changes remain harmless.
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
cast_row$:  .ds 1
cast_word$: .ds 2
