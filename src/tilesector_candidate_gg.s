        .module tilesector_candidate_gg
        .area   _CODE

        .globl  _g_candidate_ctx
        .globl  _g_best_seg
        .globl  _g_best_inv
        .globl  _g_best_border
        .globl  _g_best_inv_l_q6
        .globl  _g_best_inv_r_q6

; Stage 11 candidate context ABI:
;   +0 seg_id, +1 view_c0, +2 view_c1, +3 span pointer (little endian)
; TSProjectedSpan ABI:
;   +0 c0, +1 c1, +2 inv_q6, +4 step_q6, +6 original_c0, +7 original_c1
;
; This is deliberately non-reentrant scratch code. The Game Gear renderer is
; single-threaded; avoiding SDCC's 15-byte IX frame and five separate base+col
; address constructions in every candidate column is the whole point.

_ts_candidate_span_fast::
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ; Materialize the tiny projected span into fixed scratch once.
        ld      hl, (#_g_candidate_ctx + 3)
        ld      a, (hl)
        ld      (#cand_col$), a
        inc     hl
        ld      a, (hl)
        ld      (#cand_end$), a
        inc     hl
        ld      a, (hl)
        ld      (#cand_invq$), a
        inc     hl
        ld      a, (hl)
        ld      (#cand_invq$ + 1), a
        inc     hl
        ld      a, (hl)
        ld      (#cand_step$), a
        inc     hl
        ld      a, (hl)
        ld      (#cand_step$ + 1), a
        inc     hl
        ld      a, (hl)
        ld      (#cand_orig0$), a
        inc     hl
        ld      a, (hl)
        ld      (#cand_orig1$), a

        ; Clip left edge to the portal/view interval while keeping reciprocal
        ; interpolation aligned with the original projected segment.
        ld      a, (#_g_candidate_ctx + 1)
        ld      b, a
cand_clip_left$:
        ld      a, (#cand_col$)
        cp      b
        jr      nc, cand_left_ready$
        ld      hl, (#cand_invq$)
        ld      de, (#cand_step$)
        add     hl, de
        ld      (#cand_invq$), hl
        ld      hl, #cand_col$
        inc     (hl)
        jr      cand_clip_left$

cand_left_ready$:
        ; end=min(projected c1, view_c1)
        ld      a, (#cand_end$)
        ld      b, a
        ld      a, (#_g_candidate_ctx + 2)
        cp      b
        jr      nc, cand_end_ready$
        ld      (#cand_end$), a
cand_end_ready$:
        ld      a, (#cand_col$)
        ld      b, a
        ld      a, (#cand_end$)
        cp      b
        jp      c, cand_done$

        ; IX/IY walk the two byte-wide hot arrays. Wider winner-only fields are
        ; addressed only when a candidate actually wins the depth test.
        ld      a, (#cand_col$)
        ld      l, a
        ld      h, #0
        ld      de, #_g_best_seg
        add     hl, de
        push    hl
        pop     ix

        ld      a, (#cand_col$)
        ld      l, a
        ld      h, #0
        ld      de, #_g_best_inv
        add     hl, de
        push    hl
        pop     iy

        ld      de, (#cand_invq$)       ; current left reciprocal, Q6
        ld      bc, (#cand_step$)       ; signed Q6 step per coarse column

cand_loop$:
        ; next=inv+step; midpoint reciprocal is (inv+next)>>7. Because all
        ; visible reciprocal values are positive, one left shift then H is the
        ; exact low eight bits of that result.
        ld      h, d
        ld      l, e
        add     hl, bc
        ld      (#cand_next$), hl
        add     hl, de
        add     hl, hl
        ld      a, h
        ld      (#cand_mid$), a

        ld      a, 0 (ix)
        inc     a
        jr      z, cand_winner$
        ld      a, (#cand_mid$)
        cp      0 (iy)
        jr      c, cand_no_win$
        jr      z, cand_no_win$

cand_winner$:
        ld      a, (#_g_candidate_ctx + 0)
        ld      0 (ix), a
        ld      a, (#cand_mid$)
        ld      0 (iy), a

        ; Endpoint border flags need no signed bounds tests: cand_col is always
        ; 0..19, so an offscreen original endpoint simply cannot compare equal.
        xor     a
        ld      (#cand_border$), a
        ld      a, (#cand_col$)
        ld      h, a
        ld      a, (#cand_orig0$)
        cp      h
        jr      nz, cand_border_right$
        ld      a, #1
        ld      (#cand_border$), a
cand_border_right$:
        ld      a, (#cand_orig1$)
        cp      h
        jr      nz, cand_store_border$
        ld      a, (#cand_border$)
        or      #2
        ld      (#cand_border$), a
cand_store_border$:
        ld      a, (#cand_col$)
        ld      l, a
        ld      h, #0
        ld      a, l
        add     a, #<(_g_best_border)
        ld      l, a
        ld      a, h
        adc     a, #>(_g_best_border)
        ld      h, a
        ld      a, (#cand_border$)
        ld      (hl), a

        ; Store left Q6 reciprocal.
        ld      a, (#cand_col$)
        add     a, a
        ld      l, a
        ld      h, #0
        ld      a, l
        add     a, #<(_g_best_inv_l_q6)
        ld      l, a
        ld      a, h
        adc     a, #>(_g_best_inv_l_q6)
        ld      h, a
        ld      (hl), e
        inc     hl
        ld      (hl), d

        ; Store right Q6 reciprocal from fixed scratch without disturbing DE.
        ld      a, (#cand_col$)
        add     a, a
        ld      l, a
        ld      h, #0
        ld      a, l
        add     a, #<(_g_best_inv_r_q6)
        ld      l, a
        ld      a, h
        adc     a, #>(_g_best_inv_r_q6)
        ld      h, a
        ld      a, (#cand_next$)
        ld      (hl), a
        inc     hl
        ld      a, (#cand_next$ + 1)
        ld      (hl), a

cand_no_win$:
        ld      de, (#cand_next$)
        inc     ix
        inc     iy
        ld      hl, #cand_col$
        inc     (hl)
        ld      a, (hl)
        ld      h, a
        ld      a, (#cand_end$)
        cp      h
        jr      nc, cand_loop$

cand_done$:
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        ret

        .area   _BSS
cand_col$:      .ds 1
cand_end$:      .ds 1
cand_orig0$:    .ds 1
cand_orig1$:    .ds 1
cand_mid$:      .ds 1
cand_border$:   .ds 1
cand_invq$:     .ds 2
cand_step$:     .ds 2
cand_next$:     .ds 2
