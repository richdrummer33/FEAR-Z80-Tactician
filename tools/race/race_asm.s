        .title  "Z80 raster kernels, hand written"
        .module race_asm

        .area _HOME
        .globl _dda_span_asm
        .globl _pack_span_asm
        .globl _g_as_iq
        .globl _g_as_step
        .globl _g_as_n
        .globl _g_as_outp
        .globl _g_as_srcp
        .globl _g_as_mlen
        .globl _g_as_ret

; The SDCC C kernels exist to compare the SHAPE of the algorithms. They also
; exaggerate the budget available to a stream selector by roughly an order of
; magnitude, because the selector has to beat the DDA and an SDCC DDA is slow.
; These are the same two functions written by hand, so the budget is the real
; one. Arguments arrive in globals rather than on the stack: it sidesteps the
; calling convention entirely and costs a few loads that both kernels pay.
;
; Rows are biased by +64 throughout. They span -7..8, so biasing makes every
; comparison unsigned, which the Z80 does in one CP; signed compares would cost
; a sign/overflow dance on every column. Differences are unaffected by the bias.

; row(HL) -> A, biased by 64. Preserves HL, DE, BC.
;   a>>7 is the top nine bits of a 16-bit accumulator: H*2 plus bit 7 of L.
as_row:
        ld      a, h
        add     a, a
        bit     7, l
        jr      z, as_row_1
        inc     a
as_row_1:
        neg                             ; -(a>>7)
        add     a, #71                  ; 71 - (a>>7), the unshifted screen row
        sra     a
        sra     a
        sra     a                       ; >>3 into tile rows, signed
        add     a, #64                  ; bias
        ret

_dda_span_asm::
        push    ix
        ld      hl, (_g_as_iq)
        ld      bc, #32
        add     hl, bc                  ; hl = accumulator at column 0
        ld      de, (_g_as_outp)
        call    as_row
        ld      (as_r0), a
        ld      bc, (_g_as_step)
        add     hl, bc
        call    as_row
        ld      (as_r1), a
        ld      a, (_g_as_n)
        ld      (as_left), a

as_col:
        ; ndown = |r1 - r0|, hi = max(r0, r1)   (both biased, so unsigned)
        ld      a, (as_r1)
        ld      b, a
        ld      a, (as_r0)
        cp      b
        jr      nc, as_r0_hi
        ; r0 < r1: hi = r1, ndown = r1 - r0
        ld      a, b
        ld      (as_hi), a
        ld      a, (as_r0)
        neg
        add     a, b
        jr      as_have_nd
as_r0_hi:
        ld      (as_hi), a              ; hi = r0
        sub     b                       ; r0 - r1
as_have_nd:
        or      a
        jr      z, as_no_down
        ld      b, a
        xor     a
as_down:
        ld      (de), a
        inc     de
        djnz    as_down
as_no_down:
        ld      a, (as_left)
        dec     a
        ld      (as_left), a
        jr      nz, as_more
        ld      a, #6                   ; run terminator
        ld      (de), a
        inc     de
        jr      as_fin
as_more:
        ld      bc, (_g_as_step)
        add     hl, bc
        call    as_row                  ; r2
        ld      b, a
        ld      a, (as_r1)
        ld      c, a
        ld      (as_r0), a              ; r0 <- r1 for the next column
        ld      a, b
        ld      (as_r1), a              ; r1 <- r2
        ; lo1 = min(r1, r2) with r1 in C, r2 in B
        ld      a, c
        cp      b
        jr      c, as_lo1_c
        ld      a, b
as_lo1_c:
        ld      b, a                    ; b = lo1
        ld      a, (as_hi)
        sub     b                       ; hi - lo1, in 0..4
        inc     a                       ; the move byte is 1 + (hi - lo1)
        ld      (de), a
        inc     de
        jp      as_col
as_fin:
        ld      hl, (_g_as_outp)
        ex      de, hl
        sbc     hl, de                  ; bytes written; carry is clear here
        ld      a, l
        ld      (_g_as_ret), a
        pop     ix
        ret

; LDIR is the whole point: the Z80 copies a byte per 21 T-states with no loop
; overhead at all, which is what makes replay so hard to beat.
_pack_span_asm::
        ld      a, (_g_as_mlen)
        ld      (_g_as_ret), a
        or      a
        ret     z
        ld      c, a
        ld      b, #0
        ld      hl, (_g_as_srcp)
        ld      de, (_g_as_outp)
        ldir
        ret

        .area _BSS
as_r0:   .ds 1
as_r1:   .ds 1
as_hi:   .ds 1
as_left: .ds 1
