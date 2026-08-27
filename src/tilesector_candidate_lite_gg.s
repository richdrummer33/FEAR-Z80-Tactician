        .module tilesector_candidate_lite_gg
        .area   _CODE

        .globl  _g_candidate_lite_ctx
        .globl  _g_best_seg
        .globl  _g_best_inv

; STAGE26_CANDIDATE_ENVELOPE
;
; Stage 12 candidate ABI:
;   context +0 seg_id, +1 view_c0, +2 view_c1, +3 span pointer
;   TSProjectedSpan +0 c0, +1 c1, +2 inv_q6, +4 step_q6,
;                   +6 original_c0, +7 original_c1
;
; Keep the proven 20-column winner arrays. For spans >=4 columns, first test
; whether the existing ownership is one uniform segment. If so, affine-depth
; endpoint bounds can safely:
;   - reject the whole candidate when existing is strictly nearer at both ends;
;   - replace the whole interval when candidate is >=2 quantized units nearer
;     at both ends (the +2 protects the old-winner-on-tie rule).
; Ambiguous/crossing/nonuniform cases use the exact Stage-12 loop unchanged.

_ts_candidate_fast_reset::
        xor     a
        ld      (_g_ts_cand_total), a
        ld      (_g_ts_cand_uniform), a
        ld      (_g_ts_cand_reject), a
        ld      (_g_ts_cand_replace), a
        ld      (_g_ts_cand_fallback), a
        ret

_ts_candidate_span_lite_fast::
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ld      a, (#_g_candidate_lite_ctx + 0)
        ld      (#cl_seg$), a
        ld      hl, (#_g_candidate_lite_ctx + 3)
        ld      a, (hl)
        ld      (#cl_col$), a
        inc     hl
        ld      a, (hl)
        ld      (#cl_end$), a
        inc     hl
        ld      a, (hl)
        ld      (#cl_invq$), a
        inc     hl
        ld      a, (hl)
        ld      (#cl_invq$ + 1), a
        inc     hl
        ld      a, (hl)
        ld      (#cl_step$), a
        inc     hl
        ld      a, (hl)
        ld      (#cl_step$ + 1), a

        ; Left clip. Keep reciprocal phase aligned while throwing columns away.
        ld      a, (#_g_candidate_lite_ctx + 1)
        ld      b, a
cl_clip_left$:
        ld      a, (#cl_col$)
        cp      b
        jr      nc, cl_left_ready$
        ld      hl, (#cl_invq$)
        ld      de, (#cl_step$)
        add     hl, de
        ld      (#cl_invq$), hl
        ld      hl, #cl_col$
        inc     (hl)
        jr      cl_clip_left$

cl_left_ready$:
        ; end=min(projected c1, view_c1)
        ld      a, (#cl_end$)
        ld      b, a
        ld      a, (#_g_candidate_lite_ctx + 2)
        cp      b
        jr      nc, cl_end_ready$
        ld      (#cl_end$), a
cl_end_ready$:
        ld      a, (#cl_col$)
        ld      b, a
        ld      a, (#cl_end$)
        cp      b
        jp      c, cl_done$

        ld      hl, #_g_ts_cand_total
        inc     (hl)

        ; IX and IY point at existing winner ID/depth for the first column.
        ld      a, (#cl_col$)
        ld      l, a
        ld      h, #0
        ld      de, #_g_best_seg
        add     hl, de
        push    hl
        pop     ix

        ld      a, (#cl_col$)
        ld      l, a
        ld      h, #0
        ld      de, #_g_best_inv
        add     hl, de
        push    hl
        pop     iy

        ; count=end-start+1. Tiny spans stay on the exact old loop: the envelope
        ; setup would cost more than the comparisons it avoids.
        ld      a, (#cl_end$)
        ld      c, a
        ld      a, (#cl_col$)
        ld      b, a
        ld      a, c
        sub     b
        inc     a
        ld      (#cl_count$), a
        cp      #4
        jp      c, cl_fast_fallback$

        ; Prove the whole overlap is currently one owner ID.
        ld      a, 0 (ix)
        ld      (#cl_uniform_seg$), a
        push    ix
        pop     hl
        ld      a, (#cl_count$)
        ld      b, a
cl_uniform_scan$:
        ld      a, (#cl_uniform_seg$)
        cp      (hl)
        jp      nz, cl_fast_fallback$
        inc     hl
        djnz    cl_uniform_scan$

        ld      hl, #_g_ts_cand_uniform
        inc     (hl)

        ; Empty uniform ownership is an unconditional whole-interval fill.
        ld      a, (#cl_uniform_seg$)
        inc     a
        jp      z, cl_fast_replace$

        ; Candidate quantized reciprocal at first and last columns.
        ld      de, (#cl_invq$)
        ld      bc, (#cl_step$)
        call    cl_mid_from_de_bc$
        ld      (#cl_first_mid$), a

        ld      a, (#cl_count$)
        dec     a
        jp      z, cl_last_current_ready$
cl_last_advance$:
        ld      h, d
        ld      l, e
        add     hl, bc
        ld      d, h
        ld      e, l
        dec     a
        jp      nz, cl_last_advance$
cl_last_current_ready$:
        call    cl_mid_from_de_bc$
        ld      (#cl_last_mid$), a

        ld      a, 0 (iy)
        ld      (#cl_old_first$), a
        ld      a, (#cl_end$)
        ld      l, a
        ld      h, #0
        ld      de, #_g_best_inv
        add     hl, de
        ld      a, (hl)
        ld      (#cl_old_last$), a

        ; Safe whole reject: old is strictly nearer at both endpoints.
        ld      a, (#cl_old_first$)
        ld      c, a
        ld      a, (#cl_first_mid$)
        cp      c
        jp      nc, cl_try_replace$
        ld      a, (#cl_old_last$)
        ld      c, a
        ld      a, (#cl_last_mid$)
        cp      c
        jp      c, cl_fast_reject$

cl_try_replace$:
        ; Safe whole replace requires >=2 at both endpoints. A one-unit lead can
        ; collapse to an interior quantized tie, which must keep the old winner.
        ld      a, (#cl_old_first$)
        ld      c, a
        ld      a, (#cl_first_mid$)
        sub     c
        jp      c, cl_fast_fallback$
        cp      #2
        jp      c, cl_fast_fallback$
        ld      a, (#cl_old_last$)
        ld      c, a
        ld      a, (#cl_last_mid$)
        sub     c
        jp      c, cl_fast_fallback$
        cp      #2
        jp      c, cl_fast_fallback$
        jp      cl_fast_replace$

cl_fast_reject$:
        ld      hl, #_g_ts_cand_reject
        inc     (hl)
        jp      cl_done$

cl_fast_replace$:
        ld      hl, #_g_ts_cand_replace
        inc     (hl)

        ld      de, (#cl_invq$)
        ld      bc, (#cl_step$)
        ld      a, (#cl_count$)
        ld      (#cl_left$), a

cl_replace_loop$:
        call    cl_mid_from_de_bc$
        ld      (#cl_mid$), a
        ld      a, (#cl_seg$)
        ld      0 (ix), a
        ld      a, (#cl_mid$)
        ld      0 (iy), a

        ld      h, d
        ld      l, e
        add     hl, bc
        ld      d, h
        ld      e, l
        inc     ix
        inc     iy

        ld      a, (#cl_left$)
        dec     a
        ld      (#cl_left$), a
        jp      nz, cl_replace_loop$
        jp      cl_done$

cl_fast_fallback$:
        ld      hl, #_g_ts_cand_fallback
        inc     (hl)
        ld      de, (#cl_invq$)
        ld      bc, (#cl_step$)

cl_loop$:
        ; Exact Stage-12 candidate rule.
        ld      h, d
        ld      l, e
        add     hl, bc
        ld      (#cl_next$), hl
        add     hl, de
        add     hl, hl
        ld      a, h
        ld      (#cl_mid$), a

        ld      a, 0 (ix)
        inc     a
        jr      z, cl_winner$
        ld      a, (#cl_mid$)
        cp      0 (iy)
        jr      c, cl_no_win$
        jr      z, cl_no_win$

cl_winner$:
        ld      a, (#cl_seg$)
        ld      0 (ix), a
        ld      a, (#cl_mid$)
        ld      0 (iy), a

cl_no_win$:
        ld      de, (#cl_next$)
        inc     ix
        inc     iy
        ld      hl, #cl_col$
        inc     (hl)
        ld      a, (hl)
        ld      h, a
        ld      a, (#cl_end$)
        cp      h
        jp      nc, cl_loop$

cl_done$:
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        ret

; DE=current Q6 reciprocal, BC=per-column step. Return exact Stage-12
; midpoint-quantized byte in A; preserve DE/BC.
cl_mid_from_de_bc$:
        ld      h, d
        ld      l, e
        add     hl, bc
        add     hl, de
        add     hl, hl
        ld      a, h
        ret

        .area   _BSS
_g_ts_cand_total::    .ds 1
_g_ts_cand_uniform::  .ds 1
_g_ts_cand_reject::   .ds 1
_g_ts_cand_replace::  .ds 1
_g_ts_cand_fallback:: .ds 1
cl_seg$:       .ds 1
cl_col$:       .ds 1
cl_end$:       .ds 1
cl_mid$:       .ds 1
cl_invq$:      .ds 2
cl_step$:      .ds 2
cl_next$:      .ds 2
cl_count$:     .ds 1
cl_left$:      .ds 1
cl_uniform_seg$:.ds 1
cl_first_mid$: .ds 1
cl_last_mid$:  .ds 1
cl_old_first$: .ds 1
cl_old_last$:  .ds 1
