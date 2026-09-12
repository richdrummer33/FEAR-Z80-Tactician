; ============ TEMP_BOUNDARY_A: the exact temporal executor ============
;
; Runs AFTER the union has produced the dirty mask. Its job is to make every
; dirty cell exact while touching nothing else.
;
; WHY WHOLE DIRTY COLUMNS, AND WHY THAT IS STILL EXACT
; ----------------------------------------------------
; The union guarantees no cell OUTSIDE the dirty set changed, so clean columns
; already hold the right words. A dirty column is made exact by resetting it to
; background and replaying every span that covers it, far->near - which is
; precisely what the full renderer does for that column. So the executor is
; exact by construction, and it reuses the VERIFIED DDA_G materializer rather
; than a parallel tile-selection path.
;
; Measured shape of the problem (U=1): 6.97 dirty columns of 20 on mixed
; motion and 14.72 under rotation, holding 47.5% and 80.6% of all
; column-materializations respectively. Those two percentages are what the
; executor costs against a full render, before any row gating.
;
; DRIVING DDA_G OVER A SUB-RANGE
; ------------------------------
; The kernel takes iq, step, c0, c1, profile and the two border flags. To
; restrict a run to a contiguous dirty sub-range [a,b] inside [c0,c1]:
;   step stays as it is - it was derived from the ORIGINAL column count and
;        changing it would change the interpolation
;   iq   advances by step*(a-c0)
;   left_real  survives only if a == c0
;   right_real survives only if b == c1
; Getting any of those wrong changes the image, so the oracle checks the whole
; name table rather than the dirty cells alone.
;
; MEMORY
;   0xDC00 run count      0xDC10 run array, 20 x 10 bytes
;   0xDB00 dirty mask, 18 rows x 3 bytes, row-major as the union leaves it
;   0xDD00 dirty column bitmap, 3 bytes      0xDD03 current column
;   0xDD10.. the sub-range walk's state
;   0xE080 BASEWORD[18], 2 bytes each
;   0xC200 name table, 0xC000-0xC009 kernel inputs - DDA_G's own map, untouched

exec_entry:
        call exec_cols
        call exec_reset
        call exec_runs
        ret

; ---- collapse the row-major mask into a 20-bit dirty COLUMN bitmap ----
; The union marks row ranges within a column, so the mask is row-major; the
; executor wants columns. 18 ORs of three bytes is the whole conversion.
exec_cols:
        xor a
        ld (0xDD00),a
        ld (0xDD01),a
        ld (0xDD02),a
        ld hl,0xDB00
        ld b,18
ec_row:
        ld a,(hl)
        ld c,a
        ld a,(0xDD00)
        or c
        ld (0xDD00),a
        inc hl
        ld a,(hl)
        ld c,a
        ld a,(0xDD01)
        or c
        ld (0xDD01),a
        inc hl
        ld a,(hl)
        ld c,a
        ld a,(0xDD02)
        or c
        ld (0xDD02),a
        inc hl
        djnz ec_row
        ret

; ---- reset every dirty column to background ----
; Only dirty columns are touched. A clean column keeps last frame's words,
; which the union has already guaranteed are correct.
exec_reset:
        xor a
        ld (0xDD03),a                ; column
er_col:
        ld a,(0xDD03)
        call exec_is_dirty
        jp z,er_next
        ; Reset this column to background. base_word is three constants -
        ; ceiling for rows 0-8, horizon at row 9, floor for 10-17 - so no
        ; table and no second pointer are needed, which also avoids the
        ; (de) indirection the assembler does not provide.
        ld a,(0xDD03)
        ld l,a
        ld h,0
        add hl,hl                    ; 2*col
        ld de,0xC200
        add hl,de                    ; MAP + 2*col
        ld de,40                     ; one name-table row
        xor a
        ld b,9
er_ceil:
        ld (hl),a
        inc hl
        ld (hl),a
        dec hl
        add hl,de
        djnz er_ceil
        ld (hl),2                    ; TSP_TILE_HORIZON
        inc hl
        ld (hl),0
        dec hl
        add hl,de
        ld b,8
er_floor:
        ld (hl),1                    ; TSP_TILE_FLOOR
        inc hl
        ld (hl),0
        dec hl
        add hl,de
        djnz er_floor
er_next:
        ld a,(0xDD03)
        cp 19
        ret z
        inc a
        ld (0xDD03),a
        jp er_col

; A = column -> Z if clean, NZ if dirty
exec_is_dirty:
        push hl
        push bc
        ld c,a
        srl a
        srl a
        srl a
        ld l,a
        ld h,0
        ld de,0xDD00
        add hl,de
        ld a,c
        and 7
        inc a
        ld b,a
        ld a,1
eid_sh:
        dec b
        jp z,eid_have
        add a,a
        jp eid_sh
eid_have:
        ld c,a
        ld a,(hl)
        and c
        pop bc
        pop hl
        ret

; ---- replay every run over its dirty sub-ranges, far->near ----
exec_runs:
        ld a,(0xDC00)
        or a
        ret z
        ld b,a
        ld hl,0xDC10
ex_run:
        push bc
        push hl
        call exec_one_run
        pop hl
        ld bc,10
        add hl,bc
        pop bc
        djnz ex_run
        ret

; HL -> run record: iq(2) step(2) c0 c1 profile lr rr shade
exec_one_run:
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld (0xDD10),de               ; iq at c0
        inc hl
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld (0xDD12),de               ; step
        inc hl
        ld a,(hl)
        ld (0xDD14),a                ; c0
        ld (0xDD18),a                ; running iq column
        inc hl
        ld a,(hl)
        ld (0xDD15),a                ; c1
        inc hl
        ld a,(hl)
        ld (0xDD16),a                ; profile
        inc hl
        ld a,(hl)
        ld (0xDD17),a                ; left_real
        inc hl
        ld a,(hl)
        ld (0xDD19),a                ; right_real
        inc hl
        ld a,(hl)
        ld (0xDD1A),a                ; shade

        ; Walk c0..c1 finding maximal dirty sub-ranges. iq advances by step per
        ; column whether or not the column is dirty, so the sub-range's own
        ; start value is always correct.
        ld a,(0xDD14)
        ld (0xDD18),a
        ld de,(0xDD10)
        ld (0xDD1C),de               ; iq at the current column
eor_scan:
        ld a,(0xDD18)
        call exec_is_dirty
        jp z,eor_advance
        ; start of a dirty sub-range
        ld a,(0xDD18)
        ld (0xDD1E),a                ; a = sub-range start
        ld de,(0xDD1C)
        ld (0xDD20),de               ; iq at the sub-range start
eor_extend:
        ld a,(0xDD18)
        ld b,a
        ld a,(0xDD15)
        cp b
        jp z,eor_emit                ; hit c1
        ld a,b
        inc a
        ld (0xDD18),a
        ld de,(0xDD1C)
        ld hl,(0xDD12)
        add hl,de
        ld (0xDD1C),hl
        ld a,(0xDD18)
        call exec_is_dirty
        jp nz,eor_extend
        ; the column after the sub-range is clean: emit [start, cur-1]
        ld a,(0xDD18)
        dec a
        ld (0xDD1F),a
        call exec_emit
        jp eor_advance_nostep
eor_emit:
        ld a,(0xDD18)
        ld (0xDD1F),a
        call exec_emit
        ret                          ; c1 reached
eor_advance:
        ld a,(0xDD18)
        ld b,a
        ld a,(0xDD15)
        cp b
        ret z
        ld a,b
        inc a
        ld (0xDD18),a
        ld de,(0xDD1C)
        ld hl,(0xDD12)
        add hl,de
        ld (0xDD1C),hl
        jp eor_scan
eor_advance_nostep:
        ld a,(0xDD18)
        ld b,a
        ld a,(0xDD15)
        cp b
        ret z
        jp eor_scan

; Emit one sub-range [0xDD1E, 0xDD1F] with iq from 0xDD20 into the kernel's
; input block, then call the verified materializer.
exec_emit:
        ld de,(0xDD20)
        ld (0xC000),de
        ld de,(0xDD12)
        ld (0xC002),de
        ld a,(0xDD1E)
        ld (0xC005),a
        ld a,(0xDD1F)
        ld (0xC006),a
        ld a,(0xDD16)
        ld (0xC004),a
        ld a,(0xDD1A)
        ld (0xC009),a
        ; left_real survives only at the run's own c0
        ld a,(0xDD1E)
        ld b,a
        ld a,(0xDD14)
        cp b
        jp nz,ee_no_left
        ld a,(0xDD17)
        jp ee_set_left
ee_no_left:
        xor a
ee_set_left:
        ld (0xC007),a
        ; right_real survives only at the run's own c1
        ld a,(0xDD1F)
        ld b,a
        ld a,(0xDD15)
        cp b
        jp nz,ee_no_right
        ld a,(0xDD19)
        jp ee_set_right
ee_no_right:
        xor a
ee_set_right:
        ld (0xC008),a
        jp MATERIALIZE_ENTRY
