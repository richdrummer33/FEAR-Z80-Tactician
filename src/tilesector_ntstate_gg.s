        .title  "TileSector authoritative name-table state"
        .module tilesector_ntstate_gg

        .area   _HOME

        .globl  _g_map
        .globl  _g_ts_dirty_words

; Stage 13 removes the rebuilt-frame + later-diff model on Game Gear.
;
; g_map is now the authoritative desired/displayed name-table shadow. Geometry
; stores compare their final preloaded-tile word against that shadow immediately
; and set a dirty bit only when it changes. Two tiny 18x3-byte coverage maps
; identify geometry-owned cells; cells that disappear this frame are restored to
; the static ceiling/horizon/floor base at frame end. VBlank therefore scans 54
; dirty bytes instead of comparing all 360 16-bit map words.
;
; Bit layout per visible row:
;   byte 0 => columns 0..7
;   byte 1 => columns 8..15
;   byte 2 => columns 16..19 (low nibble only)

_ts_nt_init::
        push    af
        push    bc
        push    de
        push    hl

        ; Static base name-table shadow: 9 ceiling rows, horizon row, 8 floor.
        ld      hl, #_g_map
        ld      b, #180               ; 9 * 20
        xor     a                     ; TS_TILE_CEILING = 0
nti_ceil$:
        ld      (hl), a
        inc     hl
        ld      (hl), #0
        inc     hl
        djnz    nti_ceil$

        ld      b, #20
        ld      a, #2                 ; TS_TILE_HORIZON
nti_horizon$:
        ld      (hl), a
        inc     hl
        ld      (hl), #0
        inc     hl
        djnz    nti_horizon$

        ld      b, #160               ; 8 * 20
        ld      a, #1                 ; TS_TILE_FLOOR
nti_floor$:
        ld      (hl), a
        inc     hl
        ld      (hl), #0
        inc     hl
        djnz    nti_floor$

        ; No geometry coverage exists before the first render.
        xor     a
        ld      hl, #_g_nt_cov_cur
        ld      b, #54
nti_clear_cur$:
        ld      (hl), a
        inc     hl
        djnz    nti_clear_cur$
        ld      hl, #_g_nt_cov_prev
        ld      b, #54
nti_clear_prev$:
        ld      (hl), a
        inc     hl
        djnz    nti_clear_prev$

        ; First upload must establish the entire visible name table while the
        ; display is still off. Mark all 360 legal cells dirty.
        ld      hl, #_g_nt_dirty
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

_ts_nt_begin_frame::
        push    af
        push    bc
        push    hl
        xor     a
        ld      hl, #_g_nt_cov_cur
        ld      b, #54
ntb_clear$:
        ld      (hl), a
        inc     hl
        djnz    ntb_clear$
        pop     hl
        pop     bc
        pop     af
        ret

; Geometry-store ABI used by the hand-written raster:
;   A  = hardware tile row 0..17
;   B  = hardware tile column 0..19
;   HL = &g_map[row*20+col]
;   DE = final 16-bit name-table word
;
; Preserves BC and HL so the column raster's row-fill loop can keep its counter
; and map pointer. DE/AF are scratch. Coverage is marked even if the word is
; unchanged; dirty is marked only when the authoritative shadow changes.
_ts_nt_store_word::
        ld      (#nts_word$), de
        push    bc
        push    hl
        ld      c, a                   ; C=row, B=column for bit addressing

        ; mask = 1 << (col & 7)
        ld      a, b
        and     #7
        ld      e, a
        ld      d, #0
        ld      hl, #nts_mask_lut$
        add     hl, de
        ld      a, (hl)
        ld      (#nts_mask$), a

        ; bit-byte index = row*3 + (col>>3)
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
        ld      de, #_g_nt_cov_cur
        add     hl, de
        ld      a, (#nts_mask$)
        or      (hl)
        ld      (hl), a

        ; Recover the authoritative word pointer and compare before writing.
        pop     hl
        ld      de, (#nts_word$)
        ld      a, (hl)
        cp      e
        jr      nz, nts_changed$
        inc     hl
        ld      a, (hl)
        cp      d
        dec     hl
        jr      z, nts_done$

nts_changed$:
        ld      (hl), e
        inc     hl
        ld      (hl), d
        dec     hl

        ; Same compact row/column addressing for the dirty bitset. This work is
        ; paid only when the final name-table word actually changed.
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
        ld      de, #_g_nt_dirty
        add     hl, de
        ld      a, (#nts_mask$)
        or      (hl)
        ld      (hl), a

nts_done$:
        pop     bc
        ret

; End-frame resolves only cells that were geometry last frame but were not
; touched by geometry this frame. There is no 360-word base-map clear.
_ts_nt_end_frame::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ld      hl, #_g_nt_cov_cur
        ld      de, #_g_nt_cov_prev
        ld      ix, #_g_nt_dirty
        xor     a
        ld      (#nte_row$), a

nte_row_loop$:
        ; Base tile for this visible row.
        ld      a, (#nte_row$)
        cp      #9
        jr      c, nte_base_ceiling$
        jr      z, nte_base_horizon$
        ld      a, #1                 ; floor
        jr      nte_base_ready$
nte_base_horizon$:
        ld      a, #2
        jr      nte_base_ready$
nte_base_ceiling$:
        xor     a
nte_base_ready$:
        ld      (#nte_base$), a
        xor     a
        ld      (#nte_byte$), a

nte_byte_loop$:
        ; stale = previous & ~current; previous = current.
        ld      a, (de)
        ld      c, a
        ld      a, (hl)
        ld      (de), a
        cpl
        and     c
        ld      c, a
        or      a
        jr      z, nte_byte_done$

        ; Every stale cell must return to its static base; the stale mask is
        ; exactly the dirty mask contribution for this byte.
        ld      a, c
        or      0 (ix)
        ld      0 (ix), a
        ld      a, c
        ld      (#nte_stale$), a

        ; IY = map row base + byte*8 cells (16 bytes).
        ld      a, (#nte_row$)
        add     a, a
        ld      c, a
        ld      b, #0
        push    hl
        ld      hl, #nte_map_rows$
        add     hl, bc
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        push    de
        pop     iy
        pop     hl
        ld      a, (#nte_byte$)
        or      a
        jr      z, nte_map_ready$
        dec     a
        jr      z, nte_add16$
        ld      bc, #32
        add     iy, bc
        jr      nte_map_ready$
nte_add16$:
        ld      bc, #16
        add     iy, bc
nte_map_ready$:
        ld      a, (#nte_byte$)
        cp      #2
        ld      b, #8
        jr      nz, nte_bits_ready$
        ld      b, #4
nte_bits_ready$:
        ld      a, (#nte_stale$)
        ld      c, a
nte_bit_loop$:
        srl     c
        jr      nc, nte_no_restore$
        ld      a, (#nte_base$)
        ld      0 (iy), a
        ld      1 (iy), #0
nte_no_restore$:
        inc     iy
        inc     iy
        djnz    nte_bit_loop$

nte_byte_done$:
        inc     hl
        inc     de
        inc     ix
        ld      a, (#nte_byte$)
        inc     a
        ld      (#nte_byte$), a
        cp      #3
        jp      c, nte_byte_loop$

        ld      a, (#nte_row$)
        inc     a
        ld      (#nte_row$), a
        cp      #18
        jp      c, nte_row_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; VBlank uploader: scan 54 dirty bytes, not 360 16-bit old/new pairs. The
; hardware write is deliberately conservative (one VDP address command per
; changed word) so this experiment measures the representation win without
; sneaking in a separate transfer-coalescing optimization.
_ts_nt_upload_dirty::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        xor     a
        ld      (_g_ts_dirty_words), a
        ld      (_g_ts_dirty_words+1), a
        ld      hl, #_g_nt_dirty
        xor     a
        ld      (#ntu_row$), a

ntu_row_loop$:
        xor     a
        ld      (#ntu_byte$), a
ntu_byte_loop$:
        ld      a, (hl)
        ld      (#ntu_mask$), a
        ld      (hl), #0
        or      a
        jp      z, ntu_byte_advance$

        ; Map pointer for row + byte group.
        ld      a, (#ntu_row$)
        add     a, a
        ld      c, a
        ld      b, #0
        push    hl
        ld      hl, #nte_map_rows$
        add     hl, bc
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        push    de
        pop     iy
        pop     hl

        ; VDP name-table row base = 0x18CC + row*64; byte group adds 0/16/32.
        ld      a, (#ntu_row$)
        ld      d, a
        ld      e, #0
        ; DE = row << 6 using 16-bit shifts.
        sla     d
        rr      e
        sla     d
        rr      e
        ; Above pair is awkward for 8-bit row; use table instead below.
        ld      a, (#ntu_row$)
        add     a, a
        ld      c, a
        ld      b, #0
        push    hl
        ld      hl, #ntu_vdp_rows$
        add     hl, bc
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        pop     hl
        push    de
        pop     ix

        ld      a, (#ntu_byte$)
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
        ld      a, (#ntu_byte$)
        cp      #2
        ld      b, #8
        jr      nz, ntu_bits_ready$
        ld      b, #4
ntu_bits_ready$:
        ld      a, (#ntu_mask$)
        ld      c, a

ntu_bit_loop$:
        srl     c
        jr      nc, ntu_skip_word$

        ; Save bit-loop state while BC is reused for IX -> address bytes.
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
        jr      ntu_delay_done$
ntu_delay_done$:
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
        ld      a, (#ntu_byte$)
        inc     a
        ld      (#ntu_byte$), a
        cp      #3
        jp      c, ntu_byte_loop$
        ld      a, (#ntu_row$)
        inc     a
        ld      (#ntu_row$), a
        cp      #18
        jp      c, ntu_row_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

        .area   _CODE
nts_mask_lut$:
        .db 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80

; Direct pointers keep stale-cell and dirty-upload rare paths out of multiply /
; row-stride arithmetic.
nte_map_rows$:
        .dw _g_map+0,   _g_map+40,  _g_map+80,  _g_map+120, _g_map+160, _g_map+200
        .dw _g_map+240, _g_map+280, _g_map+320, _g_map+360, _g_map+400, _g_map+440
        .dw _g_map+480, _g_map+520, _g_map+560, _g_map+600, _g_map+640, _g_map+680
ntu_vdp_rows$:
        .dw 0x18CC,0x190C,0x194C,0x198C,0x19CC,0x1A0C
        .dw 0x1A4C,0x1A8C,0x1ACC,0x1B0C,0x1B4C,0x1B8C
        .dw 0x1BCC,0x1C0C,0x1C4C,0x1C8C,0x1CCC,0x1D0C

        .area   _BSS
_g_nt_cov_cur::  .ds 54
_g_nt_cov_prev:: .ds 54
_g_nt_dirty::    .ds 54
nts_word$:        .ds 2
nts_mask$:        .ds 1
nte_row$:         .ds 1
nte_byte$:        .ds 1
nte_base$:        .ds 1
nte_stale$:       .ds 1
ntu_row$:         .ds 1
ntu_byte$:        .ds 1
ntu_mask$:        .ds 1
