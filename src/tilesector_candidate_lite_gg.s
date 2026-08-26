        .module tilesector_candidate_lite_gg
        .area   _CODE

        .globl  _g_candidate_lite_ctx
        .globl  _g_best_seg
        .globl  _g_best_inv

; Stage 12 candidate ABI:
;   context +0 seg_id, +1 view_c0, +2 view_c1, +3 span pointer
;   TSProjectedSpan +0 c0, +1 c1, +2 inv_q6, +4 step_q6,
;                   +6 original_c0, +7 original_c1
;
; Only winner segment and byte reciprocal survive. Left/right Q6 interpolation
; belongs to the later contiguous-run materializer, so the candidate stage no
; longer writes five arrays for every depth winner.

_ts_candidate_span_lite_fast::
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

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

        ; IX and IY advance directly through the only two candidate arrays that
        ; survive this stage.
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

        ld      de, (#cl_invq$)
        ld      bc, (#cl_step$)

cl_loop$:
        ; next=current+step. midpoint reciprocal=(current+next)>>7.
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
        ld      a, (#_g_candidate_lite_ctx + 0)
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

        .area   _BSS
cl_col$:       .ds 1
cl_end$:       .ds 1
cl_mid$:       .ds 1
cl_invq$:      .ds 2
cl_step$:      .ds 2
cl_next$:      .ds 2
