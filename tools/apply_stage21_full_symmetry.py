#!/usr/bin/env python3
from pathlib import Path
import re

core_p=Path('src/tilesector_core.c')
run_p=Path('src/tilesector_run_gg.s')
sym_p=Path('src/tilesector_symfull_gg.s')

core=core_p.read_text()
run=run_p.read_text()

if 'STAGE21_FULL_SYMMETRY' in run:
    print('Stage 21 FULL symmetry already applied')
    raise SystemExit(0)
if 'STAGE20_ROW_EXTENTS' not in Path('src/tilesector_ntstate_gg.s').read_text():
    raise SystemExit('apply Stage 20 row extents first')
if 'STAGE16_DIRECT_OPAQUE' not in run:
    raise SystemExit('Stage 18/16 direct opaque routing not present')

# ---------------------------------------------------------------------------
# Host/reference: FULL walls intentionally adopt the exact hardware invariant.
# The host remains the oracle for the NEW deliberate quantization convention.
# ---------------------------------------------------------------------------
m=re.search(r'static void render_sector_candidates_ref\(.*?\n\}\n',core,re.S)
if not m:
    raise SystemExit('Stage12 host reference raster function not found')
ref=m.group(0)

old=r'''        profile=k_segments[seg_id].profile;
        hl=(int16_t)(g_best_inv_l_q6[c]>>1);
        hr=(int16_t)(g_best_inv_r_q6[c]>>1);
        top_l=(int16_t)(TS_HORIZON-((hl+31)>>6));
        top_target=(int16_t)(TS_HORIZON-((hr+31)>>6));
        if (profile==TS_PROFILE_RAISED_FULL) {
            hl=(int16_t)(hl-(hl>>2));
            hr=(int16_t)(hr-(hr>>2));
        }
        bot_l=(int16_t)(TS_HORIZON+((hl+32)>>6));
        bot_target=(int16_t)(TS_HORIZON+((hr+32)>>6));
        if (prev_seg==seg_id) { top_l=carry_top; bot_l=carry_bottom; }
        top_r=connected_end(top_l,top_target);
        bot_r=connected_end(bot_l,bot_target);
        carry_top=top_r; carry_bottom=bot_r; prev_seg=seg_id;
'''
new=r'''        profile=k_segments[seg_id].profile;
        if (profile==TS_PROFILE_FULL) {
            /* STAGE21_FULL_SYMMETRY: quantize around the physical 143/2
             * screen centre. One edge determines the other exactly. */
            uint8_t il=(uint8_t)((g_best_inv_l_q6[c]+32)>>6);
            uint8_t ir=(uint8_t)((g_best_inv_r_q6[c]+32)>>6);
            top_l=(int16_t)(71-(int16_t)(il>>1));
            top_target=(int16_t)(71-(int16_t)(ir>>1));
            if (prev_seg==seg_id) top_l=carry_top;
            bot_l=(int16_t)(143-top_l);
            top_r=connected_end(top_l,top_target);
            bot_r=(int16_t)(143-top_r);
        } else {
            hl=(int16_t)(g_best_inv_l_q6[c]>>1);
            hr=(int16_t)(g_best_inv_r_q6[c]>>1);
            top_l=(int16_t)(TS_HORIZON-((hl+31)>>6));
            top_target=(int16_t)(TS_HORIZON-((hr+31)>>6));
            if (profile==TS_PROFILE_RAISED_FULL) {
                hl=(int16_t)(hl-(hl>>2));
                hr=(int16_t)(hr-(hr>>2));
            }
            bot_l=(int16_t)(TS_HORIZON+((hl+32)>>6));
            bot_target=(int16_t)(TS_HORIZON+((hr+32)>>6));
            if (prev_seg==seg_id) { top_l=carry_top; bot_l=carry_bottom; }
            top_r=connected_end(top_l,top_target);
            bot_r=connected_end(bot_l,bot_target);
        }
        carry_top=top_r; carry_bottom=bot_r; prev_seg=seg_id;
'''
if old not in ref:
    raise SystemExit('host FULL geometry block not found')
ref=ref.replace(old,new,1)
core=core[:m.start()]+ref+core[m.end():]

# ---------------------------------------------------------------------------
# GG run kernel: FULL computes only the top edge. Bottom endpoints are 143-top.
# RAISED/LINTEL/RISER retain the existing generic geometry path.
# ---------------------------------------------------------------------------
old_block=r'''nr_visible$:
        ; next reciprocal = current + per-column step.
        ld      hl, (#nr_invq$)
        ld      de, (#nr_step$)
        add     hl, de
        ld      (#nr_nextq$), hl

        ; Left ideal geometry. If this run is continuous, the actual left edge
        ; is the exact endpoint emitted by the preceding tile instead.
        ld      hl, (#nr_invq$)
        call    nr_round_inv$
        call    nr_lookup_geom$
        ld      a, (#nr_have_carry$)
        or      a
        jr      nz, nr_use_carry$
        ld      (_g_raster_ctx + 2), bc
        ld      (_g_raster_ctx + 6), de
        jr      nr_left_ready$
nr_use_carry$:
        ld      hl, (#nr_carry_top$)
        ld      (_g_raster_ctx + 2), hl
        ld      hl, (#nr_carry_bot$)
        ld      (_g_raster_ctx + 6), hl
nr_left_ready$:

        ; Right ideal target from the next reciprocal. Clamp the emitted rise to
        ; +/-7 pixels, exactly the precomputed edge vocabulary, and carry that
        ; actual endpoint into the next column.
        ld      hl, (#nr_nextq$)
        call    nr_round_inv$
        call    nr_lookup_geom$        ; BC=top target, DE=bottom target
        ld      (#nr_target_bot$), de

        ld      hl, (#_g_raster_ctx + 2)
        ld      d, b
        ld      e, c
        call    nr_connect$            ; DE = connected top right
        ld      (_g_raster_ctx + 4), de
        ld      (#nr_carry_top$), de

        ld      hl, (#_g_raster_ctx + 6)
        ld      de, (#nr_target_bot$)
        call    nr_connect$
        ld      (_g_raster_ctx + 8), de
        ld      (#nr_carry_bot$), de
        ld      a, #1
        ld      (#nr_have_carry$), a

        ; Exact midpoint reciprocal for distance shade: (left+right)>>7.
'''
new_block=r'''nr_visible$:
        ; next reciprocal = current + per-column step.
        ld      hl, (#nr_invq$)
        ld      de, (#nr_step$)
        add     hl, de
        ld      (#nr_nextq$), hl

        ; FULL is the overwhelmingly common case. Its exact screen-space
        ; symmetry means there is only ONE connected edge to solve.
        ld      a, (#nr_profile$)
        or      a
        jp      nz, nr_generic_geometry$

        ld      a, (#nr_have_carry$)
        or      a
        jp      nz, nr_sym_use_carry$

        ld      hl, (#nr_invq$)
        call    nr_round_inv$
        call    nr_sym_top_de$         ; DE = signed top Y
        ld      (_g_raster_ctx + 2), de
        ld      hl, #143
        or      a
        sbc     hl, de
        ld      (_g_raster_ctx + 6), hl
        jp      nr_sym_left_ready$

nr_sym_use_carry$:
        ld      de, (#nr_carry_top$)
        ld      (_g_raster_ctx + 2), de
        ld      hl, #143
        or      a
        sbc     hl, de
        ld      (_g_raster_ctx + 6), hl

nr_sym_left_ready$:
        ld      hl, (#nr_nextq$)
        call    nr_round_inv$
        call    nr_sym_top_de$         ; DE = ideal top-right target
        ld      hl, (#_g_raster_ctx + 2)
        call    nr_connect$
        ld      (_g_raster_ctx + 4), de
        ld      (#nr_carry_top$), de

        ; The connected bottom endpoint is the exact vertical mirror of the
        ; ACTUAL emitted top endpoint, so continuity survives quantization.
        ld      hl, #143
        or      a
        sbc     hl, de
        ld      (_g_raster_ctx + 8), hl

        ld      a, #1
        ld      (#nr_have_carry$), a
        jp      nr_geometry_ready$

nr_generic_geometry$:
        ; Existing asymmetric profiles retain their two-edge geometry.
        ld      hl, (#nr_invq$)
        call    nr_round_inv$
        call    nr_lookup_geom$
        ld      a, (#nr_have_carry$)
        or      a
        jr      nz, nr_use_carry$
        ld      (_g_raster_ctx + 2), bc
        ld      (_g_raster_ctx + 6), de
        jr      nr_left_ready$
nr_use_carry$:
        ld      hl, (#nr_carry_top$)
        ld      (_g_raster_ctx + 2), hl
        ld      hl, (#nr_carry_bot$)
        ld      (_g_raster_ctx + 6), hl
nr_left_ready$:

        ld      hl, (#nr_nextq$)
        call    nr_round_inv$
        call    nr_lookup_geom$
        ld      (#nr_target_bot$), de

        ld      hl, (#_g_raster_ctx + 2)
        ld      d, b
        ld      e, c
        call    nr_connect$
        ld      (_g_raster_ctx + 4), de
        ld      (#nr_carry_top$), de

        ld      hl, (#_g_raster_ctx + 6)
        ld      de, (#nr_target_bot$)
        call    nr_connect$
        ld      (_g_raster_ctx + 8), de
        ld      (#nr_carry_bot$), de
        ld      a, #1
        ld      (#nr_have_carry$), a

nr_geometry_ready$:
        ; Exact midpoint reciprocal for distance shade: (left+right)>>7.
'''
if old_block not in run:
    raise SystemExit('run geometry block not found')
run=run.replace(old_block,new_block,1)

# Insert the cheap reciprocal->top conversion before the generic 4-byte lookup.
anchor='''; A=0..255 reciprocal. Return BC=top Y, DE=bottom Y from the profile table.
nr_lookup_geom$:
'''
helper=r'''; A=0..255 rounded reciprocal. Return DE=signed top Y for the
; symmetric FULL profile. top = 71 - floor(inv/2); bottom is 143-top.
nr_sym_top_de$:
        srl     a
        ld      c, a
        ld      a, #71
        sub     c
        ld      e, a
        sbc     a, a
        ld      d, a
        ret

; A=0..255 reciprocal. Return BC=top Y, DE=bottom Y from the profile table.
nr_lookup_geom$:
'''
if anchor not in run:
    raise SystemExit('run lookup helper anchor not found')
run=run.replace(anchor,helper,1)

# Route only FULL to the new one-edge materializer.
run=run.replace('        .globl  _ts_raster_opaque_column_fast\n',
                '        .globl  _ts_raster_opaque_column_fast\n        .globl  _ts_raster_symfull_column_fast\n',1)

old_call=r'''        ld      a, (#nr_profile$)
        cp      #1
        jp      z, nr_generic_column$
        cp      #3
        jp      z, nr_generic_column$
        ld      a, (#nr_col$)
        call    _ts_raster_opaque_column_fast
        jp      nr_column_done$
nr_generic_column$:
        ld      a, (#nr_col$)
        call    _ts_raster_surface_column_fast
nr_column_done$:
'''
new_call=r'''        ld      a, (#nr_profile$)
        or      a
        jp      z, nr_symmetric_full_column$
        cp      #1
        jp      z, nr_generic_column$
        cp      #3
        jp      z, nr_generic_column$
        ; RAISED_FULL remains the asymmetric direct opaque path.
        ld      a, (#nr_col$)
        call    _ts_raster_opaque_column_fast
        jp      nr_column_done$
nr_symmetric_full_column$:
        ld      a, (#nr_col$)
        call    _ts_raster_symfull_column_fast
        jp      nr_column_done$
nr_generic_column$:
        ld      a, (#nr_col$)
        call    _ts_raster_surface_column_fast
nr_column_done$:
'''
if old_call not in run:
    raise SystemExit('Stage16 direct opaque call route not found')
run=run.replace(old_call,new_call,1)
run=run.replace('        .module tilesector_run_gg\n',
                '        .module tilesector_run_gg\n; STAGE21_FULL_SYMMETRY\n',1)

sym=r'''        .title  "TileSector Stage 21 symmetric FULL materializer"
        .module tilesector_symfull_gg
        .area   _HOME

        .globl  _g_raster_ctx
        .globl  _g_map
        .globl  _g_nt_row_min
        .globl  _g_nt_row_max
        .globl  _ts_nt_mark_span
        .globl  _ts_edge_lut

; STAGE21_FULL_SYMMETRY
; A = screen tile column.
;
; FULL walls are exact mirrors around y=71.5:
;   bottomY = 143-topY
;   bottomRow = 17-topRow
;   bottomLocal = 7-topLocal
;   bottomSlope = -topSlope
;
; Therefore each top edge LUT result is reused verbatim for the bottom edge;
; only VFLIP + palette attributes (high-byte OR 0x0C) are added.

_ts_raster_symfull_column_fast::
        ld      (#sf_col$), a

        ; Convert current pixel aperture to hardware tile rows.
        ld      hl, (#_g_raster_ctx + 11)
        ld      a, (hl)
        add     a, #7
        srl     a
        srl     a
        srl     a
        cp      #18
        ret     nc
        ld      (#sf_clip_first$), a

        ld      hl, (#_g_raster_ctx + 13)
        ld      a, (hl)
        srl     a
        srl     a
        srl     a
        cp      #18
        jr      c, sf_clip_last_ok$
        ld      a, #17
sf_clip_last_ok$:
        ld      (#sf_clip_last$), a
        ld      c, a
        ld      a, (#sf_clip_first$)
        cp      c
        jr      c, sf_aperture_ready$
        jr      z, sf_aperture_ready$
        ret
sf_aperture_ready$:

        ; Only the TOP endpoint rows are solved.
        ld      hl, (#_g_raster_ctx + 2)
        call    sf_row_floor_hl$
        ld      (#sf_top_l_row$), a
        ld      hl, (#_g_raster_ctx + 4)
        call    sf_row_floor_hl$
        ld      (#sf_top_r_row$), a

        ; Signed top min/max row.
        ld      a, (#sf_top_l_row$)
        ld      e, a
        xor     #0x80
        ld      c, a
        ld      a, (#sf_top_r_row$)
        ld      d, a
        xor     #0x80
        cp      c
        jr      c, sf_top_r_min$
        ld      a, e
        ld      (#sf_top_min$), a
        ld      a, d
        ld      (#sf_top_max$), a
        jr      sf_top_mm_done$
sf_top_r_min$:
        ld      a, d
        ld      (#sf_top_min$), a
        ld      a, e
        ld      (#sf_top_max$), a
sf_top_mm_done$:

        ; Mark lifetime coverage once: top_min .. mirror(top_min), clipped.
        ld      a, (#sf_top_min$)
        bit     7, a
        jr      z, sf_cov_first_nonneg$
        xor     a
sf_cov_first_nonneg$:
        ld      e, a
        ld      a, (#sf_clip_first$)
        cp      e
        jr      c, sf_cov_first_ready$
        jr      z, sf_cov_first_ready$
        ld      e, a
sf_cov_first_ready$:
        ld      a, (#sf_top_min$)
        ld      c, a
        ld      a, #17
        sub     c
        bit     7, a
        jr      nz, sf_cov_done$
        cp      #18
        jr      c, sf_cov_last_screen$
        ld      a, #17
sf_cov_last_screen$:
        ld      c, a
        ld      a, (#sf_clip_last$)
        cp      c
        jr      nc, sf_cov_last_ready$
        ld      c, a
sf_cov_last_ready$:
        ld      a, e
        cp      c
        jr      c, sf_cov_emit$
        jr      nz, sf_cov_done$
sf_cov_emit$:
        ld      (#sf_cov_first$), a
        ld      a, (#sf_col$)
        ld      b, a
        ld      a, (#sf_cov_first$)
        call    _ts_nt_mark_span
sf_cov_done$:

        ; top slope = topR-topL; run kernel already constrains it to +/-7.
        ld      hl, (#_g_raster_ctx + 4)
        ld      de, (#_g_raster_ctx + 2)
        or      a
        sbc     hl, de
        ld      a, l
        ld      (#sf_slope$), a

        ; Top edge geometric rows, clipped only to physical screen. Each emitted
        ; top word independently checks the portal aperture, then mirrors to 17-r.
        ld      a, (#sf_top_max$)
        bit     7, a
        jr      nz, sf_edges_done$     ; entire top edge above screen
        ld      c, a
        ld      a, (#sf_top_min$)
        bit     7, a
        jr      z, sf_edge_first_nonneg$
        xor     a
sf_edge_first_nonneg$:
        cp      #18
        jr      nc, sf_edges_done$
        ld      (#sf_edge_row$), a
        ld      a, c
        cp      #18
        jr      c, sf_edge_last_ready$
        ld      a, #17
sf_edge_last_ready$:
        ld      (#sf_edge_last$), a

sf_edge_loop$:
        ld      a, (#sf_edge_row$)
        call    sf_top_edge_word$      ; DE = top edge word
        ld      a, (#sf_edge_row$)
        call    sf_store_if_clip$      ; preserves DE

        ; Same pattern + HFLIP state, hardware-flipped vertically for floor edge.
        ld      a, d
        or      #0x0c                  ; VFLIP | palette 1
        ld      d, a
        ld      a, (#sf_edge_row$)
        ld      c, a
        ld      a, #17
        sub     c
        call    sf_store_if_clip$

        ld      a, (#sf_edge_row$)
        ld      c, a
        ld      a, (#sf_edge_last$)
        cp      c
        jr      z, sf_edges_done$
        ld      a, c
        inc     a
        ld      (#sf_edge_row$), a
        jr      sf_edge_loop$

sf_edges_done$:
        ; Interior is top_max+1 .. mirror(top_max)-1 = 16-top_max.
        ld      a, (#sf_top_max$)
        inc     a
        bit     7, a
        jr      nz, sf_fill_first_clip$
        ld      c, a
        ld      a, (#sf_clip_first$)
        cp      c
        jr      c, sf_fill_first_keep$
        jr      z, sf_fill_first_keep$
sf_fill_first_clip$:
        ld      a, (#sf_clip_first$)
        ld      c, a
sf_fill_first_keep$:
        ld      a, c
        cp      #18
        jr      nc, sf_close$
        ld      (#sf_fill_first$), a

        ld      a, (#sf_top_max$)
        ld      c, a
        ld      a, #16
        sub     c
        bit     7, a
        jr      nz, sf_close$
        cp      #18
        jr      c, sf_fill_last_screen$
        ld      a, #17
sf_fill_last_screen$:
        ld      c, a
        ld      a, (#sf_clip_last$)
        cp      c
        jr      nc, sf_fill_last_ready$
        ld      c, a
sf_fill_last_ready$:
        ld      a, (#sf_fill_first$)
        cp      c
        jr      c, sf_fill_have$
        jr      z, sf_fill_have$
        jr      sf_close$

sf_fill_have$:
        ld      a, c
        ld      e, a
        ld      a, (#sf_fill_first$)
        ld      d, a
        ld      a, e
        sub     d
        inc     a
        ld      b, a

        ; Full interior tile from shade + segment endpoint border.
        ld      a, (#_g_raster_ctx + 1)
        or      a
        jr      z, sf_full_far$
        dec     a
        jr      z, sf_full_mid$
        ld      a, #27
        jr      sf_full_border$
sf_full_mid$:
        ld      a, #15
        jr      sf_full_border$
sf_full_far$:
        ld      a, #3
sf_full_border$:
        ld      e, a
        ld      a, (#_g_raster_ctx + 10)
        add     a, e
        ld      (#sf_full_tile$), a

        ld      a, (#sf_fill_first$)
        ld      (#sf_store_row$), a
        call    sf_map_ptr_for_row$
        push    hl
        pop     iy

sf_fill_loop$:
        ld      a, (#sf_full_tile$)
        ld      e, a
        ld      a, 0 (iy)
        cp      e
        jr      nz, sf_fill_changed$
        ld      a, 1 (iy)
        or      a
        jr      z, sf_fill_advance$
sf_fill_changed$:
        ld      0 (iy), e
        xor     a
        ld      1 (iy), a
        call    sf_dirty_store_row$
sf_fill_advance$:
        ld      de, #40
        add     iy, de
        ld      a, (#sf_store_row$)
        inc     a
        ld      (#sf_store_row$), a
        djnz    sf_fill_loop$

sf_close$:
        ; FULL is opaque: farther portal traversal cannot contribute here.
        ld      hl, (#_g_raster_ctx + 11)
        ld      (hl), #1
        ld      hl, (#_g_raster_ctx + 13)
        ld      (hl), #0
        ret

; A = top edge row. Return DE = shade-adjusted top edge name-table word.
sf_top_edge_word$:
        ld      (#sf_tmp_row$), a

        ; local = top_left - row*8. For an intersected edge row and |slope|<=7
        ; this lies safely inside the existing [-15,+15] LUT domain.
        add     a, a
        add     a, a
        add     a, a
        ld      c, a
        ld      hl, (#_g_raster_ctx + 2)
        ld      a, l
        sub     c
        add     a, #15
        ld      (#sf_local_index$), a

        ; group=slope+7; index=group*31+local.
        ld      a, (#sf_slope$)
        add     a, #7
        ld      e, a
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
        ld      a, (#sf_local_index$)
        ld      e, a
        ld      d, #0
        add     hl, de
        add     hl, hl
        ld      de, #_ts_edge_lut
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)

        ; Shade families are +0x80/+0x100 tile indices.
        ld      a, (#_g_raster_ctx + 1)
        or      a
        jr      z, sf_edge_ready$
        dec     a
        jr      nz, sf_edge_shade2$
        ld      a, e
        add     a, #0x80
        ld      e, a
        jr      nc, sf_edge_ready$
        inc     d
        jr      sf_edge_ready$
sf_edge_shade2$:
        inc     d
sf_edge_ready$:
        ret

; A=row, DE=word. Ignore rows outside the current aperture; preserve DE so the
; caller can immediately derive the mirrored bottom word.
sf_store_if_clip$:
        push    de
        ld      c, a
        ld      a, (#sf_clip_first$)
        cp      c
        jr      c, sf_store_clip_low_ok$
        jr      z, sf_store_clip_low_ok$
        pop     de
        ret
sf_store_clip_low_ok$:
        ld      a, (#sf_clip_last$)
        cp      c
        jr      nc, sf_store_clip_ok$
        pop     de
        ret
sf_store_clip_ok$:
        ld      a, c
        call    sf_store_word$
        pop     de
        ret

; A=row, DE=visible word.
sf_store_word$:
        ld      (#sf_store_row$), a
        ld      (#sf_word$), de
        call    sf_map_ptr_for_row$
        ld      de, (#sf_word$)
        ld      a, (hl)
        cp      e
        jr      nz, sf_word_changed$
        inc     hl
        ld      a, (hl)
        cp      d
        ret     z
        dec     hl
sf_word_changed$:
        ld      (hl), e
        inc     hl
        ld      (hl), d
        call    sf_dirty_store_row$
        ret

; Uses sf_store_row/sf_col. Expand Stage-20 horizontal dirty extent.
; Preserve B because fill uses DJNZ.
sf_dirty_store_row$:
        push    bc
        ld      a, (#sf_store_row$)
        ld      e, a
        ld      d, #0
        ld      hl, #_g_nt_row_min
        add     hl, de
        ld      a, (#sf_col$)
        ld      c, a
        ld      a, (hl)
        cp      #0xff
        jr      z, sf_dirty_set_min$
        ld      a, c
        cp      (hl)
        jr      nc, sf_dirty_min_done$
sf_dirty_set_min$:
        ld      (hl), c
sf_dirty_min_done$:
        ld      de, #18
        add     hl, de
        ld      a, c
        cp      (hl)
        jr      c, sf_dirty_done$
        jr      z, sf_dirty_done$
        ld      (hl), a
sf_dirty_done$:
        pop     bc
        ret

; A=row -> HL=&g_map[row*20 + column].
sf_map_ptr_for_row$:
        add     a, a
        ld      l, a
        ld      h, #0
        ld      de, #sf_map_rows$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ex      de, hl
        ld      a, (#sf_col$)
        add     a, a
        ld      e, a
        ld      d, #0
        add     hl, de
        ret

; Signed floor(y/8), returned in A.
sf_row_floor_hl$:
        sra     h
        rr      l
        sra     h
        rr      l
        sra     h
        rr      l
        ld      a, l
        ret

        .area _CODE
sf_map_rows$:
        .dw _g_map+0,   _g_map+40,  _g_map+80,  _g_map+120, _g_map+160, _g_map+200
        .dw _g_map+240, _g_map+280, _g_map+320, _g_map+360, _g_map+400, _g_map+440
        .dw _g_map+480, _g_map+520, _g_map+560, _g_map+600, _g_map+640, _g_map+680

        .area _BSS
sf_col$:          .ds 1
sf_clip_first$:   .ds 1
sf_clip_last$:    .ds 1
sf_top_l_row$:    .ds 1
sf_top_r_row$:    .ds 1
sf_top_min$:      .ds 1
sf_top_max$:      .ds 1
sf_slope$:        .ds 1
sf_edge_row$:     .ds 1
sf_edge_last$:    .ds 1
sf_tmp_row$:      .ds 1
sf_local_index$:  .ds 1
sf_store_row$:    .ds 1
sf_word$:         .ds 2
sf_cov_first$:    .ds 1
sf_fill_first$:   .ds 1
sf_full_tile$:    .ds 1
'''

core_p.write_text(core)
run_p.write_text(run)
sym_p.write_text(sym)
print('Applied Stage 21 exact symmetric FULL-wall geometry + one-edge materializer.')
