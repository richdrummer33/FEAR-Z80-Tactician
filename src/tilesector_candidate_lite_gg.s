        .module tilesector_candidate_lite_gg
        .area   _CODE

        .globl  _g_candidate_lite_ctx
        .globl  _g_best_seg
        .globl  _g_best_inv

; STAGE27_CANDIDATE_PHASE
;
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

        ; STAGE27: pre-scale the affine reciprocal line once so the high byte
        ; IS the exact Stage-12 midpoint-quantized depth for each column.
        ;
        ; phase0 = 4*current + 2*step
        ; delta  = 4*step
        ; q[col] = high8(phase), phase += delta
        ;
        ; All operations wrap at 16 bits exactly like the old Z80 sequence.
        ld      hl, (#cl_invq$)
        add     hl, hl
        add     hl, hl
        ld      de, (#cl_step$)
        add     hl, de
        add     hl, de
        push    hl                      ; phase0

        ld      h, d
        ld      l, e
        add     hl, hl
        add     hl, hl
        ld      b, h
        ld      c, l                    ; BC = phase delta
        pop     de                      ; DE = phase

cl_loop$:
        ld      a, d                    ; exact quantized midpoint reciprocal

        ld      a, 0 (ix)
        inc     a
        jr      z, cl_winner$
        ld      a, d
        cp      0 (iy)
        jr      c, cl_no_win$
        jr      z, cl_no_win$

cl_winner$:
        ld      a, (#cl_seg$)
        ld      0 (ix), a
        ld      a, d
        ld      0 (iy), a

cl_no_win$:
        ld      h, d
        ld      l, e
        add     hl, bc
        ld      d, h
        ld      e, l                    ; phase += delta
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
cl_seg$:       .ds 1
cl_col$:       .ds 1
cl_end$:       .ds 1
cl_invq$:      .ds 2
cl_step$:      .ds 2
