#!/usr/bin/env python3
from pathlib import Path

run_p = Path('src/tilesector_run_gg.s')
raster_p = Path('src/tilesector_raster_gg.s')
opaque_p = Path('src/tilesector_opaque_gg.s')

run = run_p.read_text()
raster = raster_p.read_text()

if 'STAGE16_DIRECT_OPAQUE' in run:
    print('Stage 16 direct opaque materializer already applied')
    raise SystemExit(0)
if 'STAGE15_SHADOW_GENERATION' not in raster:
    raise SystemExit('apply Stage 15 first')
if '_ts_raster_surface_column_fast' not in run:
    raise SystemExit('Stage 12 run kernel missing generic column call')

# Export the existing exact edge vocabulary instead of duplicating ~1.8 KiB of
# lookup data in the new materializer. The generic raster keeps its local alias.
if '_ts_edge_lut::' not in raster:
    if 'edge_lut$:' not in raster:
        raise SystemExit('edge LUT label not found')
    raster = raster.replace('edge_lut$:', '_ts_edge_lut::\nedge_lut$:', 1)

run = run.replace(
    '        .globl  _ts_raster_surface_column_fast\n',
    '        .globl  _ts_raster_surface_column_fast\n'
    '        .globl  _ts_raster_opaque_column_fast\n\n'
    '; STAGE16_DIRECT_OPAQUE: FULL/RAISED_FULL bypass the generic column raster.\n',
    1,
)

old_call = '''        ld      a, (#nr_col$)\n        call    _ts_raster_surface_column_fast\n'''
new_call = '''        ; FULL and RAISED_FULL are opaque and both close the aperture. They now\n        ; materialize final preloaded-tile words directly into the authoritative\n        ; name-table shadow. LINTEL/RISER retain the generic portal-aware kernel.\n        ld      a, (#nr_profile$)\n        cp      #1\n        jp      z, nr_generic_column$\n        cp      #3\n        jp      z, nr_generic_column$\n        ld      a, (#nr_col$)\n        call    _ts_raster_opaque_column_fast\n        jp      nr_column_done$\nnr_generic_column$:\n        ld      a, (#nr_col$)\n        call    _ts_raster_surface_column_fast\nnr_column_done$:\n'''
if old_call not in run:
    raise SystemExit('run->generic-column call site not found')
run = run.replace(old_call, new_call, 1)

opaque = r'''        .title  "TileSector Stage 16 direct opaque name-table materializer"
        .module tilesector_opaque_gg
        .area   _HOME

        .globl  _g_raster_ctx
        .globl  _g_map
        .globl  _g_nt_dirty
        .globl  _g_nt_meta
        .globl  _ts_edge_lut

; Private ABI from the Stage-12 run raster:
;   A = screen tile column 0..19
;   g_raster_ctx already contains shade, connected top/bottom endpoints,
;   endpoint-border bits and current clip pointers.
;
; Only profile FULL (0) and RAISED_FULL (2) enter here. Both use vector top and
; bottom edges and both close the portal aperture after the column is emitted.
; The function intentionally clobbers every register: the enclosing run kernel
; saved its caller state once for the entire horizontal run, so paying another
; push/pop stack tax per screen column would recreate the abstraction cost this
; stage exists to remove.

_ts_raster_opaque_column_fast::
        ld      (#op_col$), a

        ; Precompute the dirty-bit address components once for the whole vertical
        ; column. A changed row is then just row*3 + group and one OR.
        and     #7
        ld      e, a
        ld      d, #0
        ld      hl, #op_mask_lut$
        add     hl, de
        ld      a, (hl)
        ld      (#op_dirty_mask$), a
        ld      a, (#op_col$)
        srl     a
        srl     a
        srl     a
        ld      (#op_dirty_group$), a

        ; Convert the current pixel aperture to whole hardware tile rows.
        ld      hl, (#_g_raster_ctx + 11)
        ld      a, (hl)
        add     a, #7
        srl     a
        srl     a
        srl     a
        cp      #18
        ret     nc
        ld      (#op_clip_first$), a

        ld      hl, (#_g_raster_ctx + 13)
        ld      a, (hl)
        srl     a
        srl     a
        srl     a
        cp      #18
        jp      c, op_clip_last_ok$
        ld      a, #17
op_clip_last_ok$:
        ld      (#op_clip_last$), a
        ld      c, a
        ld      a, (#op_clip_first$)
        cp      c
        jp      c, op_aperture_ready$
        jp      z, op_aperture_ready$
        ret
op_aperture_ready$:

        ; Signed pixel endpoints -> signed tile rows. Connected right endpoints
        ; are already limited to +/-7 px by the run kernel.
        ld      hl, (#_g_raster_ctx + 2)
        call    op_row_floor_hl$
        ld      (#op_top_l_row$), a
        ld      hl, (#_g_raster_ctx + 4)
        call    op_row_floor_hl$
        ld      (#op_top_r_row$), a
        ld      hl, (#_g_raster_ctx + 6)
        call    op_row_floor_hl$
        ld      (#op_bot_l_row$), a
        ld      hl, (#_g_raster_ctx + 8)
        call    op_row_floor_hl$
        ld      (#op_bot_r_row$), a

        ; Signed min/max, top.
        ld      a, (#op_top_l_row$)
        ld      e, a
        xor     #0x80
        ld      c, a
        ld      a, (#op_top_r_row$)
        ld      d, a
        xor     #0x80
        cp      c
        jp      c, op_top_r_min$
        ld      a, e
        ld      (#op_top_min$), a
        ld      a, d
        ld      (#op_top_max$), a
        jp      op_top_mm_done$
op_top_r_min$:
        ld      a, d
        ld      (#op_top_min$), a
        ld      a, e
        ld      (#op_top_max$), a
op_top_mm_done$:

        ; Signed min/max, bottom.
        ld      a, (#op_bot_l_row$)
        ld      e, a
        xor     #0x80
        ld      c, a
        ld      a, (#op_bot_r_row$)
        ld      d, a
        xor     #0x80
        cp      c
        jp      c, op_bot_r_min$
        ld      a, e
        ld      (#op_bot_min$), a
        ld      a, d
        ld      (#op_bot_max$), a
        jp      op_bot_mm_done$
op_bot_r_min$:
        ld      a, d
        ld      (#op_bot_min$), a
        ld      a, e
        ld      (#op_bot_max$), a
op_bot_mm_done$:

        ; Top vector edge.
        xor     a
        ld      (#op_edge_bottom$), a
        ld      hl, (#_g_raster_ctx + 2)
        ld      (#op_edge_left$), hl
        ld      de, (#_g_raster_ctx + 4)
        ld      a, (#op_top_min$)
        ld      (#op_edge_min$), a
        ld      a, (#op_top_max$)
        ld      (#op_edge_max$), a
        call    op_prepare_edge$

        ; Bottom vector edge.
        ld      a, #1
        ld      (#op_edge_bottom$), a
        ld      hl, (#_g_raster_ctx + 6)
        ld      (#op_edge_left$), hl
        ld      de, (#_g_raster_ctx + 8)
        ld      a, (#op_bot_min$)
        ld      (#op_edge_min$), a
        ld      a, (#op_bot_max$)
        ld      (#op_edge_max$), a
        call    op_prepare_edge$

        ; Contiguous full-wall interior. This keeps both the name-table pointer
        ; and dirty-byte pointer live down the column, rather than recomputing a
        ; row/column address or calling a generic helper for every cell.
        call    op_draw_interior$

        ; FULL and RAISED_FULL are opaque: later portal traversal cannot see
        ; through this horizontal screen column.
        ld      hl, (#_g_raster_ctx + 11)
        ld      (hl), #1
        ld      hl, (#_g_raster_ctx + 13)
        ld      (hl), #0
        ret

; Arithmetic floor(pixel/8), including negative projected Y.
op_row_floor_hl$:
        sra     h
        rr      l
        sra     h
        rr      l
        sra     h
        rr      l
        ld      a, l
        ret

; DE=right endpoint from caller, edge_left=left. Compute the exact connected
; slope once and draw one or two edge rows.
op_prepare_edge$:
        ld      hl, (#op_edge_left$)
        ex      de, hl                  ; HL=right, DE=left
        or      a
        sbc     hl, de
        ld      a, l                    ; guaranteed -7..+7
        ld      (#op_edge_slope$), a
        ld      a, (#op_edge_min$)
        call    op_draw_edge_row$
        ld      a, (#op_edge_max$)
        ld      c, a
        ld      a, (#op_edge_min$)
        cp      c
        ret     z
        ld      a, c
        jp      op_draw_edge_row$

; A=signed edge tile row. Lookup the exact preloaded edge tile, apply distance
; shade, then update the authoritative name-table word directly.
op_draw_edge_row$:
        ld      (#op_row$), a
        bit     7, a
        ret     nz
        cp      #18
        ret     nc
        ld      c, a
        ld      a, (#op_clip_first$)
        cp      c
        jp      c, op_edge_first_ok$
        jp      z, op_edge_first_ok$
        ret
op_edge_first_ok$:
        ld      a, (#op_clip_last$)
        cp      c
        ret     c

        ; local = left_y - row*8; clamp to edge LUT domain [-15,+15].
        ld      a, c
        add     a, a
        add     a, a
        add     a, a
        ld      e, a
        ld      a, (#op_edge_left$)
        sub     e
        cp      #0x80
        jp      c, op_local_positive$
        cp      #0xF1
        jp      nc, op_local_ready$
        ld      a, #0xF1
        jp      op_local_ready$
op_local_positive$:
        cp      #16
        jp      c, op_local_ready$
        ld      a, #15
op_local_ready$:
        add     a, #15
        ld      (#op_local_index$), a

        ; group = slope+7 + (bottom?15:0), then index = group*31+local.
        ld      a, (#op_edge_slope$)
        add     a, #7
        ld      e, a
        ld      a, (#op_edge_bottom$)
        or      a
        jp      z, op_group_ready$
        ld      a, e
        add     a, #15
        ld      e, a
op_group_ready$:
        ld      l, e
        ld      h, #0
        ld      d, #0
        push    de
        add     hl, hl
        add     hl, hl
        add     hl, hl
        add     hl, hl
        add     hl, hl                 ; *32
        pop     de
        or      a
        sbc     hl, de                 ; *31
        ld      a, (#op_local_index$)
        ld      e, a
        ld      d, #0
        add     hl, de
        add     hl, hl
        ld      de, #_ts_edge_lut
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)

        ; Shade families are +0x80 and +0x100 tile indices.
        ld      a, (#_g_raster_ctx + 1)
        or      a
        jp      z, op_edge_word_ready$
        dec     a
        jp      nz, op_edge_shade2$
        ld      a, e
        add     a, #0x80
        ld      e, a
        jp      nc, op_edge_word_ready$
        inc     d
        jp      op_edge_word_ready$
op_edge_shade2$:
        inc     d
op_edge_word_ready$:
        ld      a, (#op_row$)
        jp      op_store_edge_word$

; A=row, DE=visible name-table word. Edge rows are rare, so compute their map
; pointer individually; unlike Stage 15's generic path, dirty marking itself is
; inline and shares the per-column mask/group precomputation.
op_store_edge_word$:
        ld      (#op_row$), a
        ld      (#op_word$), de
        call    op_map_ptr_for_row$
        ld      de, (#op_word$)
        ld      a, (hl)
        cp      e
        jp      nz, op_edge_changed$
        inc     hl
        ld      a, (hl)
        and     #0x1f
        cp      d
        dec     hl
        jp      nz, op_edge_changed$
        ; Visible word unchanged: only refresh RAM-private generation metadata.
        inc     hl
        ld      a, d
        and     #0x1f
        or      #0x20
        ld      c, a
        ld      a, (_g_nt_meta)
        or      c
        ld      (hl), a
        ret
op_edge_changed$:
        ld      (hl), e
        inc     hl
        ld      a, d
        and     #0x1f
        or      #0x20
        ld      c, a
        ld      a, (_g_nt_meta)
        or      c
        ld      (hl), a
        call    op_dirty_ptr_for_row$
        ld      a, (#op_dirty_mask$)
        or      (hl)
        ld      (hl), a
        ret

; Fill max(top)+1 .. min(bottom)-1, clipped to the current portal aperture.
op_draw_interior$:
        ld      a, (#op_top_max$)
        inc     a
        bit     7, a
        jp      nz, op_int_first_clip$
        ld      c, a
        ld      a, (#op_clip_first$)
        cp      c
        jp      c, op_int_first_keep$
        jp      z, op_int_first_keep$
op_int_first_clip$:
        ld      a, (#op_clip_first$)
        ld      c, a
op_int_first_keep$:
        ld      a, c
        cp      #18
        ret     nc
        ld      (#op_fill_first$), a

        ld      a, (#op_bot_min$)
        dec     a
        bit     7, a
        ret     nz
        ld      c, a
        ld      a, (#op_clip_last$)
        cp      c
        jp      nc, op_int_last_keep$
        ld      c, a
op_int_last_keep$:
        ld      a, c
        cp      #18
        jp      c, op_int_last_valid$
        ld      a, #17
        ld      c, a
op_int_last_valid$:
        ld      a, (#op_fill_first$)
        cp      c
        jp      c, op_int_have$
        ret     nz
op_int_have$:
        ; B = row count.
        ld      a, c
        ld      e, a
        ld      a, (#op_fill_first$)
        ld      d, a
        ld      a, e
        sub     d
        inc     a
        ld      b, a

        ; Full-wall tile low byte from shade and vertical endpoint-border flags.
        ld      a, (#_g_raster_ctx + 1)
        or      a
        jp      z, op_full_far$
        dec     a
        jp      z, op_full_mid$
        ld      a, #27
        jp      op_full_border$
op_full_mid$:
        ld      a, #15
        jp      op_full_border$
op_full_far$:
        ld      a, #3
op_full_border$:
        ld      e, a
        ld      a, (#_g_raster_ctx + 10)
        add     a, e
        ld      (#op_full_tile$), a

        ; IY = first name-table word; IX = matching dirty-byte group.
        ld      a, (#op_fill_first$)
        ld      (#op_row$), a
        call    op_map_ptr_for_row$
        push    hl
        pop     iy
        call    op_dirty_ptr_for_row$
        push    hl
        pop     ix

op_int_loop$:
        ld      a, (#op_full_tile$)
        ld      e, a
        ld      a, 0 (iy)
        cp      e
        jp      nz, op_int_changed$
        ld      a, 1 (iy)
        and     #0x1f
        jp      nz, op_int_changed$
        ; Same visible full tile: generation metadata only.
        ld      a, (_g_nt_meta)
        or      #0x20
        ld      1 (iy), a
        jp      op_int_advance$
op_int_changed$:
        ld      0 (iy), e
        ld      a, (_g_nt_meta)
        or      #0x20
        ld      1 (iy), a
        ld      a, (#op_dirty_mask$)
        or      0 (ix)
        ld      0 (ix), a
op_int_advance$:
        ld      de, #40
        add     iy, de
        ld      de, #3
        add     ix, de
        djnz    op_int_loop$
        ret

; A=row 0..17 -> HL=&g_map[row*20+column].
op_map_ptr_for_row$:
        add     a, a
        ld      l, a
        ld      h, #0
        ld      de, #op_map_rows$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ex      de, hl
        ld      a, (#op_col$)
        add     a, a
        ld      e, a
        ld      d, #0
        add     hl, de
        ret

; Uses op_row. Return HL=&dirty[row*3 + column_group].
op_dirty_ptr_for_row$:
        ld      a, (#op_row$)
        ld      c, a
        add     a, a
        add     a, c
        ld      c, a
        ld      a, (#op_dirty_group$)
        add     a, c
        ld      l, a
        ld      h, #0
        ld      de, #_g_nt_dirty
        add     hl, de
        ret

        .area   _CODE
op_mask_lut$:
        .db 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80
op_map_rows$:
        .dw _g_map+0,   _g_map+40,  _g_map+80,  _g_map+120, _g_map+160, _g_map+200
        .dw _g_map+240, _g_map+280, _g_map+320, _g_map+360, _g_map+400, _g_map+440
        .dw _g_map+480, _g_map+520, _g_map+560, _g_map+600, _g_map+640, _g_map+680

        .area   _BSS
op_col$:          .ds 1
op_dirty_mask$:   .ds 1
op_dirty_group$:  .ds 1
op_clip_first$:   .ds 1
op_clip_last$:    .ds 1
op_top_l_row$:    .ds 1
op_top_r_row$:    .ds 1
op_bot_l_row$:    .ds 1
op_bot_r_row$:    .ds 1
op_top_min$:      .ds 1
op_top_max$:      .ds 1
op_bot_min$:      .ds 1
op_bot_max$:      .ds 1
op_edge_bottom$:  .ds 1
op_edge_left$:    .ds 2
op_edge_min$:     .ds 1
op_edge_max$:     .ds 1
op_edge_slope$:   .ds 1
op_local_index$:  .ds 1
op_row$:          .ds 1
op_word$:         .ds 2
op_fill_first$:   .ds 1
op_full_tile$:    .ds 1
'''

run_p.write_text(run)
raster_p.write_text(raster)
opaque_p.write_text(opaque)
print('Applied Stage 16 direct opaque run->name-table materializer.')
