; ---- MASKED_F: classify from the HEIGHT BYTES, not from the row geometry ----
;
; MASKED_E's classifier cost 1,975 T per column - more than everything
; coverage could ever save. Almost all of it was re-deriving the column's row
; extent the expensive way: two signed 16-bit compares and two `rowfloor`
; calls, duplicating work `draw_edge` and `draw_full` already do.
;
; The extent is a function of the two height bytes and the profile, and each
; profile is monotonic in h, so the min/max endpoints are picked by ONE byte
; compare and then computed directly:
;
;   FULL    top 71-hmax   bottom 72+hmax
;   LINTEL  top 72-hmax   bottom 72-(hmin>>1)
;   RAISED  top 72-hmax   bottom 72+hmax-(hmax>>2)
;   RISER   top 72+hmin-(hmin>>2)   bottom 72+hmax
;
; Only the non-RISER top can go negative (h > 72), and it clamps to row 0
; there. Every other value is non-negative and up to 199, so it is shifted as
; UNSIGNED - testing bit 7 on those would read 168 as negative and blank the
; column, which is the same trap that bit RAISED in the per-column kernel.
cov_prep:
        ld a,(0xc028)                ; HLH
        ld b,a
        ld a,(0xc029)                ; HRH
        cp b
        jp nc,cp_hmax_r
        ld c,b                       ; hmax = HLH
        ld b,a                       ; hmin = HRH
        jp cp_h_done
cp_hmax_r:
        ld c,a                       ; hmax = HRH, hmin = HLH already in B
cp_h_done:
        ld a,(0xc004)                ; PROFILE
        cp 3
        jp z,cp_lo_riser
        ld a,72
        sub c                        ; 72 - hmax
        ld e,a
        ld a,(0xc004)
        or a
        jp nz,cp_lo_signed
        ld a,e
        dec a                        ; FULL: tl--
        ld e,a
cp_lo_signed:
        ld a,e
        bit 7,a
        jp z,cp_lo_shift
        xor a                        ; above the screen -> row 0
        jp cp_lo_store
cp_lo_riser:
        ld a,b
        srl a
        srl a
        ld e,a                       ; hmin>>2
        ld a,b
        sub e
        add a,72                     ; 72 + hmin - (hmin>>2), always >= 72
cp_lo_shift:
        srl a
        srl a
        srl a
        cp 18
        jp c,cp_lo_store
        xor a                        ; lo below the screen -> draws nothing,
        ld (0xc048),a                ; and must therefore own nothing
        ret
cp_lo_store:
        ld (0xc040),a                ; COVLO

        ld a,(0xc004)
        cp 1
        jp nz,cp_hi_not_lintel
        ld a,b
        srl a
        ld e,a
        ld a,72
        sub e                        ; 72 - (hmin>>1)
        jp cp_hi_shift
cp_hi_not_lintel:
        cp 2
        jp nz,cp_hi_plain
        ld a,c
        srl a
        srl a
        ld e,a
        ld a,c
        sub e
        add a,72                     ; 72 + hmax - (hmax>>2)
        jp cp_hi_shift
cp_hi_plain:
        ld a,c
        add a,72                     ; 72 + hmax
cp_hi_shift:
        srl a
        srl a
        srl a
        cp 18
        jp c,cp_hi_store
        ld a,17
cp_hi_store:
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
