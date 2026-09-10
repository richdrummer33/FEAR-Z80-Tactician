; ================= DDA_G: row extents by walking, not by dividing =================
;
; The old kernel derived rows the expensive way, SIX times per column: both
; edges and the interior each ran two signed 16-bit compares (`cmps`) and two
; `rowfloor` calls, and `rowfloor`'s negative branch negates, adds 7, shifts
; three times and negates again.
;
; None of that is needed. Every endpoint is a monotonic function of one height
; byte, so the min/max endpoints are chosen by ONE byte compare per column,
; and every value that could be negative clamps to row 0 or skips the draw
; outright - so `row_floor` is only ever needed on a non-negative value, where
; it is three `srl`s.
;
;   FULL    top 71-h        bottom 72+h
;   LINTEL  top 72-h        bottom 72-(h>>1)
;   RAISED  top 72-h        bottom 72+h-(h>>2)
;   RISER   top 72+h-(h>>2) bottom 72+h
;
; A decreasing form takes its min at hmax; an increasing form at hmin.
;
; Outputs, all bytes: TOPR0/TOPR1, BOTR0/BOTR1, FULR0/FULR1. "Nothing to
; draw" is expressed as r0=1, r1=0, which the existing r1<r0 test already
; rejects - no new control flow, no new failure mode.
;
; Only 72-h and 71-h can go negative. 72+h-(h>>2) reaches 168 and 72+h
; reaches 199, so those are shifted as UNSIGNED; testing bit 7 on them would
; read 168 as negative and blank the column, the same trap that bit RAISED
; in the per-column kernel.
col_bounds:
        ld a,(0xc028)                ; HLH
        ld b,a
        ld a,(0xc029)                ; HRH
        cp b
        jp nc,cb_hmax_r
        ld c,b                       ; hmax = HLH
        ld b,a                       ; hmin = HRH
        jp cb_h
cb_hmax_r:
        ld c,a                       ; hmax = HRH, hmin = HLH already in B
cb_h:
        ld a,(0xc004)                ; PROFILE
        cp 3
        jp z,cb_top_riser

; ---- decreasing top: min at hmax, max at hmin ----
        ld a,(0xc004)
        or a
        ld a,72
        jp nz,cb_top_dec
        ld a,71                      ; FULL: tl--
cb_top_dec:
        ld e,a                       ; E = the constant, 71 or 72
        sub c
        call cb_r0_signed            ; TOPR0 from tmin (may be negative)
        ld (0xc050),a
        ld (0xc058),a                ; MASKLO: the column's true first row.
; TOPR0 is overwritten to 1 when the whole top edge is above the screen, to
; disable that edge. The coverage mask must NOT see that sentinel - the column
; still draws its bottom edge and interior from row 0 - so the true lower
; bound is kept separately here.
        ld a,e
        sub b
        call cb_r1_signed            ; TOPR1 from tmax (may be negative)
        ld (0xc051),a
        ld a,e
        sub b
        call cb_full_first           ; FULR0 = row_floor(tmax)+1, clamped
        ld (0xc054),a
        jp cb_bottom
cb_top_riser:
        ld a,b
        call cb_riser                ; 72 + hmin - (hmin>>2), always >= 72
        srl a
        srl a
        srl a
        ld (0xc050),a
        ld (0xc058),a                ; MASKLO
        ld a,c
        call cb_riser
        call cb_r1_pos
        ld (0xc051),a
        ld a,c
        call cb_riser
        srl a
        srl a
        srl a
        inc a
        ld (0xc054),a                ; FULR0

; ---- bottom: never negative (the lowest form is 72-(h>>1) >= 9) ----
cb_bottom:
        ld a,(0xc004)
        cp 1
        jp z,cb_bot_lintel
        cp 2
        jp z,cb_bot_raised
        ld a,b
        add a,72                     ; bmin = 72 + hmin
        ld (0xc057),a
        ld a,c
        add a,72                     ; bmax = 72 + hmax
        jp cb_bot_store
cb_bot_lintel:
        ld a,c
        srl a
        ld e,a
        ld a,72
        sub e
        ld (0xc057),a                ; bmin = 72 - (hmax>>1)
        ld a,b
        srl a
        ld d,a
        ld a,72
        sub d                        ; bmax = 72 - (hmin>>1)
        jp cb_bot_store
cb_bot_raised:
        ld a,b
        call cb_riser                ; cb_riser clobbers D and E, so bmin goes
        ld (0xc057),a                ; to memory rather than a register
        ld a,c
        call cb_riser                ; bmax = 72 + hmax - (hmax>>2)
cb_bot_store:
; A = bmax, (0xc057) = bmin
        push af
        ld a,(0xc057)
        srl a
        srl a
        srl a
        ld (0xc052),a                ; BOTR0 = bmin>>3
        dec a
        ld (0xc055),a                ; FULR1 = (bmin>>3) - 1
        pop af
        call cb_r1_pos
        ld (0xc053),a                ; BOTR1 = min(bmax>>3, 17)
        ld a,(0xc055)
        cp 18
        jp c,cb_ful_ok
        ld a,17
        ld (0xc055),a
cb_ful_ok:
        ret

; A(i8) -> row for a LOWER bound: negative clamps to row 0
cb_r0_signed:
        bit 7,a
        jp z,cb_r0_pos
        xor a
        ret
cb_r0_pos:
        srl a
        srl a
        srl a
        ret

; A(i8) -> row for an UPPER bound: negative means nothing to draw, and the
; caller's r0 is 0 or more, so returning 0 alone would draw row 0 by mistake.
; Return 0 and force r0 past it instead.
cb_r1_signed:
        bit 7,a
        jp z,cb_r1_pos
        ld a,1
        ld (0xc050),a                ; r0 = 1 > r1 = 0 -> the r1<r0 test skips
        xor a
        ret

; A(u8) -> min(A>>3, 17)
cb_r1_pos:
        srl a
        srl a
        srl a
        cp 18
        ret c
        ld a,17
        ret

; A(i8) = tmax -> row_floor(tmax)+1 clamped at 0
cb_full_first:
        bit 7,a
        jp z,cb_ff_pos
        xor a
        ret
cb_ff_pos:
        srl a
        srl a
        srl a
        inc a
        ret

; A(u8) = h -> 72 + h - (h>>2)
cb_riser:
        ld d,a
        srl a
        srl a
        ld e,a
        ld a,d
        sub e
        add a,72
        ret
