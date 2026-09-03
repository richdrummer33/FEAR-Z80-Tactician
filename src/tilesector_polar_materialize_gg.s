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
        .globl  _g_polar_run_top_q6
        .globl  _g_polar_run_top_step_q6
        .globl  _g_polar_run_bot_q6
        .globl  _g_polar_run_bot_step_q6
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

run_geom_loop$:
        ; inv_left = clamp_u8(((iq + 32) arithmetic>>6), 0..255)
        ld      hl, (#_g_polar_run_iq)
        ld      de, #32
        add     hl, de
        call    q6_round_u8$
        ld      (#r_run_invl$), a

        ; inv_right uses iq+step, exactly matching the C column path.
        ld      hl, (#_g_polar_run_iq)
        ld      de, (#_g_polar_run_step)
        add     hl, de
        ld      de, #32
        add     hl, de
        call    q6_round_u8$
        ld      (#r_run_invr$), a

        ; Left profile endpoints from half=invl>>1.
        ld      a, (#r_run_invl$)
        srl     a
        call    profile_half$
        ld      (#_g_polar_mat_top_l), hl
        ld      (#_g_polar_mat_bot_l), de

        ; Right profile endpoints from half=invr>>1.
        ld      a, (#r_run_invr$)
        srl     a
        call    profile_half$
        ld      (#_g_polar_mat_top_r), hl
        ld      (#_g_polar_mat_bot_r), de

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

        ; iq += step for the next coarse column.
        ld      hl, (#_g_polar_run_iq)
        ld      de, (#_g_polar_run_step)
        add     hl, de
        ld      (#_g_polar_run_iq), hl

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

; E1M1 arbitrary-height connected-run path.  A straight wall's inverse
; depth is affine across screen x, and multiplying that plane by one authored
; z endpoint is affine too.  C therefore supplies top/bottom screen-Y Q6
; start+step once per run.  This loop performs only adds, Q6 rounding and the
; existing final-owner materializer -- no per-column z multiplications.
_tsp_polar_run_zspan_fast::
        push    bc
        push    de
        push    hl

        ld      a, (#_g_polar_run_c0)
        ld      (#r_run_col$), a
        ; C selects the quantized fog shade once per connected run.
        ld      a, #0xff
        ld      (#_g_polar_run_profile), a

zrun_loop$:
        ; Top left/right from one affine Q6 accumulator.
        ld      hl, (#_g_polar_run_top_q6)
        call    q6_round_s16$
        ld      (#_g_polar_mat_top_l), hl
        ld      hl, (#_g_polar_run_top_q6)
        ld      de, (#_g_polar_run_top_step_q6)
        add     hl, de
        call    q6_round_s16$
        ld      (#_g_polar_mat_top_r), hl

        ; Bottom left/right from the second affine Q6 accumulator.
        ld      hl, (#_g_polar_run_bot_q6)
        call    q6_round_s16$
        ld      (#_g_polar_mat_bot_l), hl
        ld      hl, (#_g_polar_run_bot_q6)
        ld      de, (#_g_polar_run_bot_step_q6)
        add     hl, de
        call    q6_round_s16$
        ld      (#_g_polar_mat_bot_r), hl

        xor     a
        ld      (#_g_polar_mat_border), a
        ld      a, (#r_run_col$)
        ld      c, a
        ld      a, (#_g_polar_run_c0)
        cp      c
        jr      nz, zrun_no_left_border$
        ld      a, (#_g_polar_run_left_real)
        or      a
        jr      z, zrun_no_left_border$
        ld      a, #1
        ld      (#_g_polar_mat_border), a
zrun_no_left_border$:
        ld      a, (#_g_polar_run_c1)
        cp      c
        jr      nz, zrun_border_done$
        ld      a, (#_g_polar_run_right_real)
        or      a
        jr      z, zrun_border_done$
        ld      a, (#_g_polar_mat_border)
        or      #2
        ld      (#_g_polar_mat_border), a
zrun_border_done$:
        ld      a, c
        ld      (#_g_polar_mat_col), a
        call    _tsp_polar_surface_column_fast

        ; Advance both projected z planes to the next coarse column.
        ld      hl, (#_g_polar_run_top_q6)
        ld      de, (#_g_polar_run_top_step_q6)
        add     hl, de
        ld      (#_g_polar_run_top_q6), hl
        ld      hl, (#_g_polar_run_bot_q6)
        ld      de, (#_g_polar_run_bot_step_q6)
        add     hl, de
        ld      (#_g_polar_run_bot_q6), hl

        ld      a, (#r_run_col$)
        ld      c, a
        ld      a, (#_g_polar_run_c1)
        cp      c
        jr      z, zrun_done$
        ld      a, c
        inc     a
        ld      (#r_run_col$), a
        jp      zrun_loop$

zrun_done$:
        pop     hl
        pop     de
        pop     bc
        ret

; HL=signed Q6 screen coordinate. Return HL=nearest signed pixel, rounding
; half values away from zero. This preserves the old zproj/horizon quantizer
; while replacing four 8x8 multiplies per coarse column with shifts/adds.
q6_round_s16$:
        bit     7, h
        jr      z, q6s_positive$
        ; magnitude = -HL
        xor     a
        sub     l
        ld      l, a
        ld      a, #0
        sbc     a, h
        ld      h, a
        ld      de, #32
        add     hl, de
        call    q6s_shift6$
        ; restore negative sign
        xor     a
        sub     l
        ld      l, a
        ld      a, #0
        sbc     a, h
        ld      h, a
        ret
q6s_positive$:
        ld      de, #32
        add     hl, de
q6s_shift6$:
        srl     h
        rr      l
        srl     h
        rr      l
        srl     h
        rr      l
        srl     h
        rr      l
        srl     h
        rr      l
        srl     h
        rr      l
        ret

; HL = signed Q6-ish accumulator + rounding bias. Return the exact C
; clamp_u8i((value)>>6,255): arithmetic shift, negative -> 0, >255 -> 255.
q6_round_u8$:
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

; A = half height (inv>>1, 0..127)
; Returns HL=top pixel Y, DE=bottom pixel Y using the exact C profile formulas.
; Profiles: 0 FULL, 1 LINTEL, 2 RAISED, 3 RISER.
profile_half$:
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

        ; Four signed pixel endpoints -> four signed tile rows.
        ld      hl, (#_g_polar_mat_top_l)
        call    row_floor_hl$
        ld      (#r_top_l_row$), a
        ld      hl, (#_g_polar_mat_top_r)
        call    row_floor_hl$
        ld      (#r_top_r_row$), a

        ; POLAR_STAGE21_FULL_VFLIP: exact FULL bottom rows are 17-top rows.
        ; Do not floor two pixel endpoints that are already implied by the top.
        ld      a, (#_g_polar_run_profile)
        or      a
        jp      z, polar_endpoint_rows_ready$
        ld      hl, (#_g_polar_mat_bot_l)
        call    row_floor_hl$
        ld      (#r_bot_l_row$), a
        ld      hl, (#_g_polar_mat_bot_r)
        call    row_floor_hl$
        ld      (#r_bot_r_row$), a
polar_endpoint_rows_ready$:

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

        ; Exact FULL mirror: bottom_min=17-top_max,
        ; bottom_max=17-top_min. Asymmetric profiles keep generic comparison.
        ld      a, (#_g_polar_run_profile)
        or      a
        jp      nz, polar_bot_generic$
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

        ; Persistent name-table lifetime: this projected surface owns one
        ; contiguous visible tile-row span in this coarse screen column.
        ; Mark it once; individual stores only need change-time dirty marking.
        ld      a, (#r_top_min$)
        bit     7, a
        jr      z, polar_cov_first_nonneg$
        xor     a
polar_cov_first_nonneg$:
        cp      #18
        jr      nc, polar_cov_done$
        ld      e, a                   ; E=first visible owned row

        ld      a, (#r_bot_max$)
        bit     7, a
        jr      nz, polar_cov_done$
        cp      #18
        jr      c, polar_cov_last_ready$
        ld      a, #17
polar_cov_last_ready$:
        ld      c, a                   ; C=last visible owned row
        ld      a, e
        cp      c
        jr      c, polar_cov_emit$
        jr      z, polar_cov_emit$
        jr      polar_cov_done$
polar_cov_emit$:
        call    polar_mark_span_fast$   ; returns A=OR of previously-unclaimed rows
_tsp_polar_p_span::
        or      a
        jp      z, raster_done$         ; nearer geometry already owns whole span
polar_cov_done$:

        ; FULL is exact hardware symmetry: calculate each top edge word once
        ; and emit its floor partner with VFLIP+palette. Other profiles retain
        ; independent top/bottom vector edges.
        ld      a, (#_g_polar_run_profile)
        or      a
        jp      z, polar_draw_symfull$
        xor     a
        call    prepare_edge$          ; top
        ld      a, #1
        call    prepare_edge$          ; bottom
        call    draw_plain_interior$
        jr      raster_done$

polar_draw_symfull$:
        call    prepare_symfull_edges$
        call    draw_plain_interior$
        jr      raster_done$

raster_done$:
        pop     hl
        pop     de
        pop     bc
        ret

; Signed floor(pixel/8). Arithmetic shifting gives true floor for negative Y.
row_floor_hl$:
        sra     h
        rr      l
        sra     h
        rr      l
        sra     h
        rr      l
        ld      a, l
        ret

; POLAR_STAGE21_FULL_VFLIP
; FULL walls use one top-edge solution. For every visible top edge row R, the
; exact floor partner is row 17-R and the same tile word ORed with
; VFLIP|palette (high-byte 0x0C). The unclaimed mask is from the state before
; this surface entered, so top/bottom members can be tested independently.
prepare_symfull_edges$:
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
        ld      (#r_row$), a
        bit     7, a
        ret     nz
        cp      #18
        ret     nc

        ld      c, a
        ld      a, #17
        sub     c
        ld      (#r_sym_bottom_row$), a

        ; Check both mirrored rows before paying for the LUT lookup.
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
        ld      a, (#r_row$)
        call    polar_row_unclaimed_fast$
_tsp_polar_p_edge::
        ret     z

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
        ld      a, (#r_row$)
        call    polar_row_unclaimed_fast$
_tsp_polar_p_cap::
        ret     z
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

; Return low-byte full tile ID for current shade/border, cap none.
full_tile_low$:
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

; A=first owned row, C=last owned row, B=column. No register-save ceremony:
; this call sits at the column-materializer level where AF/C/DE/HL are scratch.
; span = prefix[last+1] XOR prefix[first], ORed into cov_cur[col*3].
polar_mark_span_fast$:
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

; A=row 0..17, B=column 0..19 -> HL=&g_map[row*20+col].
map_ptr_row_col$:
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

        .area _DATA
r_run_col$:
        .ds     1
r_run_invl$:
        .ds     1
r_run_invr$:
        .ds     1
r_run_half$:
        .ds     1
r_clip_first$:
        .ds     1
r_clip_last$:
        .ds     1
r_top_l_row$:
        .ds     1
r_top_r_row$:
        .ds     1
r_bot_l_row$:
        .ds     1
r_bot_r_row$:
        .ds     1
r_top_min$:
        .ds     1
r_top_max$:
        .ds     1
r_bot_min$:
        .ds     1
r_bot_max$:
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
        .ds     1
r_full_tile$:
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
        .ds     1
r_unclaimed1$:
        .ds     1
r_unclaimed2$:
        .ds     1
r_claim_row$:
        .ds     1
