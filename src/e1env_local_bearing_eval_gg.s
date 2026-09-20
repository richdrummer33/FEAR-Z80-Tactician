        .title  "Exact-envelope local bearing evaluator"
        .module e1env_local_bearing_eval_gg

        .area   _HOME
        .globl  _e1env_local_bearing_eval
        .globl  _g_e1env_lbf_ptr
        .globl  _g_e1env_lbf_lx
        .globl  _g_e1env_lbf_ly
        .globl  _g_e1env_lbf_out

; Evaluate one four-byte affine descriptor already copied into WRAM:
;   [base_lo, base_hi4, sx_s8, sy_s8]
; bearing = base + trunc(sx*lx/16) + trunc(sy*ly/16), modulo 4096.
;
; The hot multiply is signed 8x4, not generic 16x16. Local coordinates are
; Q4 fractions 0..15, so only four shift/add bits are needed.
_e1env_local_bearing_eval::
        push    bc
        push    de
        push    hl

        ld      hl, (#_g_e1env_lbf_ptr)
        ld      e, (hl)                 ; base low
        inc     hl
        ld      a, (hl)                 ; base high nibble
        and     #0x0f
        ld      d, a
        ld      (#lbf_base$), de
        inc     hl
        ld      a, (hl)
        ld      (#lbf_slope$), a
        inc     hl
        ld      a, (hl)
        ld      (#lbf_sy$), a

        ld      a, (#_g_e1env_lbf_lx)
        ld      c, a
        ld      a, (#lbf_slope$)
        call    lbf_mul_s8_u4_div16$
        ld      (#lbf_corrx$), a

        ld      a, (#_g_e1env_lbf_ly)
        ld      c, a
        ld      a, (#lbf_sy$)
        call    lbf_mul_s8_u4_div16$
        ld      c, a

        ld      hl, (#lbf_base$)

        ; Add signed Y correction.
        ld      e, c
        ld      d, #0
        bit     7, e
        jr      z, lbf_y_pos$
        dec     d
lbf_y_pos$:
        add     hl, de

        ; Add signed X correction.
        ld      a, (#lbf_corrx$)
        ld      e, a
        ld      d, #0
        bit     7, e
        jr      z, lbf_x_pos$
        dec     d
lbf_x_pos$:
        add     hl, de

        ld      a, h
        and     #0x0f
        ld      h, a
        ld      (#_g_e1env_lbf_out), hl

        pop     hl
        pop     de
        pop     bc
        ret

; A=signed slope [-127,127], C=local [0,15].
; Return A=trunc_toward_zero(A*C/16).
lbf_mul_s8_u4_div16$:
        ld      e, a
        xor     a
        ld      (#lbf_sign$), a
        ld      a, e
        bit     7, a
        jr      z, lbf_abs_ready$
        neg
        ld      e, a
        ld      a, #1
        ld      (#lbf_sign$), a
        jr      lbf_mag_ready$
lbf_abs_ready$:
        ld      e, a
lbf_mag_ready$:
        ld      d, #0
        ld      hl, #0
        ld      a, c

        srl     a
        jr      nc, lbf_b0$
        add     hl, de
lbf_b0$:
        sla     e
        rl      d
        srl     a
        jr      nc, lbf_b1$
        add     hl, de
lbf_b1$:
        sla     e
        rl      d
        srl     a
        jr      nc, lbf_b2$
        add     hl, de
lbf_b2$:
        sla     e
        rl      d
        srl     a
        jr      nc, lbf_b3$
        add     hl, de
lbf_b3$:
        ; Divide magnitude by 16.
        srl     h
        rr      l
        srl     h
        rr      l
        srl     h
        rr      l
        srl     h
        rr      l

        ld      a, (#lbf_sign$)
        or      a
        ld      a, l
        ret     z
        neg
        ret

        .area _BSS
lbf_base$:      .ds 2
lbf_slope$:     .ds 1
lbf_sy$:        .ds 1
lbf_corrx$:     .ds 1
lbf_sign$:      .ds 1
