        .title  "TileSector direct GG name-table upload"
        .module tilesector_vram_gg

        .area   _HOME

        .globl  _g_map
        .globl  _g_prev_map
        .globl  _g_ts_dirty_words

; Game Gear / GBDK fixed geometry used by this demo:
;   default name table = 0x1800
;   visible GG origin  = tile (6,3)
;   visible map        = 20 x 18 words
; Therefore visible row zero begins at 0x1800 + 3*64 + 6*2 = 0x18CC.
;
; This deliberately bypasses generic set_tile_map().  It performs one linear
; compare against the RAM shadow and writes only changed 16-bit name-table
; entries directly to the VDP.  Each changed word gets its own VDP address
; command in this first acceptance experiment: that keeps the routine simple
; and makes the timing result conservative.  A later run-coalesced variant can
; remove those commands if the profiler says they matter.
;
; No arguments: the hot demo buffers are persistent globals.  This also avoids
; ABI/stack traffic entirely.
_ts_upload_dirty_map_fast::
        push    ix
        push    bc
        push    de
        push    hl

        xor     a
        ld      (_g_ts_dirty_words), a
        ld      (_g_ts_dirty_words+1), a

        ld      hl, #_g_map
        ld      de, #_g_prev_map
        ld      ix, #0x18CC
        ld      b, #18

row_loop$:
        ld      c, #20

col_loop$:
        ; Compare the 16-bit name-table word.  On the equal path HL/DE finish
        ; already advanced to the next word.  On a high-byte mismatch they are
        ; rewound so changed$ always starts on the low byte.
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
        ; VDP write command = VRAM address with command bits 01 in bits 15..14.
        ; Keep command + two data bytes atomic against the VBlank ISR so the VDP
        ; internal address latch cannot be stolen halfway through this word.
        di
        ld      a, ixl
        out     (#0xBF), a
        ld      a, ixh
        or      #0x40
        out     (#0xBF), a

        ld      a, (hl)
        ld      (de), a
        out     (#0xBE), a
        ; Match GBDK's conservative GG VDP inter-write delay.
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

        ; 16-bit dirty-word counter (the first invalidated frame can reach 360).
        ld      a, (_g_ts_dirty_words)
        inc     a
        ld      (_g_ts_dirty_words), a
        jr      nz, word_done$
        ld      a, (_g_ts_dirty_words+1)
        inc     a
        ld      (_g_ts_dirty_words+1), a

word_done$:
        ; Next visible tile within this 32-tile hardware name-table row.
        inc     ix
        inc     ix
        dec     c
        jr      nz, col_loop$

        ; We consumed 40 bytes; hardware row stride is 64 bytes. Skip 24.
        ld      a, ixl
        add     #24
        ld      ixl, a
        jr      nc, row_no_carry$
        inc     ixh
row_no_carry$:
        dec     b
        jr      nz, row_loop$

        pop     hl
        pop     de
        pop     bc
        pop     ix
        ret
