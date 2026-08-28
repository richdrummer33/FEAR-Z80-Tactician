        .title  "Polar cell-local bearing playback"
        .module tilesector_polar_projection_gg

        .area _HOME
        .globl _tsp_polar_projection_eval_fast
        .globl _g_proj_corner_ptr
        .globl _g_proj_corner_depth
        .globl _g_proj_lx
        .globl _g_proj_ly
        .globl _g_corner_bearing_q12

; Evaluate every baked corner descriptor for the current 4-world-unit cell.
; Runtime work is WRAM pointer/depth lookup plus two signed 8x6 scaled products.
; 0xff depth means absent or exact-fallback; C handles the tiny fallback mask.
_tsp_polar_projection_eval_fast::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ld      ix, #_g_proj_corner_depth
        ld      iy, #_g_corner_bearing_q12
        ld      hl, #_g_proj_corner_ptr
        ld      (#pe_ptrp$), hl
        ld      a, #14
        ld      (#pe_count$), a

pe_corner_loop$:
        ; Consume this corner's pointer entry even if depth says skip.
        ld      hl, (#pe_ptrp$)
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        inc     hl
        ld      (#pe_ptrp$), hl

        ld      a, 0 (ix)
        cp      #0xff
        jp      z, pe_corner_done$
        ld      (#pe_depth$), a
        ex      de, hl                  ; HL = first four-byte leaf record

        ; Depth zero is overwhelmingly common: one record covers full cell.
        or      a
        jp      z, pe_depth0$
        dec     a
        jp      z, pe_depth1$
        dec     a
        jp      z, pe_depth2$
        jp      pe_depth3$

pe_depth0$:
        ld      a, (_g_proj_lx)
        ld      (#pe_localx$), a
        ld      a, (_g_proj_ly)
        ld      (#pe_localy$), a
        ld      a, #6
        ld      (#pe_shift$), a
        jp      pe_leaf_ready$

pe_depth1$:
        ld      a, (_g_proj_lx)
        and     #31
        ld      (#pe_localx$), a
        ld      a, (_g_proj_ly)
        and     #31
        ld      (#pe_localy$), a
        ld      a, #5
        ld      (#pe_shift$), a

        ld      a, (_g_proj_ly)
        and     #0x20
        rrca
        rrca
        rrca
        rrca                            ; y leaf * 2
        ld      c, a
        ld      a, (_g_proj_lx)
        and     #0x20
        rrca
        rrca
        rrca
        rrca
        rrca                            ; x leaf
        or      c
        jp      pe_add_leaf4$

pe_depth2$:
        ld      a, (_g_proj_lx)
        and     #15
        ld      (#pe_localx$), a
        ld      a, (_g_proj_ly)
        and     #15
        ld      (#pe_localy$), a
        ld      a, #4
        ld      (#pe_shift$), a

        ld      a, (_g_proj_ly)
        and     #0x30
        rrca
        rrca                            ; y leaf * 4
        ld      c, a
        ld      a, (_g_proj_lx)
        and     #0x30
        rrca
        rrca
        rrca
        rrca                            ; x leaf
        or      c
        jp      pe_add_leaf4$

pe_depth3$:
        ld      a, (_g_proj_lx)
        and     #7
        ld      (#pe_localx$), a
        ld      a, (_g_proj_ly)
        and     #7
        ld      (#pe_localy$), a
        ld      a, #3
        ld      (#pe_shift$), a

        ld      a, (_g_proj_ly)
        and     #0x38                  ; already y leaf * 8
        ld      c, a
        ld      a, (_g_proj_lx)
        and     #0x38
        rrca
        rrca
        rrca                            ; x leaf
        or      c

pe_add_leaf4$:
        add     a, a
        add     a, a                    ; four bytes / leaf
        ld      e, a
        ld      d, #0
        add     hl, de

pe_leaf_ready$:
        ; Snapshot base and slopes before the scaled-product helper clobbers regs.
        ld      e, (hl)
        inc     hl
        ld      a, (hl)
        and     #0x0f
        ld      d, a
        ld      (#pe_base$), de
        inc     hl
        ld      a, (hl)
        ld      (#pe_sx$), a
        inc     hl
        ld      a, (hl)
        ld      (#pe_sy$), a

        ld      a, (#pe_sx$)
        ld      c, (#pe_localx$)
        ld      b, (#pe_shift$)
        call    pe_scaled_mul8$
        ld      (#pe_corrx$), a

        ld      a, (#pe_sy$)
        ld      c, (#pe_localy$)
        ld      b, (#pe_shift$)
        call    pe_scaled_mul8$
        ld      c, a                    ; signed y correction

        ld      hl, (#pe_base$)
        ; Add signed C.
        ld      e, c
        ld      d, #0
        bit     7, e
        jr      z, pe_y_positive$
        dec     d
pe_y_positive$:
        add     hl, de

        ; Add signed x correction.
        ld      a, (#pe_corrx$)
        ld      e, a
        ld      d, #0
        bit     7, e
        jr      z, pe_x_positive$
        dec     d
pe_x_positive$:
        add     hl, de
        ld      a, h
        and     #0x0f                   ; modulo one Q12 turn
        ld      h, a
        ld      0 (iy), l
        ld      1 (iy), h

pe_corner_done$:
        inc     ix
        inc     iy
        inc     iy
        ld      a, (#pe_count$)
        dec     a
        ld      (#pe_count$), a
        jp      nz, pe_corner_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; A=signed slope (-127..127), C=unsigned local coordinate (0..63),
; B=right shift 3..6. Return A=trunc_toward_zero(slope*local / 2^B).
pe_scaled_mul8$:
        ld      (#pe_mul_sign$), #0
        bit     7, a
        jr      z, pe_mul_abs_ready$
        neg
        ld      (#pe_mul_sign$), #1
pe_mul_abs_ready$:
        ld      e, a
        ld      d, #0
        ld      hl, #0
        ld      a, c

        srl     a
        jr      nc, pe_mul_b0$
        add     hl, de
pe_mul_b0$:
        sla     e
        rl      d
        srl     a
        jr      nc, pe_mul_b1$
        add     hl, de
pe_mul_b1$:
        sla     e
        rl      d
        srl     a
        jr      nc, pe_mul_b2$
        add     hl, de
pe_mul_b2$:
        sla     e
        rl      d
        srl     a
        jr      nc, pe_mul_b3$
        add     hl, de
pe_mul_b3$:
        sla     e
        rl      d
        srl     a
        jr      nc, pe_mul_b4$
        add     hl, de
pe_mul_b4$:
        sla     e
        rl      d
        srl     a
        jr      nc, pe_mul_b5$
        add     hl, de
pe_mul_b5$:

pe_mul_shift$:
        srl     h
        rr      l
        djnz    pe_mul_shift$
        ld      a, (#pe_mul_sign$)
        or      a
        ld      a, l
        ret     z
        neg
        ret

        .area _BSS
pe_ptrp$:       .ds 2
pe_count$:      .ds 1
pe_depth$:      .ds 1
pe_localx$:     .ds 1
pe_localy$:     .ds 1
pe_shift$:      .ds 1
pe_base$:       .ds 2
pe_sx$:         .ds 1
pe_sy$:         .ds 1
pe_corrx$:      .ds 1
pe_mul_sign$:   .ds 1
