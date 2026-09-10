
; ================= coverage (MASKED_E) =================
; Per-column ownership as a 3-byte bitmask (row r -> byte r>>3, bit r&7),
; the same shape the shipped GG assembly already uses for g_polar_nt_cov.
;
; The column is classified ONCE, then drawn in one of three ways:
;   status 0  fully occluded - skip the column outright (geometry included)
;   status 1  nothing owned  - draw unmasked, zero per-row cost
;   status 2  partial        - stash the owned words, draw, put them back
;
; The stash/restore is what keeps intra-column semantics intact. Within a
; column the top edge, bottom edge and interior may share a row (h=0 puts
; both edges on row 9, and LINTEL/RISER move a whole edge across the
; horizon), and there the LAST writer must win. Gating each store and
; marking ownership as we go would let the first of those writes claim the
; row and reject the second, which is a different image. Drawing freely and
; then restoring only the rows an earlier, nearer run already owns keeps
; both rules: last-writer-wins inside the column, first-writer-wins across
; runs.
cov_prep:
        ld hl,(0xc010)               ; TL
        ld de,(0xc012)               ; TR
        call cmps
        jp c,cp_lo
        ld hl,(0xc012)
cp_lo:
        call rowfloor
        bit 7,a
        jp z,cp_lo_nn
        xor a
cp_lo_nn:
        cp 18
        jp c,cp_lo_ok
; lo below the screen means the column draws NOTHING - and it must then claim
; NOTHING either. Clamping lo up to 17 instead made an empty column mark row
; 17 as owned, which suppressed the next (farther) run's real write there.
        xor a
        ld (0xc048),a
        ret
cp_lo_ok:
        ld (0xc040),a                ; COVLO
        ld hl,(0xc014)               ; BL
        ld de,(0xc016)               ; BR
        call cmps
        jp nc,cp_hi
        ld hl,(0xc016)
cp_hi:
        call rowfloor
        bit 7,a
        jp z,cp_hi_nn
        xor a
        ld (0xc048),a                ; hi < 0 -> nothing drawn, nothing owned
        ret
cp_hi_nn:
        cp 18
        jp c,cp_hi_ok
        ld a,17
cp_hi_ok:
        ld (0xc041),a                ; COVHI
        ld b,a
        ld a,(0xc040)
        cp b
        jp c,cp_rows_ok
        jp z,cp_rows_ok
        xor a                        ; hi < lo -> nothing drawn, nothing owned
        ld (0xc048),a
        ret
cp_rows_ok:

; RANGE = LOWTAB[lo] & HIGHTAB[hi]
        ld a,(0xc040)
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        ld de,0xc800
        add hl,de
        ld a,(hl)
        ld (0xc042),a
        inc hl
        ld a,(hl)
        ld (0xc043),a
        inc hl
        ld a,(hl)
        ld (0xc044),a
        ld a,(0xc041)
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        ld de,0xc880
        add hl,de
        ld a,(hl)
        ld b,a
        ld a,(0xc042)
        and b
        ld (0xc042),a
        inc hl
        ld a,(hl)
        ld b,a
        ld a,(0xc043)
        and b
        ld (0xc043),a
        inc hl
        ld a,(hl)
        ld b,a
        ld a,(0xc044)
        and b
        ld (0xc044),a

; COVPTR = COV + c*3
        ld a,(0xc030)                ; CURC
        ld l,a
        ld h,0
        ld d,h
        ld e,l
        add hl,hl
        add hl,de
        ld de,0xc600
        add hl,de
        ld (0xc04a),hl

; B = OR of unowned bits, C = OR of overlapping bits, OVL[i] stored
        ld bc,0
        ld a,(hl)
        ld e,a
        ld a,(0xc042)
        and e
        ld (0xc045),a
        or c
        ld c,a
        ld a,e
        cpl
        ld e,a
        ld a,(0xc042)
        and e
        or b
        ld b,a
        inc hl
        ld a,(hl)
        ld e,a
        ld a,(0xc043)
        and e
        ld (0xc046),a
        or c
        ld c,a
        ld a,e
        cpl
        ld e,a
        ld a,(0xc043)
        and e
        or b
        ld b,a
        inc hl
        ld a,(hl)
        ld e,a
        ld a,(0xc044)
        and e
        ld (0xc047),a
        or c
        ld c,a
        ld a,e
        cpl
        ld e,a
        ld a,(0xc044)
        and e
        or b
        ld b,a

        xor a
        ld (0xc048),a                ; COVST = 0 (skip)
        ld a,b
        or a
        ret z                        ; nothing unowned -> occluded
        ld a,1
        ld (0xc048),a                ; COVST = 1 (free)
        ld a,c
        or a
        ret z
        ld a,2
        ld (0xc048),a                ; COVST = 2 (partial)
        ret

; OVL bitmask -> FLAGS[0..23], one byte per row
cov_flags:
        ld hl,0xc740
        ld a,(0xc045)
        ld c,a
        call cf_byte
        ld a,(0xc046)
        ld c,a
        call cf_byte
        ld a,(0xc047)
        ld c,a
cf_byte:
        ld b,8
cf_bit:
        ld a,c
        and 1
        ld (hl),a
        inc hl
        srl c
        dec b
        jp nz,cf_bit
        ret

; save the map words for rows an earlier (nearer) run already owns
cov_stash:
        call cov_flags
        xor a
        ld (0xc01f),a                ; R0 = 0
cs_loop:
        ld a,(0xc01f)
        ld l,a
        ld h,0
        ld de,0xc740
        add hl,de
        ld a,(hl)
        or a
        jp z,cs_next
        call row_addr
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld a,(0xc01f)
        ld l,a
        ld h,0
        add hl,hl
        ld bc,0xc700
        add hl,bc
        ld (hl),e
        inc hl
        ld (hl),d
cs_next:
        ld a,(0xc01f)
        cp 17
        ret z
        inc a
        ld (0xc01f),a
        jp cs_loop

; put them back, undoing this run's writes on rows it does not own
cov_restore:
        xor a
        ld (0xc01f),a
cr_loop:
        ld a,(0xc01f)
        ld l,a
        ld h,0
        ld de,0xc740
        add hl,de
        ld a,(hl)
        or a
        jp z,cr_next
        call row_addr                ; FIRST: row_addr clobbers DE, so the
        push hl                      ; stashed word cannot be live across it
        ld a,(0xc01f)
        ld l,a
        ld h,0
        add hl,hl
        ld bc,0xc700
        add hl,bc
        ld e,(hl)
        inc hl
        ld d,(hl)
        pop hl
        ld (hl),e
        inc hl
        ld (hl),d
cr_next:
        ld a,(0xc01f)
        cp 17
        ret z
        inc a
        ld (0xc01f),a
        jp cr_loop

; owned |= range
cov_mark:
        ld hl,(0xc04a)
        ld a,(0xc042)
        or (hl)
        ld (hl),a
        inc hl
        ld a,(0xc043)
        or (hl)
        ld (hl),a
        inc hl
        ld a,(0xc044)
        or (hl)
        ld (hl),a
        ret
