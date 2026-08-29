        .title  "Polar diagnostic full visible name-table upload"
        .module tilesector_polar_ntupload_full_gg

        .area _HOME
        .globl _g_map

; Diagnostic only.
; Ignore every dirty-region optimization and send all 20 visible words of all
; 18 visible rows every frame. This intentionally proves/disproves whether the
; normal row-extent/dirty path is responsible for temporal edge ghosts.
_tsp_polar_nt_upload_full::
        push    af
        push    bc
        push    de
        push    hl
        push    ix

        ld      ix, #full_vdp_rows$
        xor     a
        ld      (#full_row$), a

full_row_loop$:
        ; HL = &g_map[row * 40 bytes]
        ld      a, (#full_row$)
        ld      l, a
        ld      h, #0
        add     hl, hl                  ; *2
        ld      e, l
        ld      d, h
        add     hl, hl                  ; *4
        add     hl, hl                  ; *8
        add     hl, hl                  ; *16
        add     hl, hl                  ; *32
        add     hl, de                  ; *34
        add     hl, de                  ; *36
        add     hl, de                  ; *38
        add     hl, de                  ; *40
        ld      de, #_g_map
        add     hl, de

        ; DE = visible VDP row base.
        push    hl
        ld      e, 0 (ix)
        ld      d, 1 (ix)
        pop     hl

        di
        ld      a, e
        out     (#0xBF), a
        ld      a, d
        or      #0x40
        out     (#0xBF), a
        ld      c, #0xBE
        ld      b, #40                  ; 20 words * 2 bytes
        otir
        ei

        inc     ix
        inc     ix
        ld      a, (#full_row$)
        inc     a
        ld      (#full_row$), a
        cp      #18
        jp      c, full_row_loop$

        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

        .area _CODE
full_vdp_rows$:
        .dw 0x18CC,0x190C,0x194C,0x198C,0x19CC,0x1A0C
        .dw 0x1A4C,0x1A8C,0x1ACC,0x1B0C,0x1B4C,0x1B8C
        .dw 0x1BCC,0x1C0C,0x1C4C,0x1C8C,0x1CCC,0x1D0C

        .area _DATA
full_row$: .ds 1
