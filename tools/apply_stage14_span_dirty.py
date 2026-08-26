#!/usr/bin/env python3
from pathlib import Path

raster_p = Path('src/tilesector_raster_gg.s')
nt_p = Path('src/tilesector_ntstate_gg.s')
raster = raster_p.read_text()
nt = nt_p.read_text()

if 'STAGE14_SPAN_COVERAGE' in raster:
    print('Stage 14 span coverage already applied')
    raise SystemExit(0)
if '_ts_nt_store_word' not in raster:
    raise SystemExit('Stage 13 raster wiring must be applied first')

# The key correction from Stage 13: coverage is a property of the contiguous
# projected surface span, not of every individual tile store. Mark it once per
# surface-column. Dirty addressing is paid only when a final word actually
# changes in the authoritative name-table shadow.
raster = raster.replace('''        .globl  _ts_nt_store_word\n''',
                        '''        .globl  _ts_nt_store_word\n        .globl  _ts_nt_mark_span\n        .globl  _ts_nt_mark_dirty\n\n; STAGE14_SPAN_COVERAGE\n''', 1)

anchor = '''bot_minmax_done$:\n\n        ; Riser has a snapped top cap; everything else has a vector edge.'''
insert = '''bot_minmax_done$:\n\n        ; Stage 14: every profile occupies one contiguous tile-row span in this\n        ; coarse screen column. Mark that coverage once, rather than making each\n        ; tile store rediscover row/column bit addressing.\n        ld      a, (#r_top_min$)\n        bit     7, a\n        jr      z, cov_first_nonneg$\n        xor     a\ncov_first_nonneg$:\n        ld      e, a                   ; E = clamped geometric first row\n        ld      a, (#r_clip_first$)\n        cp      e\n        jr      c, cov_first_ready$\n        ld      e, a                   ; aperture begins lower\ncov_first_ready$:\n\n        ld      a, (#r_bot_max$)\n        bit     7, a\n        jr      nz, cov_done$\n        cp      #18\n        jr      c, cov_last_screen$\n        ld      a, #17\ncov_last_screen$:\n        ld      c, a                   ; C = geometric last row\n        ld      a, (#r_clip_last$)\n        cp      c\n        jr      nc, cov_last_ready$\n        ld      c, a                   ; aperture ends earlier\ncov_last_ready$:\n        ld      a, e\n        cp      c\n        jr      c, cov_emit$\n        jr      nz, cov_done$\ncov_emit$:\n        call    _ts_nt_mark_span       ; A=first, C=last, B=screen column\ncov_done$:\n\n        ; Riser has a snapped top cap; everything else has a vector edge.'''
if anchor not in raster:
    raise SystemExit('min/max coverage insertion anchor not found')
raster = raster.replace(anchor, insert, 1)

old_edge = '''        push    de\n        ld      a, (#r_row$)\n        call    map_ptr_row_col$\n        pop     de\n        ld      a, (#r_row$)\n        call    _ts_nt_store_word\n        ld      a, (#r_row$)\n        ret'''
new_edge = '''        push    de\n        ld      a, (#r_row$)\n        call    map_ptr_row_col$\n        pop     de\n        ld      a, (hl)\n        cp      e\n        jr      nz, edge_word_changed$\n        inc     hl\n        ld      a, (hl)\n        cp      d\n        dec     hl\n        jr      z, edge_word_done$\nedge_word_changed$:\n        ld      (hl), e\n        inc     hl\n        ld      (hl), d\n        dec     hl\n        ld      a, (#r_row$)\n        call    _ts_nt_mark_dirty\nedge_word_done$:\n        ld      a, (#r_row$)\n        ret'''
if old_edge not in raster:
    raise SystemExit('Stage 13 edge store not found')
raster = raster.replace(old_edge, new_edge, 1)

old_full = '''        call    full_tile_low$\n        ld      e, a\n        ld      a, (#r_cap_delta$)\n        add     a, e\n        ld      e, a\n        ld      d, #0\n        ld      a, (#r_row$)\n        call    _ts_nt_store_word\n        ret'''
new_full = '''        call    full_tile_low$\n        ld      e, a\n        ld      a, (#r_cap_delta$)\n        add     a, e\n        ld      e, a\n        ld      d, #0\n        ld      a, (hl)\n        cp      e\n        jr      nz, full_word_changed$\n        inc     hl\n        ld      a, (hl)\n        or      a\n        dec     hl\n        ret     z\nfull_word_changed$:\n        ld      (hl), e\n        inc     hl\n        ld      (hl), #0\n        dec     hl\n        ld      a, (#r_row$)\n        call    _ts_nt_mark_dirty\n        ret'''
if old_full not in raster:
    raise SystemExit('Stage 13 full store not found')
raster = raster.replace(old_full, new_full, 1)

old_interior = '''interior_loop$:\n        ld      a, (#r_full_tile$)\n        ld      e, a\n        ld      d, #0\n        ld      a, (#r_row$)\n        call    _ts_nt_store_word\n        ld      de, #40              ; next hardware name-table row, same column\n        add     hl, de\n        ld      a, (#r_row$)\n        inc     a\n        ld      (#r_row$), a\n        dec     c\n        jr      nz, interior_loop$\n        ret'''
new_interior = '''interior_loop$:\n        ld      a, (#r_full_tile$)\n        ld      e, a\n        ld      d, #0\n        ld      a, (hl)\n        cp      e\n        jr      nz, interior_word_changed$\n        inc     hl\n        ld      a, (hl)\n        or      a\n        dec     hl\n        jr      z, interior_word_done$\ninterior_word_changed$:\n        ld      (hl), e\n        inc     hl\n        ld      (hl), #0\n        dec     hl\n        ld      a, (#r_row$)\n        call    _ts_nt_mark_dirty\ninterior_word_done$:\n        ld      de, #40              ; next hardware name-table row, same column\n        add     hl, de\n        ld      a, (#r_row$)\n        inc     a\n        ld      (#r_row$), a\n        dec     c\n        jr      nz, interior_loop$\n        ret'''
if old_interior not in raster:
    raise SystemExit('Stage 13 interior store not found')
raster = raster.replace(old_interior, new_interior, 1)

# Add the two amortized state helpers ahead of end-frame reconciliation.
nt_anchor = '''; End-frame resolves only cells that were geometry last frame but were not\n; touched by geometry this frame. There is no 360-word base-map clear.\n_ts_nt_end_frame::'''
if nt_anchor not in nt:
    raise SystemExit('ntstate end-frame anchor not found')
helpers = r'''; Stage 14 coverage ABI: A=first row, C=last row, B=screen column.
; One call marks the whole contiguous surface span. Row-major bitsets mean the
; pointer advances exactly three bytes per tile row.
_ts_nt_mark_span::
        push    af
        push    bc
        push    de
        push    hl
        ld      (#ntm_first$), a
        ld      a, c
        ld      (#ntm_last$), a

        ld      a, b
        and     #7
        ld      e, a
        ld      d, #0
        ld      hl, #nts_mask_lut$
        add     hl, de
        ld      a, (hl)
        ld      (#ntm_mask$), a

        ld      a, (#ntm_first$)
        ld      c, a
        add     a, a
        add     a, c                   ; row * 3
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

        ld      a, (#ntm_last$)
        ld      c, a
        ld      a, (#ntm_first$)
        ld      e, a
        ld      a, c
        sub     e
        inc     a
        ld      b, a
ntm_span_loop$:
        ld      a, (#ntm_mask$)
        or      (hl)
        ld      (hl), a
        inc     hl
        inc     hl
        inc     hl
        djnz    ntm_span_loop$

        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; Dirty ABI: A=row, B=screen column. This runs only when the authoritative
; name-table word actually changed, not for every rasterized cell.
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
        ld      hl, #nts_mask_lut$
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

; End-frame resolves only cells that were geometry last frame but were not
; touched by geometry this frame. There is no 360-word base-map clear.
_ts_nt_end_frame::'''
nt = nt.replace(nt_anchor, helpers, 1)

bss_anchor = '''ntu_mask$:        .ds 1'''
if bss_anchor not in nt:
    raise SystemExit('ntstate BSS anchor not found')
nt = nt.replace(bss_anchor, bss_anchor + '''\nntm_first$:       .ds 1\nntm_last$:        .ds 1\nntm_mask$:        .ds 1\nntd_mask$:        .ds 1''', 1)

raster_p.write_text(raster)
nt_p.write_text(nt)
print('Applied Stage 14 span-amortized coverage + change-only dirty marking.')
