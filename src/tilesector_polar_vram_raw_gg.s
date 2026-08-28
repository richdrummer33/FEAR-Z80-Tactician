        .title  "Polar raw GG name-table upload"
        .module tilesector_polar_vram_raw_gg

        .area   _HOME

        .globl  _g_map
        .globl  _g_prev_map

; Untethered polar uploader used by profiler-hook A/B builds.
; It performs the same compare + VDP writes as the profiled uploader, but has
; deliberately NO dirty-word counter or profiler-visible bookkeeping.
_ts_upload_dirty_map_fast::
        push    ix
        push    bc
        push    de
        push    hl

        ld      hl, #_g_map
        ld      de, #_g_prev_map
        ld      ix, #0x18CC
        ld      b, #18

row_loop$:
        ld      c, #20

col_loop$:
        ld      a, (de)
        cp      (hl)
        jr      nz, changed$
        inc     de
        inc     hl
        ld      a, (de)
        cp      (hl)
        jr      nz, changed_high$
        inc     de
        inc     hl
        jr      word_done$

changed_high$:
        dec     de
        dec     hl

changed$:
        push    bc
        push    ix
        pop     bc
        di
        ld      a, c
        out     (#0xBF), a
        ld      a, b
        or      #0x40
        out     (#0xBF), a

        ld      a, (hl)
        ld      (de), a
        out     (#0xBE), a
        nop
        jr      vdp_delay_done$
vdp_delay_done$:
        inc     hl
        inc     de
        ld      a, (hl)
        ld      (de), a
        out     (#0xBE), a
        ei
        inc     hl
        inc     de
        pop     bc

word_done$:
        inc     ix
        inc     ix
        dec     c
        jr      nz, col_loop$

        push    de
        ld      de, #24
        add     ix, de
        pop     de
        dec     b
        jr      nz, row_loop$

        pop     hl
        pop     de
        pop     bc
        pop     ix
        ret
