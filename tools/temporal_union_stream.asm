; ============ UNION_S: the dirty union over a SPAN STREAM ============
;
; A33 measured the retained-COLUMN union at 35-55% of a full render and put the
; cost in per-column iteration and presence testing, not in any real work. The
; span stream removes both: a span's columns are contiguous c0..c1, so there is
; no presence test, and a 6-byte record compare proves ALL of that span's
; columns match, so an unchanged span costs six byte-compares and nothing else.
;
; Host measurement backing this (U=1, all regimes): 3.19 visible spans per
; frame, 39.6% of them unchanged, 23.90 column-materializations. The column
; form compares 71.57 bytes per update; this compares 19.12.
;
; RECORD, 8 bytes, and that is the whole retained state for a span
;   +0 keyid   identity across poses. sid is NOT unique - 1,340 frames in
;              1,789,440 carry two visible spans with the same sid.
;   +1 sid     supplies profile and shade bias
;   +2 inv0    +3 inv1    endpoint inverse depths
;   +4 c0      +5 c1      tile-column extent. draw_run reads only x0>>3 and
;              x1>>3, so sub-tile X is not part of the exact state at all.
;   +6 flags   bit0 left_real, bit1 right_real
;   +7 profile cached, so the marker needs no second table read
;
; 20 slots x 8 bytes = 160 bytes per stream, against 1,280 for the column form.
;
; MEMORY
;   0xC000 n_new  0xC001 n_old
;   0xC010 LO  0xC011 HI  0xC012 COL  0xC018 profile  0xC019 hmin  0xC01A hmax
;   0xC022 mark row count  0xC021 order-inversion flag
;   0xC030 iq lo/hi, 0xC032 step lo/hi  (the walk, per span)
;   0xC100 new stream (20 x 8)      0xC200 old stream (20 x 8)
;   0xCD00 dirty mask, 18 rows x 3 bytes
;   0xE000 COLBIT[20]  0xE014 COLBYTE[20]  0xE028 ROWBASE[18] as 16-bit
;   0xE100 k_col_recip_q8[21]

        call union_stream
        halt

union_stream:
        ld hl,0xCD00
        ld b,54
        xor a
us_clr:
        ld (hl),a
        inc hl
        djnz us_clr
        xor a
        ld (0xC021),a
        ld a,0xff
        ld (0xC015),a                ; last old slot seen
        ld a,(0xC000)
        or a
        jp z,us_pass2
        ld b,a
        ld hl,0xC100
us_span:
        push bc
        push hl
        call us_one
        pop hl
        ld a,l
        add a,8
        ld l,a
        jp nc,us_snc
        inc h
us_snc:
        pop bc
        djnz us_span
        jp us_pass2

; ---- one new span: find its retained twin, compare six bytes, dispatch ----
us_one:
        push hl
        ld a,(hl)                    ; keyid
        ld c,a
        ld a,(0xC001)
        or a
        jp z,us_new_span
        ld b,a
        ld hl,0xC200
        xor a
        ld (0xC016),a
us_find:
        ld a,(hl)
        cp c
        jp z,us_found
        ld a,l
        add a,8
        ld l,a
        jp nc,us_fnc
        inc h
us_fnc:
        ld a,(0xC016)
        inc a
        ld (0xC016),a
        djnz us_find
us_new_span:
        pop hl
        jp us_mark_whole             ; appeared: all of it is dirty

us_found:
        ; draw-order check, free: walking new spans in rank order the old slot
        ; indices must strictly increase
        ld a,(0xC015)
        cp 0xff
        jp z,us_ord_ok
        ld b,a
        ld a,(0xC016)
        cp b
        jp c,us_ord_bad
        jp z,us_ord_bad
us_ord_ok:
        ld a,(0xC016)
        ld (0xC015),a
        jp us_cmp
us_ord_bad:
        ld a,(0xC016)
        ld (0xC015),a
        ld a,1
        ld (0xC021),a
us_cmp:
        ; HL -> old record, stack top -> new record. Six bytes decide it:
        ; sid, inv0, inv1, c0, c1, flags. keyid is identity, profile is cached.
        ex de,hl                     ; DE -> old record
        pop hl                       ; HL -> new record
        push hl
        push de
        inc hl
        inc de
        ld b,6
us_cmp_l:
        ld a,(hl)
        ex de,hl
        cp (hl)
        jp nz,us_changed
        ex de,hl
        inc hl
        inc de
        djnz us_cmp_l
        pop de                       ; identical: this span is finished
        pop hl
        ret
us_changed:
        ex de,hl
        pop de                       ; DE -> old record
        pop hl                       ; HL -> new record
        ; mark the new extent, then the old one. Marking both is what covers
        ; uncovering: the columns the span has left are exactly its old extent
        ; minus its new one, and marking the whole of each is a superset.
        push de
        call us_walk_mark
        pop hl
        jp us_walk_mark

us_mark_whole:
        jp us_walk_mark

; ---- pass 2: retained spans that no longer exist ----
us_pass2:
        ld a,(0xC001)
        or a
        jp z,us_tail
        ld b,a
        ld hl,0xC200
us2_span:
        push bc
        push hl
        call us2_one
        pop hl
        ld a,l
        add a,8
        ld l,a
        jp nc,us2_snc
        inc h
us2_snc:
        pop bc
        djnz us2_span
        jp us_tail

us2_one:
        push hl
        ld a,(hl)
        ld c,a
        ld a,(0xC000)
        or a
        jp z,us2_gone
        ld b,a
        ld hl,0xC100
us2_find:
        ld a,(hl)
        cp c
        jp z,us2_alive
        ld a,l
        add a,8
        ld l,a
        jp nc,us2_fnc
        inc h
us2_fnc:
        djnz us2_find
us2_gone:
        pop hl
        jp us_walk_mark
us2_alive:
        pop hl
        ret

; ---- tail: a draw-order inversion means every span is suspect ----
us_tail:
        ld a,(0xC021)
        or a
        ret z
        ld a,(0xC000)
        or a
        jp z,ust_old
        ld b,a
        ld hl,0xC100
ust_n:
        push bc
        push hl
        call us_walk_mark
        pop hl
        ld a,l
        add a,8
        ld l,a
        jp nc,ust_n1
        inc h
ust_n1:
        pop bc
        djnz ust_n
ust_old:
        ld a,(0xC001)
        or a
        ret z
        ld b,a
        ld hl,0xC200
ust_o:
        push bc
        push hl
        call us_walk_mark
        pop hl
        ld a,l
        add a,8
        ld l,a
        jp nc,ust_o1
        inc h
ust_o1:
        pop bc
        djnz ust_o
        ret

; ---- walk one span's columns and mark them ----
; HL -> record. The per-column heights are not stored: they come from the same
; iq/step walk draw_run uses, which is one 16-bit add per column. That is the
; trade the span form makes - it recomputes two bytes per column instead of
; storing and comparing three.
us_walk_mark:
        push hl
        ld a,(hl)
        inc hl
        ld a,(hl)                    ; sid (unused here)
        inc hl
        ld a,(hl)                    ; inv0
        ld (0xC01B),a
        inc hl
        ld a,(hl)                    ; inv1
        ld (0xC01C),a
        inc hl
        ld a,(hl)                    ; c0
        ld (0xC012),a
        ld (0xC01D),a
        inc hl
        ld a,(hl)                    ; c1
        ld (0xC01E),a
        inc hl
        inc hl
        ld a,(hl)                    ; profile
        ld (0xC018),a
        pop hl

        ; n = c1 - c0 + 1 ; step = ((inv1-inv0) * recip[n]) >> 2, signed
        ld a,(0xC01E)
        ld b,a
        ld a,(0xC01D)
        ld c,a
        ld a,b
        sub c
        ret c                        ; c1 < c0: nothing
        inc a
        ld c,a
        ld b,0
        ld hl,0xE100                 ; k_col_recip_q8
        add hl,bc
        ld a,(hl)
        ld (0xC01F),a                ; recip
        ; iq = inv0 << 6
        ld a,(0xC01B)
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld (0xC030),hl
        ; d = inv1 - inv0. Both are uint8, so d spans -255..255 and does NOT
        ; fit a signed byte: computing it with an 8-bit `sub` turned -255 into
        ; +1 and mis-stepped the whole walk.
        ld a,(0xC01C)
        ld b,a
        ld a,(0xC01B)
        ld c,a
        ld a,b
        sub c
        ld l,a
        ld h,0
        jp nc,us_d_pos
        dec h                        ; sign-extend the borrow
us_d_pos:
        call us_mul16                ; HL = d * recip, signed
        ; >>2 arithmetic
        call us_sar2
        ld (0xC032),hl

        ; ---- the column walk ----
us_col:
        ld hl,(0xC030)
        call us_h_of_iq              ; A = left height for this column
        ld b,a                       ; B = running max
        ld c,a                       ; C = running min
        ld hl,(0xC030)
        ld de,(0xC032)
        add hl,de
        call us_h_of_iq              ; A = right height
        cp b
        jp c,us_c_lo
        ld b,a
us_c_lo:
        cp c
        jp nc,us_c_hi
        ld c,a
us_c_hi:
        ld a,c
        ld (0xC019),a                ; hmin, needed by the edge-range marker
        ld a,b
        ld (0xC01A),a                ; hmax
        call US_MARK_HOOK
        ; advance the walk
        ld hl,(0xC030)
        ld de,(0xC032)
        add hl,de
        ld (0xC030),hl
        ld a,(0xC012)
        ld b,a
        ld a,(0xC01E)
        cp b
        ret z
        ld a,b
        inc a
        ld (0xC012),a
        jp us_col

; HL = iq -> A = (clamp((iq+32)>>6, 255)) >> 1, the height byte
us_h_of_iq:
        ld de,32
        add hl,de
        bit 7,h
        jp z,us_hq_pos
        xor a                        ; negative: clamps to 0
        ret
us_hq_pos:
        ; >>6 on a 16-bit value: take H, shift left 2, fold in L's top 2 bits
        ld a,h
        cp 64
        jp c,us_hq_ok
        ld a,255                     ; >= 64<<6 saturates the u8 clamp
        srl a
        ret
us_hq_ok:
        ld a,l
        rlca
        rlca
        and 3
        ld e,a
        ld a,h
        add a,a
        add a,a
        or e
        srl a                        ; >>1 to reach the height byte
        ret

; HL = signed 16-bit multiplicand, (0xC01F) = unsigned multiplier -> HL
; Shift-add. The union runs this once per SPAN, not per column, so the cheap
; primitive is right here and A13's quarter-square table would not pay.
us_mul16:
        bit 7,h
        jp z,us_mul16u
        call us_neghl
        call us_mul16u
        jp us_neghl
us_mul16u:
        ld d,h
        ld e,l
        ld hl,0
        ld a,(0xC01F)
        ld b,8
us_mu_l:
        srl a
        jp nc,us_mu_skip
        push af
        add hl,de
        pop af
us_mu_skip:
        ex de,hl
        add hl,hl
        ex de,hl
        djnz us_mu_l
        ret

us_neghl:
        ld a,h
        cpl
        ld h,a
        ld a,l
        cpl
        ld l,a
        inc hl
        ret

; HL >>= 2, arithmetic
us_sar2:
        ld a,h
        bit 7,a
        jp nz,us_sar2_neg
        srl h
        rr l
        srl h
        rr l
        ret
us_sar2_neg:
        ; -((-v)>>2), matching shr_signed exactly
        call us_neghl
        srl h
        rr l
        srl h
        rr l
        jp us_neghl
