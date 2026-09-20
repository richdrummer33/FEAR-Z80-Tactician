        .title  "Exact-envelope local bearing evaluator"
        .module e1env_local_bearing_eval_gg

        .area   _HOME
        .globl  _e1env_local_bearing_eval
        .globl  _g_e1env_lbf_ptr
        .globl  _g_e1env_lbf_lx
        .globl  _g_e1env_lbf_ly
        .globl  _g_e1env_lbf_out

; Evaluate one four-byte descriptor already copied into WRAM:
;   [base_lo, packed, sx_s8, sy_s8]
;
; packed low nibble  = base bits 8..11
; packed high nibble = signed 4-bit bilinear coefficient sxy [-8,7]
;
; Normal affine records have sxy=0 and pay only one cheap branch. Rescue
; records add:
;   trunc(trunc(sxy*lx/16)*ly/16)
; which is exactly the arithmetic exhaustively validated by the baker.
;
; bearing = base + trunc(sx*lx/16) + trunc(sy*ly/16) + cross, modulo 4096.
_e1env_local_bearing_eval::
        push    bc
        push    de
        push    hl

        ld      hl, (#_g_e1env_lbf_ptr)
        ld      e, (hl)                 ; base low
        inc     hl
        ld      a, (hl)                 ; packed base high + signed sxy nibble
        ld      b, a                    ; retain packed byte through affine work
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

        ; High nibble zero is the overwhelmingly common affine path. Keep the
        ; old hot path intact apart from this test. For rescue records decode
        ; the signed 4-bit coefficient, then reuse the exact same signed 8x4
        ; truncating multiply twice. Preserve the already-accumulated bearing
        ; across the helper calls because they use HL as scratch.
        ld      a, b
        and     #0xf0
        jr      z, lbf_cross_done$
        rrca
        rrca
        rrca
        rrca                            ; A = 0..15 nibble
        bit     3, a
        jr      z, lbf_cross_sign_ready$
        or      #0xf0                   ; sign-extend -8..-1
lbf_cross_sign_ready$:
        push    hl
        ld      e, a                    ; preserve sxy while loading lx
        ld      a, (#_g_e1env_lbf_lx)
        ld      c, a
        ld      a, e
        call    lbf_mul_s8_u4_div16$
        ld      e, a                    ; first truncated product becomes slope
        ld      a, (#_g_e1env_lbf_ly)
        ld      c, a
        ld      a, e
        call    lbf_mul_s8_u4_div16$
        ld      c, a                    ; final signed cross correction
        pop     hl

        ld      e, c
        ld      d, #0
        bit     7, e
        jr      z, lbf_cross_pos$
        dec     d
lbf_cross_pos$:
        add     hl, de
lbf_cross_done$:

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
