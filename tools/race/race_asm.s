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

; ===========================================================================
; The selector ladder, hand written.
; ===========================================================================
; Every kernel below is the same span driver with a different lookup bolted in,
; so the difference between any two of them is the cost of naming a body and
; nothing else. The driver, the replay, and the chunk chaining are shared code.
;
; The replay is an LDIR of the body's baked move stream. The packed comparator
; already showed a copy is the floor for emitting moves, so measuring each
; selector against that comparator gives the naming cost directly.
;
; Arguments arrive in globals, as everywhere else in this file. (_g_as_ord) is
; the step ordinal: A0 is handed it, A1 has to earn it.

        .globl  _a0_span_asm
        .globl  _a1_span_asm
        .globl  _bfl_span_asm
        .globl  _bfb_span_asm
        .globl  _bpl_span_asm
        .globl  _g_as_ord
        .globl  _gg_sel_a0_0
        .globl  _gg_sel_a0_1
        .globl  _gg_sel_fix
        .globl  _gg_sel_pblob
        .globl  _gg_sel_a1lo
        .globl  _gg_sel_a1hi
        .globl  _gg_sel_poff
        .globl  _gg_sel_bstream
        .globl  _gg_sel_bptr
        .globl  _gg_sel_bpre

        .area _HOME
MAPFRAME2 = 0xFFFF

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

; --- A0: the ideal oracle --------------------------------------------------
; A dense byte per (step ordinal, phase). A row is 1024 bytes and 1024 divides
; a 16 KB bank, so a row never straddles one and the bank is the ordinal's high
; nibble. The switch is hoisted to span setup because the step does not change
; inside a span; that is the honest placement, not a favour.
_a0_span_asm::
        call    a0_base
        ld      hl, #lk_a0
        ld      (as_lk), hl
        jp      sel_enter

a0_base:
        ld      a, (_g_as_ord)
        cp      #16
        jr      nc, a0b_hi
        ld      c, a
        ld      a, #2
        ld      (MAPFRAME2), a
        ld      hl, #_gg_sel_a0_0
        jr      a0b_go
a0b_hi:
        sub     #16
        ld      c, a
        ld      a, #3
        ld      (MAPFRAME2), a
        ld      hl, #_gg_sel_a0_1
a0b_go:
        ld      a, c
        add     a, a
        add     a, a                    ; (ord & 15) * 1024, in the high byte
        add     a, h
        ld      h, a
        ld      (as_base), hl
        ret

lk_a0:
        ld      hl, (as_base)
        add     hl, bc
        ld      a, (hl)
        ret

; --- A1: the same table, paying for the ordinal ----------------------------
_a1_span_asm::
        ld      a, #6
        ld      (MAPFRAME2), a
        ld      a, (_g_as_step + 1)
        ld      c, a
        ld      b, #0
        ld      hl, #_gg_sel_a1hi
        add     hl, bc
        ld      a, (hl)                 ; which low page this step's high byte uses
        add     a, a
        ld      h, a
        ld      l, #0                   ; page * 512
        ld      bc, #_gg_sel_a1lo
        add     hl, bc
        ld      a, (_g_as_step)
        ld      c, a
        ld      b, #0
        add     hl, bc
        add     hl, bc                  ; two-byte entries
        ld      a, (hl)
        inc     hl
        ld      h, (hl)                 ; the ordinal's high byte, read and charged
        ld      (_g_as_ord), a
        call    a0_base
        ld      hl, #lk_a0
        ld      (as_lk), hl
        jp      sel_enter

; --- B: exact-step phase intervals, fixed eight-slot records ---------------
; Twenty-four bytes a step, always: eight slots of high threshold byte, low
; threshold byte, body id. Unused slots carry 0xFFFF, which a descending scan
; steps over and an ascending search treats as above everything.
fix_base:
        ld      a, #4
        ld      (MAPFRAME2), a
        ld      a, (_g_as_ord)
        ld      l, a
        ld      h, #0
        add     hl, hl
        add     hl, hl
        add     hl, hl                  ; ord * 8
        ld      b, h
        ld      c, l
        add     hl, hl                  ; ord * 16
        add     hl, bc                  ; ord * 24
        ld      bc, #_gg_sel_fix
        add     hl, bc
        ld      (as_base), hl           ; slot 0
        ld      bc, #21
        add     hl, bc
        ld      (as_base2), hl          ; slot 7
        ret

_bfl_span_asm::
        call    fix_base
        ld      hl, #lk_flin
        ld      (as_lk), hl
        jp      sel_enter
_bfb_span_asm::
        call    fix_base
        ld      hl, #lk_fbin
        ld      (as_lk), hl
        jp      sel_enter

; linear, descending. Slot zero's threshold is zero and the phase is never
; negative, so the walk always terminates and needs no counter at all.
lk_flin:
        ld      hl, (as_base2)
fl_loop:
        ld      a, (hl)
        cp      b
        jr      c, fl_hit
        jr      nz, fl_next
        inc     hl
        ld      a, (hl)
        dec     hl
        cp      c
        jr      z, fl_hit
        jr      c, fl_hit
fl_next:
        dec     hl
        dec     hl
        dec     hl
        jr      fl_loop
fl_hit:
        inc     hl
        inc     hl
        ld      a, (hl)
        ret

; balanced, three compares, constant time. This is what the fixed layout buys
; that the variable-length one cannot: the slot count is known, so the search
; can jump by baked strides instead of walking.
lk_fbin:
        push    de
        ld      hl, (as_base)
        ld      de, #12
        add     hl, de
        call    fb_le
        jr      c, fb_1
        ld      de, #-12
        add     hl, de
fb_1:
        ld      de, #6
        add     hl, de
        call    fb_le
        jr      c, fb_2
        ld      de, #-6
        add     hl, de
fb_2:
        ld      de, #3
        add     hl, de
        call    fb_le
        jr      c, fb_3
        ld      de, #-3
        add     hl, de
fb_3:
        pop     de
        inc     hl
        inc     hl
        ld      a, (hl)
        ret
fb_le:
        ld      a, (hl)
        cp      b
        jr      c, fb_yes
        jr      nz, fb_no
        inc     hl
        ld      a, (hl)
        dec     hl
        cp      c
        jr      z, fb_yes
        jr      c, fb_yes
fb_no:
        or      a
        ret
fb_yes:
        scf
        ret

; --- B: the same intervals in packed variable-length records ---------------
; Three bytes a band, descending, and the last band's threshold is zero, so the
; scan stops by itself and no count byte is needed. The record is found through
; a two-byte pointer instead of a shift, and that pointer table is the price of
; packing.
_bpl_span_asm::
        ld      a, #5
        ld      (MAPFRAME2), a
        ld      a, (_g_as_ord)
        ld      l, a
        ld      h, #0
        add     hl, hl
        ld      bc, #_gg_sel_poff
        add     hl, bc
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a
        ld      bc, #_gg_sel_pblob
        add     hl, bc
        ld      (as_base), hl
        ld      hl, #lk_pak
        ld      (as_lk), hl
        jp      sel_enter

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

        .area _BSS
as_acc:   .ds 2
as_step6: .ds 2
as_base:  .ds 2
as_base2: .ds 2
as_body:  .ds 2
as_src:   .ds 2
as_lk:    .ds 2
