; =========================================================== helpers ====
; carry = (HL < DE) SIGNED. HL and DE preserved.
;
; `sbc hl,de` gives an UNSIGNED comparison. Using its carry directly on
; possibly-negative operands reads -7 as 65529, which is exactly the bug that
; made every edge tile clamp to its maximum here. A7 already solved this for
; the decode-clip kernel - XOR bit 15 of both operands maps two's-complement
; ordering onto unsigned ordering - and this is the same primitive reused
; rather than re-derived.
cmps:
        push hl
        push de
        ld a,h
        xor 0x80
        ld h,a
        ld a,d
        xor 0x80
        ld d,a
        or a
        sbc hl,de
        pop de
        pop hl
        ret
; HL(i16) -> A = clamp(HL>>6, 0, 255).
; v>>6 == (int8)H*4 + (L>>6). Exact for any 16-bit v; the left-shift form
; would overflow here (iq+step+32 reaches ~16,400, and <<2 exceeds 16 bits).
shr6_clamp:
        ld a,l
        rlca
        rlca
        and 3
        ld c,a                       ; C = L>>6
        ld a,h
        ld e,a
        ld d,0
        bit 7,e
        jp z,s6_pos
        ld d,0xff
s6_pos:
        ex de,hl                     ; HL = sign-extended H
        add hl,hl
        add hl,hl                    ; *4
        ld e,c
        ld d,0
        add hl,de
        bit 7,h
        jp z,s6_nonneg
        xor a                        ; negative -> 0
        ret
s6_nonneg:
        ld a,h
        or a
        jp z,s6_fits
        ld a,255                     ; >255 -> 255
        ret
s6_fits:
        ld a,l
        ret

; A(i8) -> sign-extended i16 stored at TL / TR / BL / BR
sx_tl:
        call sx_hl
        ld ({TL:#06x}),hl
        ret
sx_tr:
        call sx_hl
        ld ({TR:#06x}),hl
        ret
sx_bl:
        call sx_hl
        ld ({BL:#06x}),hl
        ret
sx_br:
        call sx_hl
        ld ({BR:#06x}),hl
        ret
sx_hl:
        ld l,a
        ld h,0
        bit 7,l
        ret z
        ld h,0xff
        ret

; ZERO-extending stores. RAISED's bl = 72+h-(h>>2) and RISER's tl reach 168,
; so sign-extending them turns every value over 127 negative - which is what
; blanked every RAISED column. Only `72 - h` can actually go negative.
zx_bl:
        ld l,a
        ld h,0
        ld ({BL:#06x}),hl
        ret
zx_br:
        ld l,a
        ld h,0
        ld ({BR:#06x}),hl
        ret
zx_tl:
        ld l,a
        ld h,0
        ld ({TL:#06x}),hl
        ret
zx_tr:
        ld l,a
        ld h,0
        ld ({TR:#06x}),hl
        ret

; HL(i16) -> A = row_floor(HL) = HL>=0 ? HL>>3 : -(((-HL)+7)>>3)
rowfloor:
        bit 7,h
        jp z,rf_pos
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a                       ; HL = -HL
        ld de,7
        add hl,de
        call shr3_u
        neg
        ret
rf_pos:
        call shr3_u
        ret
shr3_u:
        srl h
        rr l
        srl h
        rr l
        srl h
        rr l
        ld a,l
        ret

; ---- draw_edge: YL, YR, BOTTOM ----
draw_edge:
        ld hl,({YR:#06x})
        ld de,({YL:#06x})
        or a
        sbc hl,de                    ; HL = yr - yl (exact, sign irrelevant here)
        ld de,0xfff9                 ; -7
        call cmps
        jp nc,de_sl_lo
        ld hl,0xfff9
de_sl_lo:
        ld de,8                      ; > 7  <=>  < 8 is false
        call cmps
        jp c,de_sl_hi
        ld hl,7
de_sl_hi:
        ld a,l
        ld ({SLOPE:#06x}),a
de_rows2:
; r0 = row_floor(min(yl,yr)), r1 = row_floor(max(yl,yr))
        ld hl,({YL:#06x})
        ld de,({YR:#06x})
        call cmps
        jp c,de_yl_min
        ld hl,({YR:#06x})
de_yl_min:
        call rowfloor
        ld ({R0:#06x}),a
        ld hl,({YL:#06x})
        ld de,({YR:#06x})
        call cmps
        jp nc,de_yl_max
        ld hl,({YR:#06x})
de_yl_max:
        call rowfloor
        ld ({R1:#06x}),a
        ld a,({R0:#06x})
        bit 7,a
        jp z,de_r0ok
        xor a
        ld ({R0:#06x}),a
de_r0ok:
        ld a,({R1:#06x})
        bit 7,a
        jp nz,de_r1neg
        cp {ROWS}
        jp c,de_r1ok
        ld a,{ROWS - 1}
        ld ({R1:#06x}),a
        jp de_r1ok
de_r1neg:
        ret                          ; r1 < 0 -> nothing to draw
de_r1ok:
        ld a,({R0:#06x})
        ld b,a
        ld a,({R1:#06x})
        cp b
        ret c                        ; r1 < r0 -> nothing
de_loop:
; local_left = YL - (r<<3)
        ld a,({R0:#06x})
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ex de,hl
        ld hl,({YL:#06x})
        or a
        sbc hl,de
        ld ({LOCL:#06x}),hl
        call edge_entry
        ld a,({R0:#06x})
        ld l,a
        ld h,0
        add hl,hl
        ld de,{COLBUF:#06x}
        add hl,de
        ld a,({TMP:#06x})
        ld (hl),a
        inc hl
        ld a,(0x{TMP + 1:04X})
        ld (hl),a
        ld a,({R0:#06x})
        ld b,a
        ld a,({R1:#06x})
        cp b
        ret z
        ld a,b
        inc a
        ld ({R0:#06x}),a
        jp de_loop

; ---- edge_entry: LOCL, SLOPE, BOTTOM, EDGEBASE -> TMP (word) ----
edge_entry:
        ld hl,0
        ld ({ATTR:#06x}),hl
        ld a,({SLOPE:#06x})
        ld c,a                       ; C = slope (i8)
        ld a,({BOTTOM:#06x})
        or a
        jp z,ee_top
        ld hl,7
        ld de,({LOCL:#06x})
        or a
        sbc hl,de
        ld ({LOCL:#06x}),hl          ; local_left = 7 - local_left
        ld a,c
        neg
        ld c,a                       ; slope = -slope
        ld hl,0x0c00                 ; FLIPY | PALETTE
        ld ({ATTR:#06x}),hl
ee_top:
        ld a,c
        bit 7,a
        jp z,ee_pos
        neg
        ld ({MAG:#06x}),a
        ld e,a
        ld d,0
        ld hl,({LOCL:#06x})
        or a
        sbc hl,de
        ld ({LOCL:#06x}),hl          ; local_left -= mag
        ld hl,({ATTR:#06x})
        ld de,0x0200                 ; FLIPX
        add hl,de
        ld ({ATTR:#06x}),hl
        jp ee_mag_ok
ee_pos:
        ld ({MAG:#06x}),a
ee_mag_ok:
        ld a,({MAG:#06x})
        cp 8
        jp c,ee_mag2
        ld a,7
        ld ({MAG:#06x}),a
ee_mag2:
; off = clamp(local_left, -7, 8) ; off_index = off + 7
        ld hl,({LOCL:#06x})
        ld de,0xfff9                 ; -7
        call cmps
        jp nc,ee_lo_ok
        ld hl,0xfff9                 ; local_left < -7
ee_lo_ok:
        ld de,9                      ; > 8  <=>  < 9 is false
        call cmps
        jp c,ee_hi_ok
        ld hl,8
ee_hi_ok:
        ld de,7
        add hl,de                    ; off_index 0..15
        add hl,hl
        add hl,hl
        add hl,hl                    ; *8
        ld de,({EDGEBASE:#06x})
        add hl,de
        ld a,({MAG:#06x})
        ld e,a
        ld d,0
        add hl,de
        ld de,({ATTR:#06x})
        add hl,de
        ld ({TMP:#06x}),hl
        ret

; ---- draw_full: rows row_floor(max tl,tr)+1 .. row_floor(min bl,br)-1 ----
draw_full:
        ld hl,({TL:#06x})
        ld de,({TR:#06x})
        call cmps
        jp nc,df_tmax
        ld hl,({TR:#06x})
df_tmax:
        call rowfloor
        inc a
        ld ({R0:#06x}),a
        ld hl,({BL:#06x})
        ld de,({BR:#06x})
        call cmps
        jp c,df_bmin
        ld hl,({BR:#06x})
df_bmin:
        call rowfloor
        dec a
        ld ({R1:#06x}),a
        ld a,({R0:#06x})
        bit 7,a
        jp z,df_r0ok
        xor a
        ld ({R0:#06x}),a
df_r0ok:
        ld a,({R1:#06x})
        bit 7,a
        ret nz
        cp {ROWS}
        jp c,df_r1ok
        ld a,{ROWS - 1}
        ld ({R1:#06x}),a
df_r1ok:
        ld a,({R0:#06x})
        ld b,a
        ld a,({R1:#06x})
        cp b
        ret c
df_loop:
        ld a,({R0:#06x})
        ld l,a
        ld h,0
        add hl,hl
        ld de,{COLBUF:#06x}
        add hl,de
        ld a,({FULLBASE:#06x})
        ld (hl),a
        inc hl
        ld a,(0x{FULLBASE + 1:04X})
        ld (hl),a
        ld a,({R0:#06x})
        ld b,a
        ld a,({R1:#06x})
        cp b
        ret z
        ld a,b
        inc a
        ld ({R0:#06x}),a
        jp df_loop
