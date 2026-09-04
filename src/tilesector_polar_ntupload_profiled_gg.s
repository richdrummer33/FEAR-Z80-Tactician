        .title  "Polar profiled Stage20 row-extent upload"
        .module tilesector_polar_ntupload_profiled_gg

        .area _HOME
        .globl _g_map
        .globl _g_polar_nt_row_min
        .globl _g_polar_nt_row_max
        .globl _g_ts_dirty_words

; POLAR_STAGE20_ROW_EXTENTS
; Dirty state is already shaped like the VDP transaction: one first/last
; horizontal interval for each visible row. VBlank consumes it directly.
_tsp_polar_nt_upload_dirty::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        xor     a
        ld      (_g_ts_dirty_words), a
        ld      (_g_ts_dirty_words+1), a

        xor     a
        ld      (#pe_row$), a
        ld      ix, #_g_polar_nt_row_min
        ld      iy, #_g_polar_nt_row_max

pe_row_loop$:
        ld      a, 0 (ix)
        cp      #0xff
        jp      z, pe_next_row$
        ld      (#pe_first$), a
        ld      0 (ix), #0xff

        ld      a, 0 (iy)
        ld      (#pe_last$), a
        ld      0 (iy), #0

        ld      c, a
        ld      a, (#pe_first$)
        ld      e, a
        ld      a, c
        sub     e
        inc     a
        ld      (#pe_words$), a

        ld      e, a
        ld      a, (_g_ts_dirty_words)
        add     a, e
        ld      (_g_ts_dirty_words), a
        jr      nc, pe_count_ready$
        ld      a, (_g_ts_dirty_words+1)
        inc     a
        ld      (_g_ts_dirty_words+1), a
pe_count_ready$:

        ; HL = g_map row base + first*2.
        ld      a, (#pe_row$)
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #pe_map_rows$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ex      de, hl
        ld      a, (#pe_first$)
        add     a, a
        ld      e, a
        ld      d, #0
        add     hl, de

        ; DE = visible VDP row base + first*2 in the 0x3800 name table.
        push    hl
        ld      a, (#pe_row$)
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #pe_vdp_rows$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ld      a, (#pe_first$)
        add     a, a
        ld      c, a
        ld      b, #0
        ex      de, hl
        add     hl, bc
        ex      de, hl
        pop     hl

        di
        ld      a, e
        out     (#0xBF), a
        ld      a, d
        or      #0x40
        out     (#0xBF), a
        ld      c, #0xBE
        ld      a, (#pe_words$)
        add     a, a
        ld      b, a
        otir
        ei

pe_next_row$:
        inc     ix
        inc     iy
        ld      a, (#pe_row$)
        inc     a
        ld      (#pe_row$), a
        cp      #18
        jp      c, pe_row_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

        .area _CODE
pe_map_rows$:
        .dw _g_map+0,   _g_map+40,  _g_map+80,  _g_map+120, _g_map+160, _g_map+200
        .dw _g_map+240, _g_map+280, _g_map+320, _g_map+360, _g_map+400, _g_map+440
        .dw _g_map+480, _g_map+520, _g_map+560, _g_map+600, _g_map+640, _g_map+680
pe_vdp_rows$:
        .dw 0x38CC,0x390C,0x394C,0x398C,0x39CC,0x3A0C
        .dw 0x3A4C,0x3A8C,0x3ACC,0x3B0C,0x3B4C,0x3B8C
        .dw 0x3BCC,0x3C0C,0x3C4C,0x3C8C,0x3CCC,0x3D0C

        .area _DATA
pe_row$:   .ds 1
pe_first$: .ds 1
pe_last$:  .ds 1
pe_words$: .ds 1
