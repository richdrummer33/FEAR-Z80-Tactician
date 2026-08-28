        .title  "Polar profiled dirty-bit VDP upload"
        .module tilesector_polar_ntupload_profiled_gg
        .area _HOME

        .globl _g_map
        .globl _g_polar_nt_dirty
        .globl _g_ts_dirty_words

; Scan the existing 54-byte row-major dirty bitset and upload only changed
; name-table words. No RAM-private metadata exists, so high bytes go straight
; to the VDP.
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
        ld      hl, #_g_polar_nt_dirty
        xor     a
        ld      (#pntu_row$), a

ntu_row_loop$:
        xor     a
        ld      (#pntu_byte$), a
ntu_byte_loop$:
        ld      a, (hl)
        ld      (#pntu_mask$), a
        ld      (hl), #0
        or      a
        jp      z, ntu_byte_advance$

        ld      a, (#pntu_row$)
        add     a, a
        ld      c, a
        ld      b, #0
        push    hl
        ld      hl, #pntu_map_rows$
        add     hl, bc
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        push    de
        pop     iy
        pop     hl

        ld      a, (#pntu_row$)
        add     a, a
        ld      c, a
        ld      b, #0
        push    hl
        ld      hl, #pntu_vdp_rows$
        add     hl, bc
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        pop     hl
        push    de
        pop     ix

        ld      a, (#pntu_byte$)
        or      a
        jr      z, ntu_group_ready$
        dec     a
        jr      z, ntu_group16$
        ld      bc, #32
        add     iy, bc
        add     ix, bc
        jr      ntu_group_ready$
ntu_group16$:
        ld      bc, #16
        add     iy, bc
        add     ix, bc
ntu_group_ready$:
        ld      a, (#pntu_byte$)
        cp      #2
        ld      b, #8
        jr      nz, ntu_bits_ready$
        ld      b, #4
ntu_bits_ready$:
        ld      a, (#pntu_mask$)
        ld      c, a

ntu_bit_loop$:
        srl     c
        jr      nc, ntu_skip_word$
        push    bc
        push    ix
        pop     bc
        di
        ld      a, c
        out     (#0xBF), a
        ld      a, b
        or      #0x40
        out     (#0xBF), a
        ld      a, 0 (iy)
        out     (#0xBE), a
        nop
        ld      a, 1 (iy)
        out     (#0xBE), a
        ei
        pop     bc

        ld      a, (_g_ts_dirty_words)
        inc     a
        ld      (_g_ts_dirty_words), a
        jr      nz, ntu_skip_word$
        ld      a, (_g_ts_dirty_words+1)
        inc     a
        ld      (_g_ts_dirty_words+1), a

ntu_skip_word$:
        inc     iy
        inc     iy
        inc     ix
        inc     ix
        djnz    ntu_bit_loop$

ntu_byte_advance$:
        inc     hl
        ld      a, (#pntu_byte$)
        inc     a
        ld      (#pntu_byte$), a
        cp      #3
        jp      c, ntu_byte_loop$

        ld      a, (#pntu_row$)
        inc     a
        ld      (#pntu_row$), a
        cp      #18
        jp      c, ntu_row_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret


        .area _CODE
pntu_map_rows$:
        .dw _g_map+0,   _g_map+40,  _g_map+80,  _g_map+120, _g_map+160, _g_map+200
        .dw _g_map+240, _g_map+280, _g_map+320, _g_map+360, _g_map+400, _g_map+440
        .dw _g_map+480, _g_map+520, _g_map+560, _g_map+600, _g_map+640, _g_map+680

pntu_vdp_rows$:
        .dw 0x18CC,0x190C,0x194C,0x198C,0x19CC,0x1A0C
        .dw 0x1A4C,0x1A8C,0x1ACC,0x1B0C,0x1B4C,0x1B8C
        .dw 0x1BCC,0x1C0C,0x1C4C,0x1C8C,0x1CCC,0x1D0C


        .area _BSS
pntu_row$:         .ds 1
pntu_byte$:        .ds 1
pntu_mask$:        .ds 1
