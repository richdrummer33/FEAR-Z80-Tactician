        .title  "Full-domain selector, hand written"
        .module stress_asm

        .area _HOME
        .globl  _dda_span_asm
        .globl  _bplf_span_asm
        .globl  _g_as_iq
        .globl  _g_as_step
        .globl  _g_as_n
        .globl  _g_as_outp
        .globl  _g_as_ret
        .globl  _gg_selfull_hi
        .globl  _gg_selfull_map
        .globl  _gg_sel_bstream
        .globl  _gg_sel_bptr
        .globl  _gg_sel_bpre

MAPFRAME2  = 0xFFFF
; These must match selfull_hdr.h; stress_kernels.c asserts it at compile time.
SEL_BANK0  = 2
SEL_MAPBANK = 7

; The production selector on the full-domain tables. The race version had all 22
; steps in one bank and a two-byte offset; at full domain a step's record lives
; in one of five banks, so the map entry is three bytes -- bank, then in-bank
; address -- and the span pays two switches: one to read the map, one to reach
; the record. Both are per span, because the step does not change inside one.
_bplf_span_asm::
        ld      a, (_g_as_step + 1)
        ld      c, a
        ld      b, #0
        ld      hl, #_gg_selfull_hi     ; fixed bank: readable whatever is paged
        add     hl, bc
        ld      a, (hl)                 ; which page this step's high byte uses
        ld      b, a
        add     a, a
        add     a, b                    ; page * 3
        ld      h, a
        ld      l, #0                   ; page * 768, the page stride
        ld      bc, #_gg_selfull_map
        add     hl, bc
        ld      a, (_g_as_step)
        ld      c, a
        ld      b, #0
        add     hl, bc
        add     hl, bc
        add     hl, bc                  ; three bytes per entry
        ld      a, #SEL_MAPBANK
        ld      (MAPFRAME2), a
        ld      a, (hl)                 ; the record's bank
        inc     hl
        ld      c, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, c                    ; the record's in-bank address
        add     a, #SEL_BANK0
        ld      (MAPFRAME2), a
        ld      bc, #0x8000             ; every fixed bank is paged at Frame 2
        add     hl, bc
        ld      (as_base), hl
        ld      hl, #lk_pak
        ld      (as_lk), hl
        jp      sel_enter

; --- shared span driver ----------------------------------------------------
; (as_lk) holds this kernel's lookup. The indirect costs twenty T-states a
; chunk more than a direct call would; every selector pays it identically and
; the DDA pays none of it, so it can only understate the selectors' advantage.
sel_enter:
        ld      hl, (_g_as_iq)
        ld      bc, #32
        add     hl, bc
        ld      (as_acc), hl
        ld      hl, (_g_as_step)
        ld      b, h
        ld      c, l
        add     hl, hl                  ; 2*step
        add     hl, bc                  ; 3*step
        add     hl, hl                  ; 6*step, the chunk stride
        ld      (as_step6), hl
        ld      a, (_g_as_n)
        ld      (as_left), a
        ld      de, (_g_as_outp)
        call    sel_chunk
        ld      hl, (_g_as_outp)
        ex      de, hl
        or      a
        sbc     hl, de
        ld      a, l
        ld      (_g_as_ret), a
        ret

sel_chunk:
        ld      hl, (as_acc)
        ld      a, h
        and     #3
        ld      b, a
        ld      c, l                    ; bc = the ten-bit phase
        call    sel_call_lk             ; a = body id
        ld      l, a
        ld      h, #0
        ld      (as_body), hl
        add     hl, hl
        ld      bc, #_gg_sel_bptr
        add     hl, bc
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a
        ld      bc, #_gg_sel_bstream    ; the table holds an offset, not an address
        add     hl, bc
        ld      (as_src), hl            ; the body's move stream
        ld      hl, (as_body)
        add     hl, hl
        add     hl, hl
        add     hl, hl
        ld      bc, #_gg_sel_bpre
        add     hl, bc                  ; the body's per-column prefix lengths
        ld      a, (as_left)
        cp      #7
        jr      c, sel_tail
        ld      bc, #5
        add     hl, bc
        ld      c, (hl)
        inc     c                       ; six full columns: prefix five plus the jump byte
        ld      b, #0
        ld      hl, (as_src)
        ldir
        ld      a, (as_left)
        sub     #6
        ld      (as_left), a
        ld      hl, (as_acc)
        ld      bc, (as_step6)
        add     hl, bc
        ld      (as_acc), hl
        jp      sel_chunk
sel_tail:
        dec     a                       ; the run ends on column (left-1)
        ld      c, a
        ld      b, #0
        add     hl, bc
        ld      c, (hl)
        ld      b, #0
        ld      hl, (as_src)
        ld      a, c
        or      a
        jr      z, sel_tail_0
        ldir
sel_tail_0:
        ld      a, #6                   ; the terminator replaces the jump byte
        ld      (de), a
        inc     de
        ret
sel_call_lk:
        ld      hl, (as_lk)
        jp      (hl)


lk_pak:
        ld      hl, (as_base)
pk_loop:
        ld      a, (hl)
        cp      b
        jr      c, pk_hit
        jr      nz, pk_next
        inc     hl
        ld      a, (hl)
        dec     hl
        cp      c
        jr      z, pk_hit
        jr      c, pk_hit
pk_next:
        inc     hl
        inc     hl
        inc     hl
        jr      pk_loop
pk_hit:
        inc     hl
        inc     hl
        ld      a, (hl)
        ret


; the hand DDA, lifted verbatim from the race so the on-device comparison is
; against the same reference implementation the race validated.
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

        .area _BSS
as_r0:   .ds 1
as_r1:   .ds 1
as_hi:   .ds 1
as_left: .ds 1
as_acc:   .ds 2
as_step6: .ds 2
as_base:  .ds 2
as_body:  .ds 2
as_src:   .ds 2
as_lk:    .ds 2
