        .title  "Polar GG fast materializer"
        .module tilesector_polar_materialize_gg

        .area   _HOME

        .globl  _g_polar_mat_col
        .globl  _g_polar_mat_shade
        .globl  _g_polar_mat_border
        .globl  _g_polar_mat_top_l
        .globl  _g_polar_mat_top_r
        .globl  _g_polar_mat_bot_l
        .globl  _g_polar_mat_bot_r
        .globl  _g_polar_run_c0
        .globl  _g_polar_run_c1
        .globl  _g_polar_run_profile
        .globl  _g_polar_run_left_real
        .globl  _g_polar_run_right_real
        .globl  _g_polar_run_iq
        .globl  _g_polar_run_step
        .globl  _g_polar_run_sid
        .globl  _g_polar_run_left_anchor
        .globl  _g_polar_run_right_anchor
        .globl  _g_polar_run_owned
        .globl  _g_polar_nt_cov_cur
        .globl  _g_polar_nt_row_min
        .globl  _g_polar_nt_row_max
        .globl  _g_map
        .globl  _tsp_polar_p_span
        .globl  _tsp_polar_p_edge
        .globl  _tsp_polar_p_cap
        .globl  _tsp_polar_p_fill
        .globl  _tsp_polar_p_symtop
        .globl  _tsp_polar_p_symbot
; Read-only probe aliases for the materializer census. These are LABELS on
; existing storage, not new state: no instruction is added, moved or changed,
; so a census build is cycle-identical to the shipping one.
        .globl  _tsp_probe_fill_first
        .globl  _tsp_probe_full_tile
        .globl  _tsp_probe_unclaimed0
        .globl  _tsp_probe_unclaimed1
        .globl  _tsp_probe_unclaimed2
        .globl  _tsp_probe_row
        .globl  _tsp_probe_top_min
        .globl  _tsp_probe_top_max
        .globl  _tsp_probe_bot_min
        .globl  _tsp_probe_bot_max
        .globl  _tsp_probe_occluded
        .globl  _tsp_polar_p_fill_open
        .globl  _tsp_probe_end_fill
        .globl  _tsp_probe_end_fill_open
        .globl  _tsp_h_polar_mark_span_fast
        .globl  _tsp_h_polar_set_span_owned_fast
        .globl  _tsp_h_polar_row_unclaimed_fast
        .globl  _tsp_h_polar_mark_dirty_fast
        .globl  _tsp_h_map_ptr_row_col
        .globl  _tsp_h_full_tile_low
        .globl  _tsp_h_row_floor_hl
        .globl  _tsp_h_profile_half
        .globl  _tsp_h_full_top_half
        .globl  _tsp_h_full_q6_top_row
        .globl  _tsp_h_q6_round_u8
        .globl  _tsp_h_prepare_edge
        .globl  _tsp_h_prepare_symfull_edges
        .globl  _tsp_h_draw_symfull_edge_pair
; Retained swept-boundary path (rung 26).
        .globl  _tsp_polar_ret_begin_frame
        .globl  _tsp_polar_ret_end_frame
        .globl  _tsp_polar_ret_invalidate
        .globl  _tsp_h_ret_column_gate
        .globl  _tsp_h_ret_try_patch
        .globl  _tsp_h_ret_patch_fill_run
        .globl  _tsp_h_ret_record_clean
        .globl  _tsp_probe_patch_hit
        .globl  _tsp_h_ret_column_kill
        .globl  _tsp_h_ret_run_begin
        .globl  _tsp_h_ret_bitmask
        .globl  _tsp_probe_ret_skip
        .globl  _g_ts_vblank_pending
        .globl  _tsp_polar_service_vblank
        .globl  _g_tspf_seam_count
        .globl  _g_tspf_seam_x
        .globl  _g_tspf_seam_half
        .globl  _g_tspf_seam_pending_dx
        .globl  _g_tspf_seam_pending_c0
        .globl  _g_tspf_seam_pending_half
        .globl  _g_tspf_seam_mask_for_index
        .globl  _g_tspf_seam_reflect_for_index
        .globl  _tsp_polar_record_subcolumn_seam
        .globl  _tsp_polar_subcolumn_seams_fast

; Explicit polar materializer bridge. No C struct offsets and no argument-register
; convention: every input is a named symbol, and the visible aperture is always
; the full 18-row GG viewport.

; Geometry-only connected-run path. C supplies run bounds plus Q6 inverse-depth
; start/step once. Z80 walks all covered coarse columns without returning to C.
; It intentionally feeds the already-validated single-column hardware emitter,
; so the only new behavior here is run traversal and exact profile-Y arithmetic.
_tsp_polar_run_geometry_fast::
        push    bc
        push    de
        push    hl

        ld      a, (#_g_polar_run_c0)
        ld      (#r_run_col$), a
        ld      a, #1
        ld      (#_g_polar_mat_shade), a
        call    ret_run_begin$

run_geom_loop$:
        ; Connected corner may supply a canonical physical-vertex height for
        ; this run's first LEFT endpoint. High bit is validity; low 7 bits are
        ; FULL half-height. Only the first column can consume it.
        ld      a, (#r_run_col$)
        ld      c, a
        ld      a, (#_g_polar_run_c0)
        cp      c
        jr      nz, run_left_q6$
        ld      a, (#_g_polar_run_left_anchor)
        bit     7, a
        jr      z, run_left_q6$
        and     #0x7f
        call    full_top_half$
        jr      run_left_ready$
run_left_q6$:
        ; FULL-only fused projection endpoint: round Q6 inverse depth, halve it,
        ; construct top=71-half, and derive floor(top/8) while the signed top
        ; byte is already live.  The old path spilled invl/invr to RAM and then
        ; reloaded them before calling separate top and row helpers.
        ld      hl, (#_g_polar_run_iq)
        ld      de, #32
        add     hl, de
        call    full_q6_top_row$        ; A=row, C=half, HL=signed top
run_left_ready$:
        ld      (#r_top_l_row$), a
        ld      a, c
        ld      (#r_run_halfl$), a
        ld      (#_g_polar_mat_top_l), hl

        ; R94 endpoint lock. Interior right edges use the compact Q6 step,
        ; but the LAST right edge uses the exact b endpoint already calculated
        ; by the depth bank. This removes accumulated reciprocal/step error at
        ; physical run boundaries without changing the rest of the plane.
        ld      a, (#r_run_col$)
        ld      c, a
        ld      a, (#_g_polar_run_c1)
        cp      c
        jr      z, run_right_exact$
        ld      hl, (#_g_polar_run_iq)
        ld      de, (#_g_polar_run_step)
        add     hl, de
        ld      de, #32
        add     hl, de
        call    full_q6_top_row$
        jr      run_right_ready$
run_right_exact$:
        ld      a, (#_g_polar_run_right_anchor)
        and     #0x7f
        call    full_top_half$
run_right_ready$:
        ld      (#r_top_r_row$), a
        ld      a, c
        ld      (#r_run_halfr$), a
        ld      (#_g_polar_mat_top_r), hl

        ; Physical-chain border bits: 1 at the true left endpoint, 2 at the
        ; true right endpoint. Interior coarse columns carry no border bits.
        xor     a
        ld      (#_g_polar_mat_border), a
        ld      a, (#r_run_col$)
        ld      c, a
        ld      a, (#_g_polar_run_c0)
        cp      c
        jr      nz, run_no_left_border$
        ld      a, (#_g_polar_run_left_real)
        or      a
        jr      z, run_no_left_border$
        ld      a, #1
        ld      (#_g_polar_mat_border), a
run_no_left_border$:
        ld      a, (#_g_polar_run_c1)
        cp      c
        jr      nz, run_border_done$
        ld      a, (#_g_polar_run_right_real)
        or      a
        jr      z, run_border_done$
        ld      a, (#_g_polar_mat_border)
        or      #2
        ld      (#_g_polar_mat_border), a
run_border_done$:
        ld      a, c
        ld      (#_g_polar_mat_col), a
        call    _tsp_polar_surface_column_fast

        ; R84 cooperative VBlank yield.  The ISR only sets one byte; test it
        ; here after a complete column so the authoritative name-table word and
        ; dirty extents are always coherent before any VRAM publication.  The
        ; normal path costs only a load/test/untaken branch.  The service itself
        ; runs in HOME and preserves our live traversal registers explicitly.
        ld      a, (#_g_ts_vblank_pending)
        or      a
        jr      z, run_no_vblank_service$
        push    bc
        push    de
        push    hl
        call    _tsp_polar_service_vblank
        pop     hl
        pop     de
        pop     bc
run_no_vblank_service$:

        ; iq += step for the next coarse column.
        ld      hl, (#_g_polar_run_iq)
        ld      de, (#_g_polar_run_step)
        add     hl, de
        ld      (#_g_polar_run_iq), hl
        ld      hl, (#r_ret_ptr$)
        ld      de, #8
        add     hl, de
        ld      (#r_ret_ptr$), hl

        ld      a, (#r_run_col$)
        ld      c, a
        ld      a, (#_g_polar_run_c1)
        cp      c
        jp      z, run_geom_done$
        ld      a, c
        inc     a
        ld      (#r_run_col$), a
        jp      run_geom_loop$

run_geom_done$:
        pop     hl
        pop     de
        pop     bc
        ret

; HL = Q6 accumulator + rounding bias. Run interpolation begins from
; uint8 inverse-depth endpoints, so the normal hot path is non-negative.
; Convert positive 16-bit /64 directly from H:L instead of six shift pairs.
; Keep the signed fallback for defensive compatibility with future callers.
q6_round_u8$:
_tsp_h_q6_round_u8::
        bit     7, h
        jr      nz, q6_signed_slow$
        ld      a, h
        cp      #64
        jr      nc, q6_overflow$
        add     a, a
        add     a, a
        ld      c, a
        ld      a, l
        rlca
        rlca
        and     #3
        or      c
        ret
q6_signed_slow$:
        sra     h
        rr      l
        sra     h
        rr      l
        sra     h
        rr      l
        sra     h
        rr      l
        sra     h
        rr      l
        sra     h
        rr      l
        bit     7, h
        jr      nz, q6_negative$
        ld      a, h
        or      a
        jr      nz, q6_overflow$
        ld      a, l
        ret
q6_negative$:
        xor     a
        ret
q6_overflow$:
        ld      a, #255
        ret

; FULL-only exact-envelope fused helper.
; Input HL is non-negative Q6 inverse depth PLUS the rounding bias 32.
; Exact depth interpolation stays between its two uint8 endpoints, so this
; hot path can neither be negative nor exceed 255 after >>6.  Outputs:
;   C = half height = rounded_inv >> 1
;   HL = signed top pixel = 71-half
;   A = signed tile row = floor(top/8)
full_q6_top_row$:
_tsp_h_full_q6_top_row::
        ; Positive 16-bit >>6 without six shift pairs.
        ld      a, h
        add     a, a
        add     a, a
        ld      c, a
        ld      a, l
        rlca
        rlca
        and     #3
        or      c                       ; rounded inverse depth
        srl     a                       ; FULL half height
        ld      c, a
        ld      a, #71
        sub     c
        ld      l, a
        ld      h, #0
        bit     7, l
        jr      z, full_q6_sign_ready$
        dec     h
full_q6_sign_ready$:
        sra     a
        sra     a
        sra     a
        ret

; A = FULL half height (inv>>1, 0..127). Returns signed HL=71-half and row A.
; Retained for generic/reference call sites; hot exact-envelope traversal uses
; full_q6_top_row$ above.
full_top_half$:
_tsp_h_full_top_half::
        ld      c, a
        ld      a, #71
        sub     c
        ld      l, a
        ld      h, #0
        bit     7, l
        jr      z, full_top_sign_ready$
        dec     h
full_top_sign_ready$:
        ; A still holds the signed low byte of top. All FULL tops fit in int8,
        ; so three arithmetic shifts are the exact signed floor(top/8) row.
        sra     a
        sra     a
        sra     a
        ret

; Legacy generic profile helper retained for non-benchmark/reference callers.
; The orthogonal FULL benchmark hot path above no longer calls it.
; A = half height (inv>>1, 0..127)
; Returns HL=top pixel Y, DE=bottom pixel Y using the exact C profile formulas.
; Profiles: 0 FULL, 1 LINTEL, 2 RAISED, 3 RISER.
profile_half$:
_tsp_h_profile_half::
        ld      (#r_run_half$), a

        ; POLAR_STAGE21_FULL_SYMMETRY_A: exact physical-screen mirror.
        ; FULL top = 71-half, while bottom remains 72+half, so bottom=143-top.
        ld      a, #71
        ld      c, a
        ld      a, (#r_run_half$)
        ld      b, a
        ld      a, c
        sub     b
        ld      l, a
        ld      h, #0
        bit     7, l
        jr      z, profile_top_default_ready$
        dec     h
profile_top_default_ready$:
        ; Default FULL bottom = 72+half (always 0..199).
        ld      a, #72
        add     a, b
        ld      e, a
        ld      d, #0

        ld      a, (#_g_polar_run_profile)
        or      a
        ret     z                       ; FULL is overwhelmingly hot
        cp      #1
        jr      z, profile_lintel$
        cp      #2
        jr      z, profile_raised$
        cp      #3
        ret     nz

        ; RISER top = 72 + half - (half>>2).
        ld      a, b
        srl     a
        srl     a
        ld      c, a
        ld      a, #72
        add     a, b
        sub     c
        ld      l, a
        ld      h, #0
        ret

profile_lintel$:
        ; bottom = 72 - (half>>1).
        ld      a, b
        srl     a
        ld      c, a
        ld      a, #72
        sub     c
        ld      e, a
        ld      d, #0
        ret

profile_raised$:
        ; bottom = 72 + half - (half>>2).
        ld      a, b
        srl     a
        srl     a
        ld      c, a
        ld      a, #72
        add     a, b
        sub     c
        ld      e, a
        ld      d, #0
        ret

_tsp_polar_surface_column_fast::
        push    bc
        push    de
        push    hl
        ld      a, (#_g_polar_mat_col)
        ld      b, a

        xor     a
        ld      (#r_clip_first$), a
        ld      a, #17
        ld      (#r_clip_last$), a

        ; FULL-only run traversal already produced exact signed top rows while
        ; constructing the pixel endpoints. Bottom rows are always 17-top.
polar_endpoint_rows_ready$:

        ; A foreground FULL wall whose top edge moved a row or two needs a few
        ; tile writes, not a column rebuild. Try that before paying for the
        ; ownership mask and the generic raster.
        call    ret_try_patch$
_tsp_probe_patch_hit::
        jp      z, raster_done$

        ; Signed min/max for top endpoints.
        ld      a, (#r_top_l_row$)
        ld      e, a
        xor     #0x80
        ld      c, a
        ld      a, (#r_top_r_row$)
        ld      d, a
        xor     #0x80
        cp      c
        jr      c, top_r_is_min$
        ld      a, e
        ld      (#r_top_min$), a
        ld      a, d
        ld      (#r_top_max$), a
        jr      top_minmax_done$
top_r_is_min$:
        ld      a, d
        ld      (#r_top_min$), a
        ld      a, e
        ld      (#r_top_max$), a
top_minmax_done$:

        ; FULL-only exact mirror: bottom_min=17-top_max,
        ; bottom_max=17-top_min.
        ld      a, (#r_top_max$)
        ld      c, a
        ld      a, #17
        sub     c
        ld      (#r_bot_min$), a
        ld      a, (#r_top_min$)
        ld      c, a
        ld      a, #17
        sub     c
        ld      (#r_bot_max$), a
        jp      bot_minmax_done$

polar_bot_generic$:
        ld      a, (#r_bot_l_row$)
        ld      e, a
        xor     #0x80
        ld      c, a
        ld      a, (#r_bot_r_row$)
        ld      d, a
        xor     #0x80
        cp      c
        jr      c, bot_r_is_min$
        ld      a, e
        ld      (#r_bot_min$), a
        ld      a, d
        ld      (#r_bot_max$), a
        jr      bot_minmax_done$
bot_r_is_min$:
        ld      a, d
        ld      (#r_bot_min$), a
        ld      a, e
        ld      (#r_bot_max$), a
bot_minmax_done$:

        ; Exact-envelope FULL ownership has only nine visible coverage shapes.
        ; For top_min<=0 the wall owns all 18 rows; top_min 1..8 owns the
        ; mirrored span top_min..17-top_min; top_min>=9 owns nothing.  Baked
        ; first-hit ownership means no overlap solve is required.
        ld      a, (#_g_polar_run_owned)
        or      a
        jr      z, polar_cov_generic_prepare$
        ld      a, (#r_top_min$)
        bit     7, a
        jr      z, polar_cov_full_nonneg$
        xor     a
polar_cov_full_nonneg$:
        cp      #9
        jr      nc, ret_kill_cov_done$
        call    polar_set_full_owned_fast$
        jr      polar_cov_mark_done$

polar_cov_generic_prepare$:
        ; Legacy/general path: clip an arbitrary contiguous owned span.
        ld      a, (#r_top_min$)
        bit     7, a
        jr      z, polar_cov_first_nonneg$
        xor     a
polar_cov_first_nonneg$:
        cp      #18
        jr      nc, ret_kill_cov_done$
        ld      e, a                   ; E=first visible owned row

        ld      a, (#r_bot_max$)
        bit     7, a
        jr      nz, ret_kill_cov_done$
        cp      #18
        jr      c, polar_cov_last_ready$
        ld      a, #17
polar_cov_last_ready$:
        ld      c, a                   ; C=last visible owned row
        ld      a, e
        cp      c
        jr      c, polar_cov_generic_mark$
        jr      nz, ret_kill_cov_done$
polar_cov_generic_mark$:
        ld      a, e
        call    polar_mark_span_fast$   ; returns A=OR of previously-unclaimed rows
        jr      polar_cov_mark_done$

ret_kill_cov_done$:
        ; Nothing of this surface lands in this column, so the retained slot
        ; must not be able to answer for it next frame.
        call    ret_column_kill$
        jr      polar_cov_done$
polar_cov_mark_done$:
_tsp_polar_p_span::
        or      a
        jr      nz, ret_gate_live$
        call    ret_column_kill$        ; wholly occluded: nothing written here
        jp      raster_done$
ret_gate_live$:
        ; Retained swept boundary: this surface's contribution to this coarse
        ; column is fully determined by (invl, invr, border, unclaimed[3]).
        ; If that key is bit-identical to the one it produced last frame, the
        ; cells it would write already hold the answer, so the whole raster
        ; is dead work. Coverage is already marked above, so nt_end_frame
        ; still sees this column as owned and will not restore it.
        call    ret_column_gate$
_tsp_probe_ret_skip::
        jp      z, raster_done$
polar_cov_done$:

        ; FULL-only benchmark: calculate each top edge word once and emit
        ; its floor partner with VFLIP+palette. No runtime profile dispatch.
        jp      polar_draw_symfull$

polar_draw_symfull$:
        call    prepare_symfull_edges$
        call    draw_plain_interior$
        call    ret_record_clean$
        jr      raster_done$

raster_done$:
        pop     hl
        pop     de
        pop     bc
        ret

; Signed floor(pixel/8). Projected endpoints normally fit a sign-extended
; byte (FULL top) or positive byte (bottom / special profiles). Handle those
; directly in A; retain the 16-bit shift fallback for any future wider value.
row_floor_hl$:
_tsp_h_row_floor_hl::
        ld      a, h
        or      a
        jr      z, row_floor_pos8$
        inc     a
        jr      z, row_floor_neg8$
        ; wider signed fallback
        sra     h
        rr      l
        sra     h
        rr      l
        sra     h
        rr      l
        ld      a, l
        ret
row_floor_pos8$:
        ld      a, l
        srl     a
        srl     a
        srl     a
        ret
row_floor_neg8$:
        ld      a, l
        sra     a
        sra     a
        sra     a
        ret

; POLAR_STAGE21_FULL_VFLIP
; FULL walls use one top-edge solution. For every visible top edge row R, the
; exact floor partner is row 17-R and the same tile word ORed with
; VFLIP|palette (high-byte 0x0C). The unclaimed mask is from the state before
; this surface entered, so top/bottom members can be tested independently.
prepare_symfull_edges$:
_tsp_h_prepare_symfull_edges::
        ld      hl, (#_g_polar_mat_top_l)
        ld      (#r_edge_left$), hl
        ld      de, (#_g_polar_mat_top_r)
        ex      de, hl                  ; HL=right, DE=left
        or      a
        sbc     hl, de
        ld      a, l
        bit     7, a
        jr      nz, sym_slope_negative$
        cp      #8
        jr      c, sym_slope_store$
        ld      a, #7
        jr      sym_slope_store$
sym_slope_negative$:
        cp      #0xF9
        jr      nc, sym_slope_store$
        ld      a, #0xF9
sym_slope_store$:
        ld      (#r_edge_slope$), a
        ld      a, (#r_top_min$)
sym_edge_rows_loop$:
        ld      (#r_edge_iter$), a
        call    draw_symfull_edge_pair$
        ld      a, (#r_edge_iter$)
        ld      c, a
        ld      a, (#r_top_max$)
        cp      c
        ret     z
        ld      a, c
        inc     a
        jr      sym_edge_rows_loop$

; A=signed TOP tile row.
draw_symfull_edge_pair$:
_tsp_h_draw_symfull_edge_pair::
        ld      (#r_row$), a
        bit     7, a
        ret     nz
        cp      #18
        ret     nc

        ld      c, a
        ld      a, #17
        sub     c
        ld      (#r_sym_bottom_row$), a

        ; Check both mirrored rows before paying for the LUT lookup -- unless
        ; nothing was claimed, in which case both are this surface's by
        ; construction and the two queries are pure overhead.
        ld      a, (#r_occluded$)
        or      a
        jr      nz, sym_query_rows$
        ld      a, #1
        ld      (#r_sym_top_draw$), a
        ld      (#r_sym_bot_draw$), a
        ld      c, a                   ; leave C as the query path leaves it
        jr      sym_rows_ready$
sym_query_rows$:
        ld      a, c
        call    polar_row_unclaimed_fast$
_tsp_polar_p_symtop::
        ld      (#r_sym_top_draw$), a
        ld      a, (#r_sym_bottom_row$)
        call    polar_row_unclaimed_fast$
_tsp_polar_p_symbot::
        ld      (#r_sym_bot_draw$), a
        ld      c, a
        ld      a, (#r_sym_top_draw$)
        or      c
        ret     z
sym_rows_ready$:

        ; Top local coordinate and canonical top-edge LUT index.
        ld      a, (#r_row$)
        add     a, a
        add     a, a
        add     a, a
        ld      e, a
        ld      a, (#r_edge_left$)
        sub     e
        cp      #0x80
        jr      c, sym_local_positive$
        cp      #0xF1
        jr      nc, sym_local_ready$
        ld      a, #0xF1
        jr      sym_local_ready$
sym_local_positive$:
        cp      #16
        jr      c, sym_local_ready$
        ld      a, #15
sym_local_ready$:
        add     a, #15
        ld      (#r_local_index$), a

        ld      a, (#r_edge_slope$)
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
        add     hl, hl                  ; *32
        pop     de
        or      a
        sbc     hl, de                  ; *31
        ld      a, (#r_local_index$)
        ld      e, a
        ld      d, #0
        add     hl, de
        add     hl, hl
        ld      de, #edge_lut$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)

        ; Same shade-family adjustment as the generic edge path.
        ld      a, (#_g_polar_mat_shade)
        or      a
        jr      z, sym_edge_word_ready$
        dec     a
        jr      nz, sym_edge_shade_two$
        ld      a, e
        add     a, #0x80
        ld      e, a
        jr      nc, sym_edge_word_ready$
        inc     d
        jr      sym_edge_word_ready$
sym_edge_shade_two$:
        inc     d
sym_edge_word_ready$:
        ld      (#r_sym_word$), de

        ld      a, (#r_sym_top_draw$)
        or      a
        jr      z, sym_skip_top_store$
        ld      de, (#r_sym_word$)
        ld      a, (#r_row$)
        call    sym_store_word$
sym_skip_top_store$:
        ld      a, (#r_sym_bot_draw$)
        or      a
        ret     z
        ld      de, (#r_sym_word$)
        ld      a, d
        or      #0x0c                  ; VFLIP | palette 1
        ld      d, a
        ld      a, (#r_sym_bottom_row$)
        call    sym_store_word$
        ret

; A=row, DE=final word. B remains the current hardware column.
sym_store_word$:
        ld      (#r_sym_store_row$), a
        push    de
        call    map_ptr_row_col$
        pop     de
        ld      a, (hl)
        cp      e
        jr      nz, sym_word_changed$
        inc     hl
        ld      a, (hl)
        cp      d
        ret     z
        dec     hl
sym_word_changed$:
        ld      (hl), e
        inc     hl
        ld      (hl), d
        ld      a, (#r_sym_store_row$)
        call    polar_mark_dirty_fast$
        ret

; A=0 top / 1 bottom. Prepares original edge endpoints/slope then draws the
; one or two hardware-tile rows that a <=7px connected edge can cross.
prepare_edge$:
_tsp_h_prepare_edge::
        ld      (#r_edge_bottom$), a
        or      a
        jr      nz, prep_bottom$
        ld      hl, (#_g_polar_mat_top_l)
        ld      (#r_edge_left$), hl
        ld      de, (#_g_polar_mat_top_r)
        ld      a, (#r_top_min$)
        ld      (#r_edge_min$), a
        ld      a, (#r_top_max$)
        ld      (#r_edge_max$), a
        jr      prep_slope$
prep_bottom$:
        ld      hl, (#_g_polar_mat_bot_l)
        ld      (#r_edge_left$), hl
        ld      de, (#_g_polar_mat_bot_r)
        ld      a, (#r_bot_min$)
        ld      (#r_edge_min$), a
        ld      a, (#r_bot_max$)
        ld      (#r_edge_max$), a
prep_slope$:
        ex      de, hl                  ; HL=right, DE=left
        or      a
        sbc     hl, de
        ld      a, l                    ; signed slope, clamp exactly like C
        bit     7, a
        jr      nz, slope_negative$
        cp      #8
        jr      c, slope_store$
        ld      a, #7
        jr      slope_store$
slope_negative$:
        cp      #0xF9                  ; -7
        jr      nc, slope_store$
        ld      a, #0xF9
slope_store$:
        ld      (#r_edge_slope$), a
        ; Polar path may cross more than two tile rows at steep/near
        ; perspective. Match the C oracle: draw every row from min..max while
        ; using the clamped [-7,+7] edge slope for tile selection.
        ld      a, (#r_edge_min$)
edge_rows_loop$:
        ld      (#r_edge_iter$), a
        call    draw_edge_row$
        ld      a, (#r_edge_iter$)
        ld      c, a
        ld      a, (#r_edge_max$)
        cp      c
        ret     z
        ld      a, c
        inc     a
        jr      edge_rows_loop$

; A=signed row. Reject offscreen/occluded rows, then table-lookup the exact
; edge tile+flip attributes and write it into g_map.
draw_edge_row$:
        ld      (#r_row$), a
        bit     7, a
        ret     nz
        cp      #18
        ret     nc
        ld      c, a
        ld      a, (#r_clip_first$)
        cp      c
        jr      c, edge_after_first$
        jr      z, edge_after_first$
        ret
edge_after_first$:
        ld      a, (#r_clip_last$)
        cp      c
        ret     c

        ; Near->far fast path: only rows that were unclaimed before this
        ; surface entered may materialize. Same-surface top/bottom overlap is
        ; intentionally allowed because the mask is not consumed per subdraw.
        ld      a, (#r_occluded$)
        or      a
        jr      z, edge_row_open$      ; no row of this span was already claimed
        ld      a, (#r_row$)
        call    polar_row_unclaimed_fast$
_tsp_polar_p_edge::
        ret     z
edge_row_open$:

        ; local = left_y - row*8; low-byte arithmetic is exact in this range.
        ld      a, c
        add     a, a
        add     a, a
        add     a, a
        ld      e, a
        ld      a, (#r_edge_left$)
        sub     e
        ; Conservative clamp into LUT local domain [-15,+15].
        cp      #0x80
        jr      c, local_positive$
        cp      #0xF1                  ; -15
        jr      nc, local_ready$
        ld      a, #0xF1
        jr      local_ready$
local_positive$:
        cp      #16
        jr      c, local_ready$
        ld      a, #15
local_ready$:
        add     a, #15
        ld      (#r_local_index$), a

        ; group = slope+7 + (bottom?15:0); index = group*31 + local_index.
        ld      a, (#r_edge_slope$)
        add     a, #7
        ld      e, a
        ld      a, (#r_edge_bottom$)
        or      a
        jr      z, group_ready$
        ld      a, e
        add     a, #15
        ld      e, a
group_ready$:
        ld      l, e
        ld      h, #0
        ld      d, #0
        push    de                    ; group
        add     hl, hl                ; 2
        add     hl, hl                ; 4
        add     hl, hl                ; 8
        add     hl, hl                ; 16
        add     hl, hl                ; 32
        pop     de
        or      a
        sbc     hl, de                ; 31*group
        ld      a, (#r_local_index$)
        ld      e, a
        ld      d, #0
        add     hl, de
        add     hl, hl
        ld      de, #edge_lut$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)

        ; LUT is shade zero. Add 0x0080/0x0100 for mid/near families.
        ld      a, (#_g_polar_mat_shade)
        or      a
        jr      z, edge_tile_ready$
        dec     a
        jr      nz, edge_shade_two$
        ld      a, e
        add     a, #0x80
        ld      e, a
        jr      nc, edge_tile_ready$
        inc     d
        jr      edge_tile_ready$
edge_shade_two$:
        inc     d
edge_tile_ready$:
        push    de
        ld      a, (#r_row$)
        call    map_ptr_row_col$
        pop     de
        ld      a, (hl)
        cp      e
        jr      nz, polar_edge_changed$
        inc     hl
        ld      a, (hl)
        cp      d
        dec     hl
        jr      z, polar_edge_done$
polar_edge_changed$:
        ld      (hl), e
        inc     hl
        ld      (hl), d
        dec     hl
        ld      a, (#r_row$)
        call    polar_mark_dirty_fast$
polar_edge_done$:
        ld      a, (#r_row$)
        ret

; A=signed row, E=cap delta (4 top, 8 bottom).
draw_full_single$:
        ld      (#r_row$), a
        ld      a, e
        ld      (#r_cap_delta$), a
        ld      a, (#r_row$)
        bit     7, a
        ret     nz
        cp      #18
        ret     nc
        ld      c, a
        ld      a, (#r_clip_first$)
        cp      c
        jr      z, full_first_ok$
        jr      c, full_first_ok$
        ret
full_first_ok$:
        ld      a, (#r_clip_last$)
        cp      c
        ret     c
        ld      a, (#r_occluded$)
        or      a
        jr      z, cap_row_open$
        ld      a, (#r_row$)
        call    polar_row_unclaimed_fast$
_tsp_polar_p_cap::
        ret     z
cap_row_open$:
        ld      a, (#r_row$)
        call    map_ptr_row_col$
        call    full_tile_low$
        ld      e, a
        ld      a, (#r_cap_delta$)
        add     a, e
        ld      e, a
        ld      d, #0
        ld      a, (hl)
        cp      e
        jr      nz, polar_full_changed$
        inc     hl
        ld      a, (hl)
        or      a
        dec     hl
        ret     z
polar_full_changed$:
        ld      (hl), e
        inc     hl
        ld      (hl), #0
        dec     hl
        ld      a, (#r_row$)
        call    polar_mark_dirty_fast$
        ret

; Fill top_max+1 .. bot_min-1, clipped to the portal aperture.
draw_plain_interior$:
        ld      a, (#r_top_max$)
        inc     a
        bit     7, a
        jr      nz, interior_first_clip$
        ld      c, a
        ld      a, (#r_clip_first$)
        cp      c
        jr      c, interior_first_keep$
        jr      z, interior_first_keep$
interior_first_clip$:
        ld      a, (#r_clip_first$)
        ld      c, a
interior_first_keep$:
        ld      a, c
        cp      #18
        ret     nc
        ld      (#r_fill_first$), a

        ld      a, (#r_bot_min$)
        dec     a
        bit     7, a
        ret     nz
        ld      c, a
        ld      a, (#r_clip_last$)
        cp      c
        jr      nc, interior_last_keep$
        ld      c, a
interior_last_keep$:
        ld      a, c
        cp      #18
        jr      c, interior_last_valid$
        ld      a, #17
        ld      c, a
interior_last_valid$:
        ld      a, (#r_fill_first$)
        cp      c
        jr      c, interior_multi$
        jr      z, interior_multi$
        ret
interior_multi$:
        ; count=last-first+1
        ld      a, c
        ld      e, a
        ld      a, (#r_fill_first$)
        ld      d, a
        ld      a, e
        sub     d
        inc     a
        ld      c, a
        ld      a, (#r_fill_first$)
        ld      (#r_row$), a
        call    map_ptr_row_col$
        call    full_tile_low$
        ld      (#r_full_tile$), a
        ld      a, (#r_occluded$)
        or      a
        jr      z, interior_loop_open$
interior_loop$:
        push    hl
        ld      a, (#r_row$)
        call    polar_row_unclaimed_fast$
_tsp_polar_p_fill::
        pop     hl
        jr      z, polar_interior_done$
        ld      a, (#r_full_tile$)
        ld      e, a
        ld      d, #0
        ld      a, (hl)
        cp      e
        jr      nz, polar_interior_changed$
        inc     hl
        ld      a, (hl)
        or      a
        dec     hl
        jr      z, polar_interior_done$
polar_interior_changed$:
        ld      (hl), e
        inc     hl
        ld      (hl), #0
        dec     hl
        push    hl
        ld      a, (#r_row$)
        call    polar_mark_dirty_fast$
        pop     hl
polar_interior_done$:
        ld      de, #40              ; next RAM name-table row, same column
        add     hl, de
        ld      a, (#r_row$)
        inc     a
        ld      (#r_row$), a
        dec     c
        jr      nz, interior_loop$
        ret
_tsp_probe_end_fill::

; Open-interior fill. Reached when no row of this span was already claimed, so
; every interior row is this surface's to write and the per-row ownership query
; is pure overhead. The body below is the same store/compare/dirty sequence as
; interior_loop$ with that query removed; nothing else differs, which is what
; makes the name table byte-identical.
interior_loop_open$:
_tsp_polar_p_fill_open::
        ld      a, (#r_full_tile$)
        ld      e, a
        ld      a, (hl)
        cp      e
        jr      nz, polar_open_changed$
        inc     hl
        ld      a, (hl)
        or      a
        dec     hl
        jr      z, polar_open_done$
polar_open_changed$:
        ld      (hl), e
        inc     hl
        ld      (hl), #0
        dec     hl
        push    hl
        ld      a, (#r_row$)
        call    polar_mark_dirty_fast$
        pop     hl
polar_open_done$:
        ld      de, #40
        add     hl, de
        ld      a, (#r_row$)
        inc     a
        ld      (#r_row$), a
        dec     c
        jr      nz, interior_loop_open$
        ret
_tsp_probe_end_fill_open::

; Return low-byte full tile ID for current shade/border, cap none.
full_tile_low$:
_tsp_h_full_tile_low::
        ld      a, (#_g_polar_mat_shade)
        or      a
        jr      z, full_far$
        dec     a
        jr      z, full_mid$
        ld      a, #27
        jr      full_add_border$
full_mid$:
        ld      a, #15
        jr      full_add_border$
full_far$:
        ld      a, #3
full_add_border$:
        ld      e, a
        ld      a, (#_g_polar_mat_border)
        add     a, e
        ret

; A=row 0..17, B=current screen column. Expand the row's horizontal
; VDP interval directly. This preserves BC, including the interior-loop C count.
polar_mark_dirty_fast$:
_tsp_h_polar_mark_dirty_fast::
        ld      e, a
        ld      d, #0
        ld      hl, #_g_polar_nt_row_min
        add     hl, de
        ld      a, (hl)
        cp      #0xff
        jr      z, polar_dirty_set_min$
        ld      a, b
        cp      (hl)
        jr      nc, polar_dirty_min_done$
polar_dirty_set_min$:
        ld      (hl), b
polar_dirty_min_done$:
        ld      de, #18
        add     hl, de
        ld      a, b
        cp      (hl)
        jr      c, polar_dirty_done$
        jr      z, polar_dirty_done$
        ld      (hl), b
polar_dirty_done$:
        ret

; Baked front-envelope owner path. A=first row, C=last row, B=column.
; The ROM program already proved this is the one first-hit wall for the coarse
; screen column. Build the same three-byte span mask, but install it directly:
; no old-coverage loads, no AND/~old, no occlusion branches. r_unclaimed is the
; span itself so retained-key semantics remain unchanged.
polar_set_span_owned_fast$:
_tsp_h_polar_set_span_owned_fast::
        ld      (#r_cov_first$), a
        ld      a, c
        inc     a
        ld      (#r_cov_after$), a

        ld      a, (#r_cov_first$)
        ld      e, a
        add     a, a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #polar_prefix$
        add     hl, de
        ld      a, (hl)
        ld      (#r_cov_p0$), a
        inc     hl
        ld      a, (hl)
        ld      (#r_cov_p1$), a
        inc     hl
        ld      a, (hl)
        ld      (#r_cov_p2$), a

        ld      a, (#r_cov_after$)
        ld      e, a
        add     a, a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #polar_prefix$
        add     hl, de

        ; DE = cov_cur + column*3.
        push    hl
        ld      a, b
        ld      e, a
        add     a, a
        add     a, e
        ld      e, a
        ld      d, #0
        ld      hl, #_g_polar_nt_cov_cur
        add     hl, de
        ex      de, hl
        pop     hl

        ld      a, (hl)
        ld      c, a
        ld      a, (#r_cov_p0$)
        xor     c
        ld      (#r_unclaimed0$), a
        ex      de, hl
        ld      (hl), a
        inc     hl
        ex      de, hl

        inc     hl
        ld      a, (hl)
        ld      c, a
        ld      a, (#r_cov_p1$)
        xor     c
        ld      (#r_unclaimed1$), a
        ex      de, hl
        ld      (hl), a
        inc     hl
        ex      de, hl

        inc     hl
        ld      a, (hl)
        ld      c, a
        ld      a, (#r_cov_p2$)
        xor     c
        ld      (#r_unclaimed2$), a
        ex      de, hl
        ld      (hl), a

        xor     a
        ld      (#r_occluded$), a
        ld      a, (#r_unclaimed0$)
        ld      c, a
        ld      a, (#r_unclaimed1$)
        or      c
        ld      c, a
        ld      a, (#r_unclaimed2$)
        or      c
        ret

; A=FULL top_min class 0..8, B=column.
; Install the exact mirrored owned mask directly.  There are only nine shapes:
; 0 => rows 0..17, 1 => 1..16, ... 8 => row 8..9.
polar_set_full_owned_fast$:
_tsp_h_polar_set_full_owned_fast::
        ld      e, a
        add     a, a
        add     a, e                    ; A = shape*3
        ld      l, a
        ld      h, #0
        ld      de, #full_owned_mask$
        add     hl, de                  ; HL = baked mask triple

        push    hl
        ld      a, b
        ld      e, a
        add     a, a
        add     a, e                    ; A = column*3
        ld      e, a
        ld      d, #0
        ld      hl, #_g_polar_nt_cov_cur
        add     hl, de
        ex      de, hl                  ; DE = destination
        pop     hl                      ; HL = source

        ld      a, (hl)
        ld      (#r_unclaimed0$), a
        ex      de, hl
        ld      (hl), a
        inc     hl
        ex      de, hl
        inc     hl

        ld      a, (hl)
        ld      (#r_unclaimed1$), a
        ex      de, hl
        ld      (hl), a
        inc     hl
        ex      de, hl
        inc     hl

        ld      a, (hl)
        ld      (#r_unclaimed2$), a
        ex      de, hl
        ld      (hl), a

        xor     a
        ld      (#r_occluded$), a
        inc     a                       ; known non-empty mask => live column
        ret

; A=first owned row, C=last owned row, B=column. No register-save ceremony:
; this call sits at the column-materializer level where AF/C/DE/HL are scratch.
; span = prefix[last+1] XOR prefix[first], ORed into cov_cur[col*3].
polar_mark_span_fast$:
_tsp_h_polar_mark_span_fast::
        ld      (#r_cov_first$), a
        ld      a, c
        inc     a
        ld      (#r_cov_after$), a

        ; Save prefix[first].
        ld      a, (#r_cov_first$)
        ld      e, a
        add     a, a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #polar_prefix$
        add     hl, de
        ld      a, (hl)
        ld      (#r_cov_p0$), a
        inc     hl
        ld      a, (hl)
        ld      (#r_cov_p1$), a
        inc     hl
        ld      a, (hl)
        ld      (#r_cov_p2$), a

        ; HL = prefix[last+1].
        ld      a, (#r_cov_after$)
        ld      e, a
        add     a, a
        add     a, e
        ld      l, a
        ld      h, #0
        ld      de, #polar_prefix$
        add     hl, de
        xor     a
        ld      (#r_occluded$), a

        push    hl

        ; DE = cov_cur + column*3.
        ld      a, b
        ld      e, a
        add     a, a
        add     a, e
        ld      e, a
        ld      d, #0
        ld      hl, #_g_polar_nt_cov_cur
        add     hl, de
        ex      de, hl
        pop     hl

        ; For each byte: span=after^before; unclaimed=span&~old;
        ; current coverage becomes old|span.
        ld      a, (hl)
        ld      c, a
        ld      a, (#r_cov_p0$)
        xor     c
        ld      c, a                   ; C=span0
        ex      de, hl
        ld      a, (hl)
        ld      (#r_cov_old$), a
        cpl
        and     c
        ld      (#r_unclaimed0$), a
        ld      a, (#r_cov_old$)
        and     c                      ; rows this span wanted but already owned
        jr      z, polar_open_0$
        ld      a, #1
        ld      (#r_occluded$), a
polar_open_0$:
        ld      a, (#r_cov_old$)
        or      c
        ld      (hl), a
        inc     hl
        ex      de, hl

        inc     hl
        ld      a, (hl)
        ld      c, a
        ld      a, (#r_cov_p1$)
        xor     c
        ld      c, a
        ex      de, hl
        ld      a, (hl)
        ld      (#r_cov_old$), a
        cpl
        and     c
        ld      (#r_unclaimed1$), a
        ld      a, (#r_cov_old$)
        and     c                      ; rows this span wanted but already owned
        jr      z, polar_open_1$
        ld      a, #1
        ld      (#r_occluded$), a
polar_open_1$:
        ld      a, (#r_cov_old$)
        or      c
        ld      (hl), a
        inc     hl
        ex      de, hl

        inc     hl
        ld      a, (hl)
        ld      c, a
        ld      a, (#r_cov_p2$)
        xor     c
        ld      c, a
        ex      de, hl
        ld      a, (hl)
        ld      (#r_cov_old$), a
        cpl
        and     c
        ld      (#r_unclaimed2$), a
        ld      a, (#r_cov_old$)
        and     c                      ; rows this span wanted but already owned
        jr      z, polar_open_2$
        ld      a, #1
        ld      (#r_occluded$), a
polar_open_2$:
        ld      a, (#r_cov_old$)
        or      c
        ld      (hl), a

        ld      a, (#r_unclaimed0$)
        ld      c, a
        ld      a, (#r_unclaimed1$)
        or      c
        ld      c, a
        ld      a, (#r_unclaimed2$)
        or      c                       ; return NZ if anything new is visible
        ret

; A=row 0..17. Return A!=0/Z=0 only when this row was unclaimed before the
; current near->far surface claimed its complete span. Clobbers DE/HL only
; besides AF; B/C remain intact for column and fill-loop state.
polar_row_unclaimed_fast$:
_tsp_h_polar_row_unclaimed_fast::
        ld      (#r_claim_row$), a
        and     #7
        ld      l, a
        ld      h, #0
        ld      de, #polar_dirty_mask_lut$
        add     hl, de
        ld      e, (hl)                 ; E=row bit
        ld      a, (#r_claim_row$)
        cp      #8
        jr      c, polar_claim_g0$
        cp      #16
        jr      c, polar_claim_g1$
        ld      a, (#r_unclaimed2$)
        and     e
        ret
polar_claim_g1$:
        ld      a, (#r_unclaimed1$)
        and     e
        ret
polar_claim_g0$:
        ld      a, (#r_unclaimed0$)
        and     e
        ret

; X1 descriptor packer kept in fixed code so bank-255 only pays three
; bridge stores + one call. Internal connected joins always have c0 in 1..19,
; therefore (c0*8 + dx[-4,+3]) is already in the visible 0..159 domain.
_tsp_polar_record_subcolumn_seam::
        push    af
        push    bc
        push    de
        push    hl
        ld      a, (#_g_tspf_seam_count)
        cp      #32
        jr      nc, seam_record_done$
        ld      c, a                    ; descriptor index

        ld      a, (#_g_tspf_seam_pending_c0)
        add     a, a
        add     a, a
        add     a, a
        ld      b, a
        ld      a, (#_g_tspf_seam_pending_dx)
        add     a, b
        ld      e, c
        ld      d, #0
        ld      hl, #_g_tspf_seam_x
        add     hl, de
        ld      (hl), a

        ld      hl, #_g_tspf_seam_half
        add     hl, de
        ld      a, (#_g_tspf_seam_pending_half)
        ld      (hl), a

        ld      a, c
        inc     a
        ld      (#_g_tspf_seam_count), a
seam_record_done$:
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; X1 sub-column seam overlay.
; The renderer has already emitted ordinary FULL/EDGE cells for the frame and
; invalidated retained skips. Each descriptor is one physical connected corner:
;   seam_x   = exact screen X 0..159
;   seam_half= canonical FULL half-height at that corner
; We only replace FULL interior tiles, leaving top/bottom EDGE silhouettes
; untouched for this rung. Existing seam tiles are decoded and ORed so two
; acute corners may share one 8px hardware tile.
_tsp_polar_subcolumn_seams_fast::
        push    af
        push    bc
        push    de
        push    hl

        xor     a
        ld      (#r_seam_i$), a
seam_desc_loop$:
        ld      a, (#r_seam_i$)
        ld      c, a
        ld      a, (#_g_tspf_seam_count)
        cp      c
        jp      z, seam_overlay_done$

        ld      a, c
        ld      e, a
        ld      d, #0
        ld      hl, #_g_tspf_seam_x
        add     hl, de
        ld      a, (hl)
        ld      (#r_seam_x$), a
        and     #7
        ld      e, a
        ld      d, #0
        ld      hl, #seam_bit_lut$
        add     hl, de
        ld      a, (hl)
        ld      (#r_seam_mask$), a

        ld      a, (#r_seam_x$)
        srl     a
        srl     a
        srl     a
        ld      (#r_seam_col$), a

        ld      a, (#r_seam_i$)
        ld      e, a
        ld      d, #0
        ld      hl, #_g_tspf_seam_half
        add     hl, de
        ld      a, #71
        sub     (hl)                    ; signed top pixel = 71-half
        sra     a
        sra     a
        sra     a                       ; signed top tile row
        ld      c, a                    ; C = top row for last-row derivation

        inc     a                       ; first FULL interior row = top+1
        bit     7, a
        jr      z, seam_first_nonneg$
        xor     a
seam_first_nonneg$:
        cp      #18
        jp      nc, seam_next_desc$
        ld      (#r_seam_row$), a

        ld      a, #16                  ; bottom row - 1 = 16-top
        sub     c
        bit     7, a
        jp      nz, seam_next_desc$
        cp      #18
        jr      c, seam_last_ready$
        ld      a, #17
seam_last_ready$:
        ld      (#r_seam_last$), a
        ld      c, a
        ld      a, (#r_seam_row$)
        cp      c
        jr      c, seam_row_loop$
        jr      z, seam_row_loop$
        jp      seam_next_desc$

seam_row_loop$:
        ld      a, (#r_seam_col$)
        ld      b, a
        ld      a, (#r_seam_row$)
        call    map_ptr_row_col$

        ; Decode the current tile into a vertical-line mask. Ordinary
        ; geometry-only FULL-mid cap-none tiles are IDs 15..18, whose low two
        ; bits are the old left/right border flags. Seam tiles are 423..442
        ; (0x01A7..0x01BA). High byte 1 is canonical; 3 adds hardware HFLIP.
        inc     hl
        ld      a, (hl)
        dec     hl
        or      a
        jr      z, seam_decode_full$
        cp      #1
        jr      z, seam_decode_special_canon$
        cp      #3
        jr      z, seam_decode_special_flip$
        jr      seam_row_done$

seam_decode_special_canon$:
        ld      c, #0
        jr      seam_decode_special$
seam_decode_special_flip$:
        ld      c, #1
seam_decode_special$:
        ld      a, (hl)
        cp      #167
        jr      c, seam_row_done$
        cp      #187
        jr      nc, seam_row_done$
        sub     #167
        ld      e, a
        ld      d, #0
        push    hl
        ld      a, c
        or      a
        jr      nz, seam_special_reflected$
        ld      hl, #_g_tspf_seam_mask_for_index
        jr      seam_special_table$
seam_special_reflected$:
        ld      hl, #_g_tspf_seam_reflect_for_index
seam_special_table$:
        add     hl, de
        ld      a, (hl)
        pop     hl
        jr      seam_have_base_mask$

seam_decode_full$:
        ld      a, (hl)
        cp      #15
        jr      c, seam_row_done$
        cp      #19
        jr      nc, seam_row_done$
        sub     #15                      ; old border bits 0..3
        ld      e, #0
        bit     0, a
        jr      z, seam_no_left_old$
        ld      e, #1
seam_no_left_old$:
        bit     1, a
        jr      z, seam_full_mask_ready$
        ld      a, e
        or      #0x80
        ld      e, a
seam_full_mask_ready$:
        ld      a, e

seam_have_base_mask$:
        ld      e, a
        ld      a, (#r_seam_mask$)
        or      e
        ld      e, a                    ; E = merged one/two-line mask

        ; Convert the merged one/two-line mask to one of the 20 stored
        ; canonical patterns. Reflection is represented with hardware HFLIP;
        ; no 256-byte mask->code table is needed in scarce fixed ROM.
        call    seam_mask_to_code$       ; E=mask -> A=index, D=word high
        cp      #0xff                   ; >2 lines: leave proven tile untouched
        jr      z, seam_row_done$
        add     a, #167                 ; tile 423 low byte = 0xA7
        ld      e, a

        ld      a, (hl)
        cp      e
        jr      nz, seam_write_word$
        inc     hl
        ld      a, (hl)
        cp      d
        dec     hl
        jr      z, seam_row_done$
seam_write_word$:
        ld      (hl), e
        inc     hl
        ld      (hl), d
        dec     hl
        ld      a, (#r_seam_row$)
        call    polar_mark_dirty_fast$

seam_row_done$:
        ld      a, (#r_seam_row$)
        ld      c, a
        ld      a, (#r_seam_last$)
        cp      c
        jr      z, seam_next_desc$
        ld      a, c
        inc     a
        ld      (#r_seam_row$), a
        jp      seam_row_loop$

seam_next_desc$:
        ld      a, (#r_seam_i$)
        inc     a
        ld      (#r_seam_i$), a
        jp      seam_desc_loop$

seam_overlay_done$:
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; E = physical local-X mask containing one or two set bits.
; Returns A = canonical seam pattern index 0..19, D = name-table high byte:
;   1 = canonical orientation, 3 = HFLIP.
; A=0xff means unsupported (>2 lines / empty).
; B and HL are preserved because the caller still owns the map pointer/column.
seam_mask_to_code$:
        push    bc
        ld      a, e
        or      a
        jr      z, seam_code_invalid$

        ; Locate first set bit x.
        ld      c, #0
seam_code_find_x$:
        srl     a
        jr      c, seam_code_x_found$
        inc     c
        jr      seam_code_find_x$
seam_code_x_found$:
        ld      e, a
        ld      a, c
        ld      (#r_seam_x0$), a
        ld      a, e
        or      a
        jr      z, seam_code_single$

        ; Locate second set bit y. A already equals mask>>(x+1).
        ld      e, a
        ld      a, (#r_seam_x0$)
        inc     a
        ld      c, a
        ld      a, e
seam_code_find_y$:
        srl     a
        jr      c, seam_code_y_found$
        inc     c
        jr      seam_code_find_y$
seam_code_y_found$:
        ld      e, a
        ld      a, c
        ld      (#r_seam_y0$), a
        ld      a, e
        or      a
        jr      nz, seam_code_invalid$   ; a third line is outside X1 vocab

        ; Pair reflection maps (x,y) -> (7-y, 7-x). Choose whichever has the
        ; smaller first coordinate; equality implies a self-reflecting pair.
        ld      a, #7
        sub     c                        ; A = reflected x = 7-y
        ld      e, a
        ld      a, (#r_seam_x0$)
        cp      e
        jr      c, seam_code_pair_canon$
        jr      z, seam_code_pair_canon$

        ; Reflected pair is canonical.
        ld      d, #3
        ld      a, (#r_seam_x0$)
        ld      c, a
        ld      a, #7
        sub     c                        ; A = reflected y = 7-old_x
        ld      (#r_seam_y0$), a
        ld      a, e
        ld      (#r_seam_x0$), a
        jr      seam_code_pair_index$

seam_code_pair_canon$:
        ld      d, #1

seam_code_pair_index$:
        ; Stored pair ordering:
        ; x=0,y=1..7 -> 4..10
        ; x=1,y=2..6 -> 11..15
        ; x=2,y=3..5 -> 16..18
        ; x=3,y=4    -> 19
        ld      a, (#r_seam_x0$)
        or      a
        jr      nz, seam_code_pair_x1$
        ld      a, (#r_seam_y0$)
        add     a, #3
        jr      seam_code_return$
seam_code_pair_x1$:
        dec     a
        jr      nz, seam_code_pair_x2$
        ld      a, (#r_seam_y0$)
        add     a, #9
        jr      seam_code_return$
seam_code_pair_x2$:
        dec     a
        jr      nz, seam_code_pair_x3$
        ld      a, (#r_seam_y0$)
        add     a, #13
        jr      seam_code_return$
seam_code_pair_x3$:
        ld      a, #19
        jr      seam_code_return$

seam_code_single$:
        ; Singles x=0..3 are stored directly; x=4..7 use their mirror.
        ld      a, (#r_seam_x0$)
        cp      #4
        jr      c, seam_code_single_canon$
        ld      c, a
        ld      a, #7
        sub     c
        ld      d, #3
        jr      seam_code_return$
seam_code_single_canon$:
        ld      d, #1
        ; A is already x.
        jr      seam_code_return$

seam_code_invalid$:
        ld      a, #0xff
        ld      d, #1
seam_code_return$:
        pop     bc
        ret

seam_bit_lut$:
        .db 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80

; A=row 0..17, B=column 0..19 -> HL=&g_map[row*20+col].
map_ptr_row_col$:
_tsp_h_map_ptr_row_col::
        ld      l, a
        ld      h, #0
        add     hl, hl                ; 2r
        add     hl, hl                ; 4r
        add     hl, hl                ; 8r
        ld      d, h
        ld      e, l                  ; DE=8r
        add     hl, hl                ; 16r
        add     hl, hl                ; 32r
        add     hl, de                ; 40r bytes
        ld      a, b
        add     a, a
        ld      e, a
        ld      d, #0
        add     hl, de
        ld      de, #_g_map
        add     hl, de
        ret

polar_dirty_mask_lut$:
        .db 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80
; Exact FULL first-hit ownership masks indexed by top_min class 0..8.
; Bits cover 18 hardware tile rows, low row first.
full_owned_mask$:
        .db 0xff,0xff,0x03 ; 0..17
        .db 0xfe,0xff,0x01 ; 1..16
        .db 0xfc,0xff,0x00 ; 2..15
        .db 0xf8,0x7f,0x00 ; 3..14
        .db 0xf0,0x3f,0x00 ; 4..13
        .db 0xe0,0x1f,0x00 ; 5..12
        .db 0xc0,0x0f,0x00 ; 6..11
        .db 0x80,0x07,0x00 ; 7..10
        .db 0x00,0x03,0x00 ; 8..9

polar_prefix$:
        .db 0x00,0x00,0x00 ; rows < 0
        .db 0x01,0x00,0x00 ; rows < 1
        .db 0x03,0x00,0x00 ; rows < 2
        .db 0x07,0x00,0x00 ; rows < 3
        .db 0x0f,0x00,0x00 ; rows < 4
        .db 0x1f,0x00,0x00 ; rows < 5
        .db 0x3f,0x00,0x00 ; rows < 6
        .db 0x7f,0x00,0x00 ; rows < 7
        .db 0xff,0x00,0x00 ; rows < 8
        .db 0xff,0x01,0x00 ; rows < 9
        .db 0xff,0x03,0x00 ; rows < 10
        .db 0xff,0x07,0x00 ; rows < 11
        .db 0xff,0x0f,0x00 ; rows < 12
        .db 0xff,0x1f,0x00 ; rows < 13
        .db 0xff,0x3f,0x00 ; rows < 14
        .db 0xff,0x7f,0x00 ; rows < 15
        .db 0xff,0xff,0x00 ; rows < 16
        .db 0xff,0xff,0x01 ; rows < 17
        .db 0xff,0xff,0x03 ; rows < 18

; edge_lut[bottom][slope+7][local+15], shade-zero tile word.
; Each entry already contains H/V flip + palette attributes exactly as the
; C edge_entry() path; the assembly kernel only adds the shade tile offset.
edge_lut$:
        ; Fresh polar LUT generated from edge_entry(shade=0,local,slope,bottom).
        ; Index: bottom(0/1), slope -7..+7, raw local -15..+15.
        ; bottom=0 slope=-7
        .dw 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E
        .dw 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E
        .dw 0x0236, 0x023E, 0x0246, 0x024E, 0x0256, 0x025E, 0x0266, 0x026E
        .dw 0x0276, 0x027E, 0x0286, 0x028E, 0x0296, 0x029E, 0x02A6
        ; bottom=0 slope=-6
        .dw 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D
        .dw 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x0235
        .dw 0x023D, 0x0245, 0x024D, 0x0255, 0x025D, 0x0265, 0x026D, 0x0275
        .dw 0x027D, 0x0285, 0x028D, 0x0295, 0x029D, 0x02A5, 0x02A5
        ; bottom=0 slope=-5
        .dw 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C
        .dw 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x0234, 0x023C
        .dw 0x0244, 0x024C, 0x0254, 0x025C, 0x0264, 0x026C, 0x0274, 0x027C
        .dw 0x0284, 0x028C, 0x0294, 0x029C, 0x02A4, 0x02A4, 0x02A4
        ; bottom=0 slope=-4
        .dw 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B
        .dw 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x0233, 0x023B, 0x0243
        .dw 0x024B, 0x0253, 0x025B, 0x0263, 0x026B, 0x0273, 0x027B, 0x0283
        .dw 0x028B, 0x0293, 0x029B, 0x02A3, 0x02A3, 0x02A3, 0x02A3
        ; bottom=0 slope=-3
        .dw 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A
        .dw 0x022A, 0x022A, 0x022A, 0x022A, 0x0232, 0x023A, 0x0242, 0x024A
        .dw 0x0252, 0x025A, 0x0262, 0x026A, 0x0272, 0x027A, 0x0282, 0x028A
        .dw 0x0292, 0x029A, 0x02A2, 0x02A2, 0x02A2, 0x02A2, 0x02A2
        ; bottom=0 slope=-2
        .dw 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229
        .dw 0x0229, 0x0229, 0x0229, 0x0231, 0x0239, 0x0241, 0x0249, 0x0251
        .dw 0x0259, 0x0261, 0x0269, 0x0271, 0x0279, 0x0281, 0x0289, 0x0291
        .dw 0x0299, 0x02A1, 0x02A1, 0x02A1, 0x02A1, 0x02A1, 0x02A1
        ; bottom=0 slope=-1
        .dw 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228
        .dw 0x0228, 0x0228, 0x0230, 0x0238, 0x0240, 0x0248, 0x0250, 0x0258
        .dw 0x0260, 0x0268, 0x0270, 0x0278, 0x0280, 0x0288, 0x0290, 0x0298
        .dw 0x02A0, 0x02A0, 0x02A0, 0x02A0, 0x02A0, 0x02A0, 0x02A0
        ; bottom=0 slope=+0
        .dw 0x0027, 0x0027, 0x0027, 0x0027, 0x0027, 0x0027, 0x0027, 0x0027
        .dw 0x0027, 0x002F, 0x0037, 0x003F, 0x0047, 0x004F, 0x0057, 0x005F
        .dw 0x0067, 0x006F, 0x0077, 0x007F, 0x0087, 0x008F, 0x0097, 0x009F
        .dw 0x009F, 0x009F, 0x009F, 0x009F, 0x009F, 0x009F, 0x009F
        ; bottom=0 slope=+1
        .dw 0x0028, 0x0028, 0x0028, 0x0028, 0x0028, 0x0028, 0x0028, 0x0028
        .dw 0x0028, 0x0030, 0x0038, 0x0040, 0x0048, 0x0050, 0x0058, 0x0060
        .dw 0x0068, 0x0070, 0x0078, 0x0080, 0x0088, 0x0090, 0x0098, 0x00A0
        .dw 0x00A0, 0x00A0, 0x00A0, 0x00A0, 0x00A0, 0x00A0, 0x00A0
        ; bottom=0 slope=+2
        .dw 0x0029, 0x0029, 0x0029, 0x0029, 0x0029, 0x0029, 0x0029, 0x0029
        .dw 0x0029, 0x0031, 0x0039, 0x0041, 0x0049, 0x0051, 0x0059, 0x0061
        .dw 0x0069, 0x0071, 0x0079, 0x0081, 0x0089, 0x0091, 0x0099, 0x00A1
        .dw 0x00A1, 0x00A1, 0x00A1, 0x00A1, 0x00A1, 0x00A1, 0x00A1
        ; bottom=0 slope=+3
        .dw 0x002A, 0x002A, 0x002A, 0x002A, 0x002A, 0x002A, 0x002A, 0x002A
        .dw 0x002A, 0x0032, 0x003A, 0x0042, 0x004A, 0x0052, 0x005A, 0x0062
        .dw 0x006A, 0x0072, 0x007A, 0x0082, 0x008A, 0x0092, 0x009A, 0x00A2
        .dw 0x00A2, 0x00A2, 0x00A2, 0x00A2, 0x00A2, 0x00A2, 0x00A2
        ; bottom=0 slope=+4
        .dw 0x002B, 0x002B, 0x002B, 0x002B, 0x002B, 0x002B, 0x002B, 0x002B
        .dw 0x002B, 0x0033, 0x003B, 0x0043, 0x004B, 0x0053, 0x005B, 0x0063
        .dw 0x006B, 0x0073, 0x007B, 0x0083, 0x008B, 0x0093, 0x009B, 0x00A3
        .dw 0x00A3, 0x00A3, 0x00A3, 0x00A3, 0x00A3, 0x00A3, 0x00A3
        ; bottom=0 slope=+5
        .dw 0x002C, 0x002C, 0x002C, 0x002C, 0x002C, 0x002C, 0x002C, 0x002C
        .dw 0x002C, 0x0034, 0x003C, 0x0044, 0x004C, 0x0054, 0x005C, 0x0064
        .dw 0x006C, 0x0074, 0x007C, 0x0084, 0x008C, 0x0094, 0x009C, 0x00A4
        .dw 0x00A4, 0x00A4, 0x00A4, 0x00A4, 0x00A4, 0x00A4, 0x00A4
        ; bottom=0 slope=+6
        .dw 0x002D, 0x002D, 0x002D, 0x002D, 0x002D, 0x002D, 0x002D, 0x002D
        .dw 0x002D, 0x0035, 0x003D, 0x0045, 0x004D, 0x0055, 0x005D, 0x0065
        .dw 0x006D, 0x0075, 0x007D, 0x0085, 0x008D, 0x0095, 0x009D, 0x00A5
        .dw 0x00A5, 0x00A5, 0x00A5, 0x00A5, 0x00A5, 0x00A5, 0x00A5
        ; bottom=0 slope=+7
        .dw 0x002E, 0x002E, 0x002E, 0x002E, 0x002E, 0x002E, 0x002E, 0x002E
        .dw 0x002E, 0x0036, 0x003E, 0x0046, 0x004E, 0x0056, 0x005E, 0x0066
        .dw 0x006E, 0x0076, 0x007E, 0x0086, 0x008E, 0x0096, 0x009E, 0x00A6
        .dw 0x00A6, 0x00A6, 0x00A6, 0x00A6, 0x00A6, 0x00A6, 0x00A6
        ; bottom=1 slope=-7
        .dw 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6
        .dw 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0C9E
        .dw 0x0C96, 0x0C8E, 0x0C86, 0x0C7E, 0x0C76, 0x0C6E, 0x0C66, 0x0C5E
        .dw 0x0C56, 0x0C4E, 0x0C46, 0x0C3E, 0x0C36, 0x0C2E, 0x0C2E
        ; bottom=1 slope=-6
        .dw 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5
        .dw 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0C9D
        .dw 0x0C95, 0x0C8D, 0x0C85, 0x0C7D, 0x0C75, 0x0C6D, 0x0C65, 0x0C5D
        .dw 0x0C55, 0x0C4D, 0x0C45, 0x0C3D, 0x0C35, 0x0C2D, 0x0C2D
        ; bottom=1 slope=-5
        .dw 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4
        .dw 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0C9C
        .dw 0x0C94, 0x0C8C, 0x0C84, 0x0C7C, 0x0C74, 0x0C6C, 0x0C64, 0x0C5C
        .dw 0x0C54, 0x0C4C, 0x0C44, 0x0C3C, 0x0C34, 0x0C2C, 0x0C2C
        ; bottom=1 slope=-4
        .dw 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3
        .dw 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0C9B
        .dw 0x0C93, 0x0C8B, 0x0C83, 0x0C7B, 0x0C73, 0x0C6B, 0x0C63, 0x0C5B
        .dw 0x0C53, 0x0C4B, 0x0C43, 0x0C3B, 0x0C33, 0x0C2B, 0x0C2B
        ; bottom=1 slope=-3
        .dw 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2
        .dw 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0C9A
        .dw 0x0C92, 0x0C8A, 0x0C82, 0x0C7A, 0x0C72, 0x0C6A, 0x0C62, 0x0C5A
        .dw 0x0C52, 0x0C4A, 0x0C42, 0x0C3A, 0x0C32, 0x0C2A, 0x0C2A
        ; bottom=1 slope=-2
        .dw 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1
        .dw 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0C99
        .dw 0x0C91, 0x0C89, 0x0C81, 0x0C79, 0x0C71, 0x0C69, 0x0C61, 0x0C59
        .dw 0x0C51, 0x0C49, 0x0C41, 0x0C39, 0x0C31, 0x0C29, 0x0C29
        ; bottom=1 slope=-1
        .dw 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0
        .dw 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0C98
        .dw 0x0C90, 0x0C88, 0x0C80, 0x0C78, 0x0C70, 0x0C68, 0x0C60, 0x0C58
        .dw 0x0C50, 0x0C48, 0x0C40, 0x0C38, 0x0C30, 0x0C28, 0x0C28
        ; bottom=1 slope=+0
        .dw 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F
        .dw 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C97
        .dw 0x0C8F, 0x0C87, 0x0C7F, 0x0C77, 0x0C6F, 0x0C67, 0x0C5F, 0x0C57
        .dw 0x0C4F, 0x0C47, 0x0C3F, 0x0C37, 0x0C2F, 0x0C27, 0x0C27
        ; bottom=1 slope=+1
        .dw 0x0EA0, 0x0EA0, 0x0EA0, 0x0EA0, 0x0EA0, 0x0EA0, 0x0EA0, 0x0EA0
        .dw 0x0EA0, 0x0EA0, 0x0EA0, 0x0EA0, 0x0EA0, 0x0EA0, 0x0E98, 0x0E90
        .dw 0x0E88, 0x0E80, 0x0E78, 0x0E70, 0x0E68, 0x0E60, 0x0E58, 0x0E50
        .dw 0x0E48, 0x0E40, 0x0E38, 0x0E30, 0x0E28, 0x0E28, 0x0E28
        ; bottom=1 slope=+2
        .dw 0x0EA1, 0x0EA1, 0x0EA1, 0x0EA1, 0x0EA1, 0x0EA1, 0x0EA1, 0x0EA1
        .dw 0x0EA1, 0x0EA1, 0x0EA1, 0x0EA1, 0x0EA1, 0x0E99, 0x0E91, 0x0E89
        .dw 0x0E81, 0x0E79, 0x0E71, 0x0E69, 0x0E61, 0x0E59, 0x0E51, 0x0E49
        .dw 0x0E41, 0x0E39, 0x0E31, 0x0E29, 0x0E29, 0x0E29, 0x0E29
        ; bottom=1 slope=+3
        .dw 0x0EA2, 0x0EA2, 0x0EA2, 0x0EA2, 0x0EA2, 0x0EA2, 0x0EA2, 0x0EA2
        .dw 0x0EA2, 0x0EA2, 0x0EA2, 0x0EA2, 0x0E9A, 0x0E92, 0x0E8A, 0x0E82
        .dw 0x0E7A, 0x0E72, 0x0E6A, 0x0E62, 0x0E5A, 0x0E52, 0x0E4A, 0x0E42
        .dw 0x0E3A, 0x0E32, 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A
        ; bottom=1 slope=+4
        .dw 0x0EA3, 0x0EA3, 0x0EA3, 0x0EA3, 0x0EA3, 0x0EA3, 0x0EA3, 0x0EA3
        .dw 0x0EA3, 0x0EA3, 0x0EA3, 0x0E9B, 0x0E93, 0x0E8B, 0x0E83, 0x0E7B
        .dw 0x0E73, 0x0E6B, 0x0E63, 0x0E5B, 0x0E53, 0x0E4B, 0x0E43, 0x0E3B
        .dw 0x0E33, 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B
        ; bottom=1 slope=+5
        .dw 0x0EA4, 0x0EA4, 0x0EA4, 0x0EA4, 0x0EA4, 0x0EA4, 0x0EA4, 0x0EA4
        .dw 0x0EA4, 0x0EA4, 0x0E9C, 0x0E94, 0x0E8C, 0x0E84, 0x0E7C, 0x0E74
        .dw 0x0E6C, 0x0E64, 0x0E5C, 0x0E54, 0x0E4C, 0x0E44, 0x0E3C, 0x0E34
        .dw 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C
        ; bottom=1 slope=+6
        .dw 0x0EA5, 0x0EA5, 0x0EA5, 0x0EA5, 0x0EA5, 0x0EA5, 0x0EA5, 0x0EA5
        .dw 0x0EA5, 0x0E9D, 0x0E95, 0x0E8D, 0x0E85, 0x0E7D, 0x0E75, 0x0E6D
        .dw 0x0E65, 0x0E5D, 0x0E55, 0x0E4D, 0x0E45, 0x0E3D, 0x0E35, 0x0E2D
        .dw 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D
        ; bottom=1 slope=+7
        .dw 0x0EA6, 0x0EA6, 0x0EA6, 0x0EA6, 0x0EA6, 0x0EA6, 0x0EA6, 0x0EA6
        .dw 0x0E9E, 0x0E96, 0x0E8E, 0x0E86, 0x0E7E, 0x0E76, 0x0E6E, 0x0E66
        .dw 0x0E5E, 0x0E56, 0x0E4E, 0x0E46, 0x0E3E, 0x0E36, 0x0E2E, 0x0E2E
        .dw 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E


; ---------------------------------------------------------------------------
; Retained swept-boundary state (rung 26).
;
; Per (surface, coarse column) this keeps the six bytes that completely
; determine what surface_column_fast writes: the two Q8 inverse depths that
; generate every pixel endpoint through the (per-surface constant) profile,
; the border bits, and the three coverage bytes naming the rows this surface
; actually owns. Shade is constant 1 on this path and the clip window is the
; full viewport, so nothing else is an input.
;
; An entry is only trusted when the surface was drawn in the immediately
; preceding frame exactly once. A surface that vanished for a frame may have
; had its cells restored to background by nt_end_frame, and a surface drawn
; twice in one frame would have its own two runs overwrite each other's key,
; so both cases are invalidated rather than reasoned about.
; ---------------------------------------------------------------------------

; A = sid -> HL = &polar_ret_live$[sid>>3], C = bit mask for that sid.
ret_bitmask$:
_tsp_h_ret_bitmask::
        push    af
        and     #7
        ld      l, a
        ld      h, #0
        ld      de, #ret_mask8$
        add     hl, de
        ld      c, (hl)
        pop     af
        srl     a
        srl     a
        srl     a
        ld      l, a
        ld      h, #0
        ld      de, #polar_ret_live$
        add     hl, de
        ret

; Drop every retained key. Called on renderer reset, where nothing about the
; previous name table can be assumed.
_tsp_polar_ret_invalidate::
        push    bc
        push    hl
        ld      hl, #polar_ret_valid$
        ld      b, #4
ret_inval_loop$:
        ld      (hl), #0
        inc     hl
        djnz    ret_inval_loop$
        ld      hl, #polar_ret_pc0$
        ld      b, #32
ret_inval_r0$:
        ld      (hl), #1
        inc     hl
        djnz    ret_inval_r0$
        ld      hl, #polar_ret_pc1$
        ld      b, #32
ret_inval_r1$:
        ld      (hl), #0
        inc     hl
        djnz    ret_inval_r1$
        pop     hl
        pop     bc
        ; fall through to clear live/poison and disable the direct path
_tsp_polar_ret_begin_frame::
        push    hl
        xor     a
        ld      (#polar_ret_live$+0), a
        ld      (#polar_ret_live$+1), a
        ld      (#polar_ret_live$+2), a
        ld      (#polar_ret_live$+3), a
        ld      (#polar_ret_poison$+0), a
        ld      (#polar_ret_poison$+1), a
        ld      (#polar_ret_poison$+2), a
        ld      (#polar_ret_poison$+3), a
        ld      (#r_ret_fresh$), a
        ld      hl, #0
        ld      (#r_ret_base$), hl      ; direct column calls bypass retention
        pop     hl
        ret

; valid[sid] <- drawn exactly once this frame.
; valid <- drawn exactly once this frame. Four bytes of bitmask, not
; thirty bytes of expansion: the per-run path already has the bit index
; in hand, so expanding it here was work nobody needed.
_tsp_polar_ret_end_frame::
        push    bc
        push    de
        push    hl
        ld      hl, #polar_ret_live$
        ld      de, #polar_ret_poison$
        ld      bc, #(polar_ret_valid$ - polar_ret_live$)
ret_end_group$:
        ld      a, (de)
        cpl
        and     (hl)
        push    hl
        add     hl, bc
        ld      (hl), a
        pop     hl
        inc     hl
        inc     de
        ld      a, l
        sub     #<(polar_ret_live$ + 4)
        jr      nz, ret_end_group$
        pop     hl
        pop     de
        pop     bc
        ret

; Called once per run, from run_geometry_fast, with _g_polar_run_sid live.
ret_run_begin$:
_tsp_h_ret_run_begin::
        ld      a, (#_g_polar_run_sid)
        cp      #32
        jp      nc, ret_run_disable$
        call    ret_bitmask$            ; HL=&live[group], C=mask
        ld      a, (hl)
        and     c
        jr      z, ret_run_first$

        ; Same surface already drawn this frame: the two runs would share one
        ; key slot, so neither this frame nor the next may trust it.
        ld      de, #(polar_ret_poison$ - polar_ret_live$)
        add     hl, de
        ld      a, (hl)
        or      c
        ld      (hl), a
        xor     a
        ld      (#r_ret_fresh$), a
        jr      ret_run_base$

ret_run_first$:
        ld      a, (hl)
        or      c
        ld      (hl), a
        ld      de, #(polar_ret_valid$ - polar_ret_live$)
        add     hl, de
        ld      a, (hl)
        and     c
        ld      (#r_ret_fresh$), a

ret_run_base$:
        ; Swap in last frame's visited range and record this frame's.
        ld      a, (#_g_polar_run_sid)
        ld      l, a
        ld      h, #0
        ld      de, #polar_ret_pc0$
        add     hl, de
        ld      a, (hl)
        ld      (#r_ret_pc0$), a
        ld      a, (#_g_polar_run_c0)
        ld      (hl), a
        ld      de, #(polar_ret_pc1$ - polar_ret_pc0$)
        add     hl, de
        ld      a, (hl)
        ld      (#r_ret_pc1$), a
        ld      a, (#_g_polar_run_c1)
        ld      (hl), a

        ld      a, (#_g_polar_run_sid)
        add     a, a
        ld      l, a
        ld      h, #0
        ld      de, #polar_ret_index$
        add     hl, de
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a
        ld      (#r_ret_base$), hl
        ; Column cursor: the run walks c0..c1 in order, so the slot address is
        ; an add of eight per column rather than a multiply per column.
        ld      a, (#_g_polar_run_c0)
        add     a, a
        add     a, a
        add     a, a
        ld      e, a
        ld      d, #0
        add     hl, de
        ld      (#r_ret_ptr$), hl
        ; Trusted column range: columns this run covers that this surface also
        ; visited last frame. Everything else has no key worth comparing, so
        ; the per-column path needs two bounds checks and nothing else.
        ld      a, (#r_ret_fresh$)
        or      a
        jr      z, ret_run_untrusted$
        ld      a, (#_g_polar_run_c0)
        ld      c, a
        ld      a, (#r_ret_pc0$)
        cp      c
        jr      nc, ret_run_t0$
        ld      a, c
ret_run_t0$:
        ld      (#r_ret_t0$), a
        ld      a, (#_g_polar_run_c1)
        ld      c, a
        ld      a, (#r_ret_pc1$)
        cp      c
        jr      c, ret_run_t1$
        ld      a, c
ret_run_t1$:
        ld      (#r_ret_t1$), a
        ret
ret_run_untrusted$:
        ld      a, #1
        ld      (#r_ret_t0$), a
        xor     a
        ld      (#r_ret_t1$), a
        ret

ret_run_disable$:
        xor     a
        ld      (#r_ret_fresh$), a
        ld      a, #1
        ld      (#r_ret_t0$), a
        xor     a
        ld      (#r_ret_t1$), a
        ld      hl, #0
        ld      (#r_ret_base$), hl
        ret

; Mark this column's slot as answering for nothing. A rasterized column always
; has at least one unclaimed row, so an all-zero coverage triple can never be
; mistaken for a live key.
ret_column_kill$:
_tsp_h_ret_column_kill::
        push    hl
        ld      hl, (#r_ret_base$)
        ld      a, h
        or      l
        jr      z, ret_kill_out$
        ld      hl, (#r_ret_ptr$)
        inc     hl
        inc     hl
        inc     hl                      ; -> unclaimed0 of this column's slot
        ld      (hl), #0
        inc     hl
        ld      (hl), #0
        inc     hl
        ld      (hl), #0
        inc     hl
        ld      (hl), #0xff             ; and no patchable previous form
ret_kill_out$:
        pop     hl
        ret

; Returns Z when this column's key is unchanged since last frame, NZ otherwise,
; and always leaves the slot holding this frame's key. The compare runs inline
; and falls straight into a partial store from the first byte that differed:
; the bytes before it already match, so rewriting them is pure cost.
ret_column_gate$:
_tsp_h_ret_column_gate::
        push    bc
        ld      hl, (#r_ret_base$)
        ld      a, h
        or      l
        jr      z, ret_gate_off$
        ld      hl, (#r_ret_ptr$)
        ld      a, (#_g_polar_mat_col)
        ld      c, a
        ld      a, (#r_ret_t0$)
        cp      c
        jr      z, ret_gate_try$
        jr      nc, ret_put0$           ; before the trusted range
ret_gate_try$:
        ld      a, (#r_ret_t1$)
        cp      c
        jr      c, ret_put0$            ; after the trusted range

        ld      a, (#r_run_halfl$)
        cp      (hl)
        jr      nz, ret_put0$
        inc     hl
        ld      a, (#r_run_halfr$)
        cp      (hl)
        jr      nz, ret_put1$
        inc     hl
        ld      a, (#_g_polar_mat_border)
        cp      (hl)
        jr      nz, ret_put2$
        inc     hl
        ld      a, (#r_unclaimed0$)
        cp      (hl)
        jr      nz, ret_put3$
        inc     hl
        ld      a, (#r_unclaimed1$)
        cp      (hl)
        jr      nz, ret_put4$
        inc     hl
        ld      a, (#r_unclaimed2$)
        cp      (hl)
        jr      nz, ret_put5$
        pop     bc
        xor     a                       ; Z: identical, skip the raster
        ret

ret_gate_off$:
        pop     bc
        ld      a, #1
        or      a
        ret

ret_put0$:
        ld      a, (#r_run_halfl$)
        ld      (hl), a
        inc     hl
ret_put1$:
        ld      a, (#r_run_halfr$)
        ld      (hl), a
        inc     hl
ret_put2$:
        ld      a, (#_g_polar_mat_border)
        ld      (hl), a
        inc     hl
ret_put3$:
        ld      a, (#r_unclaimed0$)
        ld      (hl), a
        inc     hl
ret_put4$:
        ld      a, (#r_unclaimed1$)
        ld      (hl), a
        inc     hl
ret_put5$:
        ld      a, (#r_unclaimed2$)
        ld      (hl), a
        pop     bc
        or      #1                      ; NZ: the column must be rasterized
        ret


; ---------------------------------------------------------------------------
; Foreground FULL-wall boundary patch (rung 27).
;
; A tall FULL wall that nothing occludes is an extremely constrained picture: a
; partial edge tile at one tile row, solid wall inward from it, and the exact
; VFLIP mirror of both at the bottom. Nothing else. So when such a wall is still
; there and its top edge has only moved a row or two, the correct new column is
; the old one with the edge tile rewritten and a guard band of at most two solid
; cells painted in on each end. The deep interior is already right.
;
; That is the case the exact-key gate cannot help with, because the key changes
; the moment anything moves. This path helps precisely the columns that changed.
;
; Guards, all cheap, all rejecting to the existing materializer:
;   - FULL profile, and the surface was drawn exactly once last frame with this
;     column inside its span (the retained trusted range)
;   - the edge occupies a single tile row this frame and did last frame, both
;     on screen and high enough that the top and bottom guard bands cannot meet
;   - the edge moved at most two tile rows
;   - the border bits are unchanged, since they are baked into the fill tile
;   - nothing nearer has touched this column at all this frame, which is what
;     makes ownership knowable without building the mask
;
; Shrinking needs no erase: the rows this wall no longer covers are absent from
; the coverage mask, and the end-of-frame reconciliation already restores
; exactly those.
;
; Returns Z when it handled the column completely.
; ---------------------------------------------------------------------------
ret_try_patch$:
_tsp_h_ret_try_patch::
        ; FULL-only benchmark: profile is a compile-time course invariant.
        ld      hl, (#r_ret_base$)
        ld      a, h
        or      l
        jp      z, ret_patch_no$

        ld      a, (#_g_polar_mat_col)
        ld      c, a
        ld      a, (#r_ret_t0$)
        cp      c
        jr      z, ret_patch_t1$
        jp      nc, ret_patch_no$
ret_patch_t1$:
        ld      a, (#r_ret_t1$)
        cp      c
        jp      c, ret_patch_no$

        ; One edge tile row this column, on screen, and tall enough that the
        ; top and bottom guard bands stay apart. cp #7 rejects negatives too.
        ld      a, (#r_top_l_row$)
        ld      c, a
        ld      a, (#r_top_r_row$)
        cp      c
        jp      nz, ret_patch_no$
        cp      #7
        jp      nc, ret_patch_no$
        ld      (#r_patch_n$), a

        ld      hl, (#r_ret_ptr$)
        ld      de, #6
        add     hl, de
        ld      a, (hl)
        cp      #7
        jp      nc, ret_patch_no$       ; 0xff: no clean previous form
        ld      (#r_patch_o$), a

        ld      c, a
        ld      a, (#r_patch_n$)
        sub     c
        jr      nc, ret_patch_abs$
        neg
ret_patch_abs$:
        cp      #3
        jp      nc, ret_patch_no$       ; moved further than the guard band

        ld      hl, (#r_ret_ptr$)
        inc     hl
        inc     hl
        ld      a, (#_g_polar_mat_border)
        cp      (hl)
        jp      nz, ret_patch_no$

        ; Nothing nearer in this column, so every row of it is this wall's.
        ld      a, b
        ld      e, a
        add     a, a
        add     a, e
        ld      e, a
        ld      d, #0
        ld      hl, #_g_polar_nt_cov_cur
        add     hl, de
        ld      a, (hl)
        inc     hl
        or      (hl)
        inc     hl
        or      (hl)
        jp      nz, ret_patch_no$

        ; --- committed: this column is this wall's, and nothing else's ---

        ; Boundary did not move at all. Then the column is identical and the
        ; only thing owed is its coverage, which the slot already records --
        ; so this case never builds the ownership mask either. That makes a
        ; static foreground wall cheaper here than through the exact-key gate,
        ; which has to pay polar_mark_span_fast before it is allowed to decide.
        ld      hl, (#r_ret_ptr$)
        ld      a, (#r_run_halfl$)
        cp      (hl)
        jr      nz, ret_patch_moved$
        inc     hl
        ld      a, (#r_run_halfr$)
        cp      (hl)
        jr      nz, ret_patch_moved$
        ld      hl, (#r_ret_ptr$)
        ld      de, #3
        add     hl, de                  ; -> the recorded coverage triple
        ld      a, b
        ld      e, a
        add     a, a
        add     a, e
        ld      e, a
        ld      d, #0
        push    hl
        ld      hl, #_g_polar_nt_cov_cur
        add     hl, de
        ex      de, hl
        pop     hl
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        ld      a, (hl)
        ld      (de), a
        xor     a
        ret                             ; Z: handled, nothing to draw

ret_patch_moved$:

        ; Same nine-shape FULL ownership vocabulary as the normal exact path.
        ; n is already proven 0..6 above, so reuse the baked 27-byte mask
        ; installer instead of reconstructing prefix[18-n] XOR prefix[n].
        ld      a, (#r_patch_n$)
        call    polar_set_full_owned_fast$

        ; Edge tile and its mirror, through the existing LUT emitter. The LUT
        ; is where sub-cell precision lives, so nothing here needs to know
        ; about pixels beyond handing it the same two endpoints.
        ld      hl, (#_g_polar_mat_top_l)
        ld      (#r_edge_left$), hl
        ld      de, (#_g_polar_mat_top_r)
        ex      de, hl
        or      a
        sbc     hl, de
        ld      a, l
        bit     7, a
        jr      nz, ret_patch_sneg$
        cp      #8
        jr      c, ret_patch_sst$
        ld      a, #7
        jr      ret_patch_sst$
ret_patch_sneg$:
        cp      #0xF9
        jr      nc, ret_patch_sst$
        ld      a, #0xF9
ret_patch_sst$:
        ld      (#r_edge_slope$), a
        ld      a, (#r_patch_n$)
        call    draw_symfull_edge_pair$

        ; Growing upward sweeps (o-n) rows into solid wall at each end. The old
        ; edge cell is one of them, so it is overwritten rather than erased.
        ld      a, (#r_patch_o$)
        ld      c, a
        ld      a, (#r_patch_n$)
        cp      c
        jr      nc, ret_patch_store$
        ld      a, (#r_patch_n$)
        neg
        add     a, c
        ld      (#r_patch_cnt$), a
        call    full_tile_low$
        ld      (#r_full_tile$), a
        ld      a, (#r_patch_n$)
        inc     a
        ld      (#r_row$), a
        ld      a, (#r_patch_cnt$)
        call    ret_patch_fill_run$     ; rows n+1 .. o
        ld      a, (#r_patch_o$)
        neg
        add     a, #17
        ld      (#r_row$), a
        ld      a, (#r_patch_cnt$)
        call    ret_patch_fill_run$     ; rows 17-o .. 16-n

ret_patch_store$:
        ; Keep the exact-key slot coherent: a column this path handled must
        ; still answer correctly for the gate next frame.
        ld      hl, (#r_ret_ptr$)
        ld      a, (#r_run_halfl$)
        ld      (hl), a
        inc     hl
        ld      a, (#r_run_halfr$)
        ld      (hl), a
        inc     hl
        ld      a, (#_g_polar_mat_border)
        ld      (hl), a
        inc     hl
        ld      a, (#r_unclaimed0$)
        ld      (hl), a
        inc     hl
        ld      a, (#r_unclaimed1$)
        ld      (hl), a
        inc     hl
        ld      a, (#r_unclaimed2$)
        ld      (hl), a
        inc     hl
        ld      a, (#r_patch_n$)
        ld      (hl), a
        xor     a
        ret                             ; Z: handled

ret_patch_no$:
        ld      a, #1
        or      a
        ret                             ; NZ: fall through to the full path

; A=row count, r_row$=first row, B=column, r_full_tile$=low byte of the tile.
ret_patch_fill_run$:
_tsp_h_ret_patch_fill_run::
        ld      c, a
        ld      a, (#r_row$)
        call    map_ptr_row_col$
ret_patch_fill_loop$:
        ld      a, (#r_full_tile$)
        ld      e, a
        ld      a, (hl)
        cp      e
        jr      nz, ret_patch_fill_wr$
        inc     hl
        ld      a, (hl)
        or      a
        dec     hl
        jr      z, ret_patch_fill_nx$
ret_patch_fill_wr$:
        ld      (hl), e
        inc     hl
        ld      (hl), #0
        dec     hl
        push    hl
        ld      a, (#r_row$)
        call    polar_mark_dirty_fast$
        pop     hl
ret_patch_fill_nx$:
        ld      de, #40
        add     hl, de
        ld      a, (#r_row$)
        inc     a
        ld      (#r_row$), a
        dec     c
        jr      nz, ret_patch_fill_loop$
        ret

; After a full raster, record whether this column ended in the clean patchable
; form: a FULL wall this surface owns outright, with its edge on a single tile
; row high enough for the guard bands. Anything else stores 0xff.
ret_record_clean$:
_tsp_h_ret_record_clean::
        push    bc
        push    hl
        ld      hl, (#r_ret_base$)
        ld      a, h
        or      l
        jr      z, ret_rc_out$
        ld      hl, (#r_ret_ptr$)
        ld      bc, #6
        add     hl, bc
        ld      (hl), #0xff
        ; FULL-only benchmark: no profile test is needed here.
        ld      a, (#r_occluded$)
        or      a
        jr      nz, ret_rc_out$
        ld      a, (#r_top_min$)
        ld      c, a
        ld      a, (#r_top_max$)
        cp      c
        jr      nz, ret_rc_out$
        cp      #7
        jr      nc, ret_rc_out$
        ld      (hl), a
ret_rc_out$:
        pop     hl
        pop     bc
        ret

ret_mask8$:
        .db 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80

polar_ret_index$:
        .dw polar_ret_store$+0
        .dw polar_ret_store$+160
        .dw polar_ret_store$+320
        .dw polar_ret_store$+480
        .dw polar_ret_store$+640
        .dw polar_ret_store$+800
        .dw polar_ret_store$+960
        .dw polar_ret_store$+1120
        .dw polar_ret_store$+1280
        .dw polar_ret_store$+1440
        .dw polar_ret_store$+1600
        .dw polar_ret_store$+1760
        .dw polar_ret_store$+1920
        .dw polar_ret_store$+2080
        .dw polar_ret_store$+2240
        .dw polar_ret_store$+2400
        .dw polar_ret_store$+2560
        .dw polar_ret_store$+2720
        .dw polar_ret_store$+2880
        .dw polar_ret_store$+3040
        .dw polar_ret_store$+3200
        .dw polar_ret_store$+3360
        .dw polar_ret_store$+3520
        .dw polar_ret_store$+3680
        .dw polar_ret_store$+3840
        .dw polar_ret_store$+4000
        .dw polar_ret_store$+4160
        .dw polar_ret_store$+4320
        .dw polar_ret_store$+4480
        .dw polar_ret_store$+4640
        .dw polar_ret_store$+4800
        .dw polar_ret_store$+4960

        .area _DATA
polar_ret_valid$:
        .ds     4
polar_ret_live$:
        .ds     4
polar_ret_poison$:
        .ds     4
; Which coarse columns this surface actually visited last frame. A column it
; did not visit has no key worth trusting, whatever the slot still holds.
polar_ret_pc0$:
        .ds     32
polar_ret_pc1$:
        .ds     32
r_ret_pc0$:
        .ds     1
r_ret_pc1$:
        .ds     1
r_ret_t0$:
        .ds     1
r_ret_t1$:
        .ds     1
r_patch_n$:
        .ds     1
r_patch_o$:
        .ds     1
r_patch_cnt$:
        .ds     1
r_patch_pp$:
        .ds     2
r_ret_fresh$:
        .ds     1
r_ret_base$:
        .ds     2
r_ret_ptr$:
        .ds     2
; 32 surfaces x 20 coarse columns x 8 bytes. Six bytes carry the key; the
; eighth-byte stride keeps the column index a shift rather than a multiply.
polar_ret_store$:
        .ds     5120
r_seam_i$:
        .ds     1
r_seam_x$:
        .ds     1
r_seam_mask$:
        .ds     1
r_seam_col$:
        .ds     1
r_seam_row$:
        .ds     1
r_seam_last$:
        .ds     1
r_seam_x0$:
        .ds     1
r_seam_y0$:
        .ds     1
r_run_col$:
        .ds     1
r_run_invl$:
        .ds     1
r_run_invr$:
        .ds     1
r_run_half$:
        .ds     1
; The half-depths are what profile_half actually consumes, so they are the
; coarsest quantity that still determines every pixel endpoint exactly.
; Keying the retained slot on inv rather than inv>>1 made the key twice as
; sensitive as the geometry it guards, for nothing.
r_run_halfl$:
        .ds     1
r_run_halfr$:
        .ds     1
r_clip_first$:
        .ds     1
r_clip_last$:
        .ds     1
r_top_l_row$:
        .ds     1
r_top_r_row$:
_tsp_probe_row::
        .ds     1
r_bot_l_row$:
        .ds     1
r_bot_r_row$:
        .ds     1
r_top_min$:
_tsp_probe_top_min::
        .ds     1
r_top_max$:
_tsp_probe_top_max::
        .ds     1
r_bot_min$:
_tsp_probe_bot_min::
        .ds     1
r_bot_max$:
_tsp_probe_bot_max::
        .ds     1
r_edge_left$:
        .ds     2
r_edge_slope$:
        .ds     1
r_edge_bottom$:
        .ds     1
r_edge_min$:
        .ds     1
r_edge_max$:
        .ds     1
r_edge_iter$:
        .ds     1
r_sym_bottom_row$:
        .ds     1
r_sym_top_draw$:
        .ds     1
r_sym_bot_draw$:
        .ds     1
r_sym_store_row$:
        .ds     1
r_sym_word$:
        .ds     2
r_row$:
        .ds     1
r_local_index$:
        .ds     1
r_cap_delta$:
        .ds     1
r_fill_first$:
_tsp_probe_fill_first::
        .ds     1
r_full_tile$:
_tsp_probe_full_tile::
        .ds     1
r_cov_first$:
        .ds     1
r_cov_after$:
        .ds     1
r_cov_p0$:
        .ds     1
r_cov_p1$:
        .ds     1
r_cov_p2$:
        .ds     1
r_cov_old$:
        .ds     1
r_unclaimed0$:
_tsp_probe_unclaimed0::
        .ds     1
r_unclaimed1$:
_tsp_probe_unclaimed1::
        .ds     1
r_unclaimed2$:
_tsp_probe_unclaimed2::
        .ds     1
r_claim_row$:
        .ds     1
; Nonzero when nearer geometry already owned at least one row of this span.
; Zero is the common case (82-99% of spans, measured), and it licenses the
; interior fill to skip the per-row ownership query entirely.
r_occluded$:
_tsp_probe_occluded::
        .ds     1
