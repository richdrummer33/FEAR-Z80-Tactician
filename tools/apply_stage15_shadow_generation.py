#!/usr/bin/env python3
from pathlib import Path

raster_p=Path('src/tilesector_raster_gg.s')
nt_p=Path('src/tilesector_ntstate_gg.s')
raster=raster_p.read_text()

if 'STAGE15_SHADOW_GENERATION' in raster:
    print('Stage 15 shadow-generation metadata already applied')
    raise SystemExit(0)
if 'STAGE14_SPAN_COVERAGE' not in raster:
    raise SystemExit('apply Stage 14 first')

# ---------------------------------------------------------------------------
# Coverage bookkeeping is folded into otherwise-unused bits of the RAM shadow.
# Bits 13/14 (high-byte bits 5/6) never reach the VDP: bit 5 says "geometry",
# bit 6 is a toggled frame generation. A geometry cell merely refreshes those
# two RAM bits while writing/comparing its normal preloaded-tile name-table word.
# End-frame scans only the high bytes; an old generation is stale geometry.
# ---------------------------------------------------------------------------
raster=raster.replace('''        .globl  _ts_nt_store_word\n        .globl  _ts_nt_mark_span\n        .globl  _ts_nt_mark_dirty\n\n; STAGE14_SPAN_COVERAGE\n''',
'''        .globl  _ts_nt_mark_dirty\n        .globl  _g_nt_meta\n\n; STAGE14_SPAN_COVERAGE\n; STAGE15_SHADOW_GENERATION: coverage lives in RAM-only name-table metadata.\n''',1)

coverage='''bot_minmax_done$:\n\n        ; Stage 14: every profile occupies one contiguous tile-row span in this\n        ; coarse screen column. Mark that coverage once, rather than making each\n        ; tile store rediscover row/column bit addressing.\n        ld      a, (#r_top_min$)\n        bit     7, a\n        jr      z, cov_first_nonneg$\n        xor     a\ncov_first_nonneg$:\n        ld      e, a                   ; E = clamped geometric first row\n        ld      a, (#r_clip_first$)\n        cp      e\n        jr      c, cov_first_ready$\n        ld      e, a                   ; aperture begins lower\ncov_first_ready$:\n\n        ld      a, (#r_bot_max$)\n        bit     7, a\n        jr      nz, cov_done$\n        cp      #18\n        jr      c, cov_last_screen$\n        ld      a, #17\ncov_last_screen$:\n        ld      c, a                   ; C = geometric last row\n        ld      a, (#r_clip_last$)\n        cp      c\n        jr      nc, cov_last_ready$\n        ld      c, a                   ; aperture ends earlier\ncov_last_ready$:\n        ld      a, e\n        cp      c\n        jr      c, cov_emit$\n        jr      nz, cov_done$\ncov_emit$:\n        call    _ts_nt_mark_span       ; A=first, C=last, B=screen column\ncov_done$:\n\n        ; Riser has a snapped top cap; everything else has a vector edge.'''
replacement='''bot_minmax_done$:\n\n        ; Stage 15 has no separate coverage walk. Every actual tile store stamps\n        ; the authoritative shadow high byte with the current generation.\n        ; Riser has a snapped top cap; everything else has a vector edge.'''
if coverage not in raster:
    raise SystemExit('Stage 14 coverage block not found')
raster=raster.replace(coverage,replacement,1)

old_edge='''        push    de\n        ld      a, (#r_row$)\n        call    map_ptr_row_col$\n        pop     de\n        ld      a, (hl)\n        cp      e\n        jr      nz, edge_word_changed$\n        inc     hl\n        ld      a, (hl)\n        cp      d\n        dec     hl\n        jr      z, edge_word_done$\nedge_word_changed$:\n        ld      (hl), e\n        inc     hl\n        ld      (hl), d\n        dec     hl\n        ld      a, (#r_row$)\n        call    _ts_nt_mark_dirty\nedge_word_done$:\n        ld      a, (#r_row$)\n        ret'''
new_edge='''        push    de\n        ld      a, (#r_row$)\n        call    map_ptr_row_col$\n        pop     de\n        ld      a, (hl)\n        cp      e\n        jr      nz, edge_visual_changed$\n        inc     hl\n        ld      a, (hl)\n        and     #0x1f                 ; ignore RAM-only geometry/generation bits\n        cp      d\n        dec     hl\n        jr      nz, edge_visual_changed$\n        ; Same visible word: refresh only the hidden generation metadata.\n        inc     hl\n        ld      a, d\n        or      #0x20                 ; geometry-owned\n        ld      d, a\n        ld      a, (_g_nt_meta)\n        or      d\n        ld      (hl), a\n        dec     hl\n        jr      edge_word_done$\nedge_visual_changed$:\n        ld      (hl), e\n        inc     hl\n        ld      a, d\n        or      #0x20\n        ld      d, a\n        ld      a, (_g_nt_meta)\n        or      d\n        ld      (hl), a\n        dec     hl\n        ld      a, (#r_row$)\n        call    _ts_nt_mark_dirty\nedge_word_done$:\n        ld      a, (#r_row$)\n        ret'''
if old_edge not in raster: raise SystemExit('Stage 14 edge block not found')
raster=raster.replace(old_edge,new_edge,1)

old_full='''        call    full_tile_low$\n        ld      e, a\n        ld      a, (#r_cap_delta$)\n        add     a, e\n        ld      e, a\n        ld      d, #0\n        ld      a, (hl)\n        cp      e\n        jr      nz, full_word_changed$\n        inc     hl\n        ld      a, (hl)\n        or      a\n        dec     hl\n        ret     z\nfull_word_changed$:\n        ld      (hl), e\n        inc     hl\n        ld      (hl), #0\n        dec     hl\n        ld      a, (#r_row$)\n        call    _ts_nt_mark_dirty\n        ret'''
new_full='''        call    full_tile_low$\n        ld      e, a\n        ld      a, (#r_cap_delta$)\n        add     a, e\n        ld      e, a\n        ld      d, #0\n        ld      a, (hl)\n        cp      e\n        jr      nz, full_visual_changed$\n        inc     hl\n        ld      a, (hl)\n        and     #0x1f\n        dec     hl\n        jr      nz, full_visual_changed$\n        inc     hl\n        ld      a, (_g_nt_meta)\n        or      #0x20\n        ld      (hl), a\n        dec     hl\n        ret\nfull_visual_changed$:\n        ld      (hl), e\n        inc     hl\n        ld      a, (_g_nt_meta)\n        or      #0x20\n        ld      (hl), a\n        dec     hl\n        ld      a, (#r_row$)\n        call    _ts_nt_mark_dirty\n        ret'''
if old_full not in raster: raise SystemExit('Stage 14 full block not found')
raster=raster.replace(old_full,new_full,1)

old_interior='''interior_loop$:\n        ld      a, (#r_full_tile$)\n        ld      e, a\n        ld      d, #0\n        ld      a, (hl)\n        cp      e\n        jr      nz, interior_word_changed$\n        inc     hl\n        ld      a, (hl)\n        or      a\n        dec     hl\n        jr      z, interior_word_done$\ninterior_word_changed$:\n        ld      (hl), e\n        inc     hl\n        ld      (hl), #0\n        dec     hl\n        ld      a, (#r_row$)\n        call    _ts_nt_mark_dirty\ninterior_word_done$:\n        ld      de, #40              ; next hardware name-table row, same column\n        add     hl, de\n        ld      a, (#r_row$)\n        inc     a\n        ld      (#r_row$), a\n        dec     c\n        jr      nz, interior_loop$\n        ret'''
new_interior='''interior_loop$:\n        ld      a, (#r_full_tile$)\n        ld      e, a\n        ld      d, #0\n        ld      a, (hl)\n        cp      e\n        jr      nz, interior_visual_changed$\n        inc     hl\n        ld      a, (hl)\n        and     #0x1f\n        dec     hl\n        jr      nz, interior_visual_changed$\n        inc     hl\n        ld      a, (_g_nt_meta)\n        or      #0x20\n        ld      (hl), a\n        dec     hl\n        jr      interior_word_done$\ninterior_visual_changed$:\n        ld      (hl), e\n        inc     hl\n        ld      a, (_g_nt_meta)\n        or      #0x20\n        ld      (hl), a\n        dec     hl\n        ld      a, (#r_row$)\n        call    _ts_nt_mark_dirty\ninterior_word_done$:\n        ld      de, #40              ; next hardware name-table row, same column\n        add     hl, de\n        ld      a, (#r_row$)\n        inc     a\n        ld      (#r_row$), a\n        dec     c\n        jr      nz, interior_loop$\n        ret'''
if old_interior not in raster: raise SystemExit('Stage 14 interior block not found')
raster=raster.replace(old_interior,new_interior,1)
raster_p.write_text(raster)

# Replace the Stage13/14 coverage implementation with a smaller generation-tag
# state machine. Dirty remains a 54-byte screen bitset because only changed
# visible words need to reach VRAM.
nt=r'''        .title  "TileSector generation-tagged authoritative name table"
        .module tilesector_ntstate_gg
        .area   _HOME

        .globl  _g_map
        .globl  _g_ts_dirty_words

; RAM shadow high-byte metadata, stripped before VDP upload:
;   bit 5 (word bit 13) = geometry-owned cell
;   bit 6 (word bit 14) = current render generation
; Visible SMS/GG name-table information remains in high-byte bits 0..4.

_ts_nt_init::
        push    af
        push    bc
        push    hl
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

        xor     a
        ld      (_g_nt_meta), a
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
        pop     bc
        pop     af
        ret

_ts_nt_begin_frame::
        push    af
        ld      a, (_g_nt_meta)
        xor     #0x40
        ld      (_g_nt_meta), a
        pop     af
        ret

; A=row 0..17, B=column 0..19. Called only when the visible word changed.
; Preserve caller state because the raster often sits inside a vertical fill.
_ts_nt_mark_dirty::
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
        ld      de, #_g_nt_dirty
        add     hl, de
        ld      a, (#ntd_mask$)
        or      (hl)
        ld      (hl), a
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; Tight high-byte generation scan. No coverage map is built during rasterization.
; Geometry carrying the previous generation was not touched this frame and is
; restored to the static ceiling/horizon/floor name-table word.
_ts_nt_end_frame::
        push    af
        push    bc
        push    de
        push    hl
        ld      hl, #_g_map+1          ; high byte of row0/col0
        ld      de, #_g_nt_dirty       ; one dirty byte per 8-column group
        xor     a
        ld      (#nte_row$), a

nte_row_loop$:
        ld      a, (#nte_row$)
        cp      #9
        jr      c, nte_ceil$
        jr      z, nte_horizon$
        ld      a, #1
        jr      nte_base_ready$
nte_horizon$:
        ld      a, #2
        jr      nte_base_ready$
nte_ceil$:
        xor     a
nte_base_ready$:
        ld      (#nte_base$), a
        ld      a, #1
        ld      (#nte_mask$), a
        ld      b, #20
        ld      c, #8

nte_cell_loop$:
        ld      a, (hl)
        bit     5, a
        jr      z, nte_cell_done$
        and     #0x40
        push    bc
        ld      c, a
        ld      a, (_g_nt_meta)
        cp      c
        pop     bc
        jr      z, nte_cell_done$

        ; Stale geometry: restore base visual word and dirty this cell.
        dec     hl
        ld      a, (#nte_base$)
        ld      (hl), a
        inc     hl
        ld      (hl), #0
        ld      a, (de)
        ld      (#nte_dirty_old$), a
        ld      a, (#nte_mask$)
        ld      c, a
        ld      a, (#nte_dirty_old$)
        or      c
        ld      (de), a
        ; C is immediately rebuilt below at group boundaries; preserve the
        ; within-group countdown explicitly instead of relying on it here.
        ld      a, (#nte_group_left$)
        ld      c, a

nte_cell_done$:
        inc     hl
        inc     hl
        ld      a, (#nte_mask$)
        add     a, a
        ld      (#nte_mask$), a
        dec     c
        ld      a, c
        ld      (#nte_group_left$), a
        jr      nz, nte_group_not_done$
        inc     de
        ld      a, #1
        ld      (#nte_mask$), a
        ld      c, #8
        ld      a, b
        cp      #4                     ; after 16 cells, final group has four
        jr      nz, nte_group_reset$
        ld      c, #4
nte_group_reset$:
        ld      a, c
        ld      (#nte_group_left$), a
nte_group_not_done$:
        djnz    nte_cell_loop$

        ; We consumed exactly three dirty bytes for this 20-cell row.
        ld      a, (#nte_row$)
        inc     a
        ld      (#nte_row$), a
        cp      #18
        jp      c, nte_row_loop$

        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; Scan 54 dirty bytes and upload only changed words. The RAM-only metadata bits
; are masked from the high byte, so the VDP sees an ordinary name-table entry.
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

        ld      a, (#ntu_row$)
        add     a, a
        ld      c, a
        ld      b, #0
        push    hl
        ld      hl, #nt_map_rows$
        add     hl, bc
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        push    de
        pop     iy
        pop     hl

        ld      a, (#ntu_row$)
        add     a, a
        ld      c, a
        ld      b, #0
        push    hl
        ld      hl, #nt_vdp_rows$
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
        and     #0x1f                  ; strip RAM metadata bits 5/6
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

        .area _CODE
nt_mask_lut$:
        .db 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80
nt_map_rows$:
        .dw _g_map+0,   _g_map+40,  _g_map+80,  _g_map+120, _g_map+160, _g_map+200
        .dw _g_map+240, _g_map+280, _g_map+320, _g_map+360, _g_map+400, _g_map+440
        .dw _g_map+480, _g_map+520, _g_map+560, _g_map+600, _g_map+640, _g_map+680
nt_vdp_rows$:
        .dw 0x18CC,0x190C,0x194C,0x198C,0x19CC,0x1A0C
        .dw 0x1A4C,0x1A8C,0x1ACC,0x1B0C,0x1B4C,0x1B8C
        .dw 0x1BCC,0x1C0C,0x1C4C,0x1C8C,0x1CCC,0x1D0C

        .area _BSS
_g_nt_dirty::     .ds 54
_g_nt_meta::      .ds 1
ntd_mask$:         .ds 1
nte_row$:          .ds 1
nte_base$:         .ds 1
nte_mask$:         .ds 1
nte_group_left$:   .ds 1
nte_dirty_old$:    .ds 1
ntu_row$:          .ds 1
ntu_byte$:         .ds 1
ntu_mask$:         .ds 1
'''
nt_p.write_text(nt)
print('Applied Stage 15 generation-tagged authoritative name-table shadow.')
