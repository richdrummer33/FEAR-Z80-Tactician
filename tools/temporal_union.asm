; ============ UNION_A: the cross-span dirty union, in Z80 ============
;
; A31 proved this operation is mandatory: a purely local per-span skip is wrong
; on 76.7% of rotating pose pairs, because an unchanged span can have its cells
; uncovered by a nearer span that moved. A26 proved that a classifier costing
; more than it saves kills the idea outright. So this is the first go/no-go and
; it gets measured on its own, not inside a bigger kernel where its cost would
; be hidden.
;
; WHY THIS IS NOT A26's CLASSIFIER
; --------------------------------
; A26 spent 1,975 T per column almost entirely on working out which rows a
; column covers - two signed 16-bit compares and two `rowfloor` calls. Here the
; row extent falls out of two retained height bytes, because the retained state
; IS (hl, hr, border) per column. DDA_G already holds those two bytes as
; HLH/HRH.
;
; WHY THE HEIGHT BYTES AND NOT (il, ir)
; -------------------------------------
; Every endpoint derives from hl = il>>1, so il and il+1 across an even
; boundary give identical geometry. Retaining il reports a change where there
; is none: measured at 3,568,759 spurious columns in 73,521,834, 4.9%, each one
; a column the union would dirty for nothing.
;
; THE ROW RANGE IS DELIBERATELY CONSERVATIVE
; ------------------------------------------
; [71-hmax, 72+hmax] contains the drawn rows for EVERY profile: LINTEL's bottom
; is 72-(h>>1) and RISER's top is 72+h-(h>>2), both inside it. So one formula
; serves all four profiles with no branch. The union is allowed to be a
; superset - it may not MISS a cell - so this is correct by construction, and
; the inflation it costs is measured against the host's exact mask rather than
; argued about.
;
; MEMORY
;   0xC000 n_new          0xC001 n_old
;   0xC010 LO  0xC011 HI  0xC012 COL  0xC013 BIT
;   0xC100 new span records, 20 slots x 64 bytes  (0xC100-0xC5FF)
;   0xC800 old span records, same                 (0xC800-0xCCFF)
;   0xCD00 dirty mask, 18 rows x 3 bytes, bit c of row r = cell (r,c)
;
; 20 slots, not 8. TSPF_MAX_ACTIVE is 20 and the corpus really does reach 12
; spans in one pose; a 8-slot bench silently dropped the rest and showed 852
; missed cells.
;
; Span record (64 bytes): +0 keyid  +1 c0  +2 c1  +3 profile
;                         +4+3c  hl, hr, border   for c in 0..19
;
; Slot index IS draw rank, far->near, because that is the order the host dumps
; them in and the order draw_run uses.
;
; THREE PASSES, and the second and third are not optional. The first version of
; this kernel had only the first and missed 22,099 dirty cells:
;
;   1. every NEW span, against its retained twin
;   2. every OLD span that has VANISHED - its cells must be restored, and
;      nothing in pass 1 ever visits them
;   3. draw-ORDER changes - if two retained spans swapped rank, a cell they
;      both cover can change winner with neither span's own state moving.
;      Detected in pass 1 for free: walk new spans in rank order, look each one
;      up in the old set, and the old slot indices must strictly increase.

; Entry stub: the interpreter stops on `halt`, so the measured region is
; exactly one call to union_build and nothing else.
        call union_build
        halt

union_build:
        ld hl,0xCD00                 ; clear the 54 mask bytes
        ld b,54
        xor a
ub_clr:
        ld (hl),a
        inc hl
        djnz ub_clr

        xor a
        ld (0xC017),a                ; "also mark the new span" flag
        ld (0xC021),a                ; draw-order inversion seen
        ld a,0xff
        ld (0xC015),a                ; last old slot seen: none yet
        ld a,(0xC000)
        or a
        jp z,ub_pass2
        ld b,a
        xor a
        ld (0xC024),a                ; new slot index, for the span summary
        ld hl,0xC100
; The per-span body is a CALL, not inline: djnz is a relative jump and the
; body outgrew its +/-128 range once passes 2 and 3 were added.
ub_span:
        push bc
        push hl
        call ub_one_span
        pop hl
        ld a,l
        add a,64
        ld l,a
        jp nc,ub_snc
        inc h
ub_snc:
        ld a,(0xC024)
        inc a
        ld (0xC024),a
        pop bc
        djnz ub_span
        jp ub_pass2

; ---- one new span: find its retained twin, check order, dispatch ----
; ---- UNION_E's span-level early-out ----
; The span-stream experiment showed a 6-byte record compare proves EVERY column
; of that span matches, and that 39.6% of spans are unchanged at U=1, holding
; 10.02 of the 23.90 column-materializations. UNION_S tried to replace the
; retained columns with the record and came out SLOWER, because it then had to
; recompute each column's heights from the iq/step walk. The hybrid keeps both:
; the record decides whether to look at the columns at all.
;
; Summaries live at 0xE200 + slot*6 (new) and 0xE280 + slot*6 (old):
;   sid, inv0, inv1, c0, c1, flags
us_precheck_nop:
        ld a,1
        or a                         ; always NZ: do the column work
        ret

us_precheck:
        ; C = new slot index, (0xC016) = old slot index. Returns Z if the two
        ; span records are byte-identical.
        push hl
        push de
        ld a,(0xC024)
        ld c,a
        ld b,0
        ld hl,0xE200
        add hl,bc
        add hl,bc
        add hl,bc
        add hl,bc
        add hl,bc
        add hl,bc
        ld a,(0xC016)
        ld c,a
        ld b,0
        ld de,0xE280
        ex de,hl
        add hl,bc
        add hl,bc
        add hl,bc
        add hl,bc
        add hl,bc
        add hl,bc
        ex de,hl                     ; HL -> new summary, DE -> old summary
        ld b,6
usp_l:
        ld a,(hl)
        ex de,hl
        cp (hl)
        jp nz,usp_diff
        ex de,hl
        inc hl
        inc de
        djnz usp_l
        pop de
        pop hl
        xor a                        ; Z: identical
        ret
usp_diff:
        pop de
        pop hl
        ld a,1
        or a                         ; NZ: differs
        ret

ub_one_span:
        push hl
        ld a,(hl)                    ; keyid
        ld c,a
        ld a,(0xC001)
        or a
        jp z,ub_no_old
        ld b,a
        ld hl,0xC800
        xor a
        ld (0xC016),a                ; slot index being probed
ub_find:
        ld a,(hl)
        cp c
        jp z,ub_found_slot
        ld a,l
        add a,64
        ld l,a
        jp nc,ub_fnc
        inc h
ub_fnc:
        ld a,(0xC016)
        inc a
        ld (0xC016),a
        djnz ub_find
ub_no_old:
        ld a,0xff
        ld (0xC016),a
        call ub_note_slot
        ld hl,0x0000                 ; 0 = span is new this update
        jp ub_dispatch

; oldslot[new slot] at 0xE300, 0xff when the span is new this update.
ub_note_slot:
        push hl
        push bc
        ld a,(0xC024)
        ld c,a
        ld b,0
        ld hl,0xE300
        add hl,bc
        ld a,(0xC016)
        ld (hl),a
        pop bc
        pop hl
        ret

ub_found_slot:
        ; Pass 3, for free: walking new spans in rank order, the old slot
        ; indices must strictly increase. If they do not, two retained spans
        ; swapped draw order and a cell they share can change winner without
        ; either span's own state moving - exactly A31's failure mode.
        ld a,(0xC015)
        cp 0xff
        jp z,ub_order_ok
        ld b,a
        ld a,(0xC016)
        cp b
        jp c,ub_order_broken
        jp z,ub_order_broken
ub_order_ok:
        ld a,(0xC016)
        ld (0xC015),a
        call ub_note_slot
        jp ub_dispatch
ub_order_broken:
        ; Marking only the span AT the inversion is not enough: every span it
        ; jumped over also swapped with it, and a cell any such pair shares can
        ; change winner. Detected here, handled once at the end - a rare event
        ; (A30: 0.32 draw-order flips per update) paid for conservatively
        ; rather than with a per-pair search.
        ld a,(0xC016)
        ld (0xC015),a
        call ub_note_slot
        ld a,1
        ld (0xC021),a

ub_dispatch:
        call US_PRECHECK_HOOK
        jp nz,ub_dispatch2
        pop hl                       ; identical span: no column work at all
        ret
ub_dispatch2:
        ex de,hl                     ; DE = old record or 0000
        pop hl                       ; HL = new record
        ld a,(0xC017)
        or a
        jp z,ub_normal
        xor a
        ld (0xC017),a
        jp ub_mark_all               ; order broke: mark the new span whole
ub_normal:
        jp ub_pair

; ---- pass 2: retained spans that no longer exist ----
; Nothing in pass 1 visits these, and every cell they owned has to be restored.
; Leaving this out cost 22,099 missed dirty cells.
ub_pass2:
        ld a,(0xC001)
        or a
        jp z,ub_tail
        ld b,a
        ld hl,0xC800
up2_span:
        push bc
        push hl
        call up2_one
        pop hl
        ld a,l
        add a,64
        ld l,a
        jp nc,up2_snc
        inc h
up2_snc:
        pop bc
        djnz up2_span
        jp ub_tail

up2_one:
        push hl
        ld a,(hl)
        ld c,a                       ; keyid
        ld a,(0xC000)
        or a
        jp z,up2_gone
        ld b,a
        ld hl,0xC100
up2_find:
        ld a,(hl)
        cp c
        jp z,up2_alive
        ld a,l
        add a,64
        ld l,a
        jp nc,up2_fnc
        inc h
up2_fnc:
        djnz up2_find
up2_gone:
        pop hl
        jp ub_mark_all
up2_alive:
        pop hl
        ret

; ---- tail: settle draw-order inversions, PAIRWISE ----
;
; The blanket version marked every span in both streams whole. Measured, that
; is the p95: inversions fire on 11.2% of rotating updates and those updates
; cost 202,883 T against 73,189 T without, which is the entire tail that made
; the union's p95 exceed a full render.
;
; Only cells that BOTH flipped spans cover can change winner, so only the
; intersection of their column ranges is dirty. Inversions are rare, so an
; O(n^2) pass over the few spans involved is affordable where marking
; everything is not.
ub_tail:
        ld a,(0xC021)
        or a
        ret z
        ld a,(0xC000)
        cp 2
        ret c                        ; fewer than two spans: nothing can flip
        xor a
        ld (0xC025),a                ; i
ubt_i:
        ld a,(0xC025)
        inc a
        ld (0xC026),a                ; j = i+1
ubt_j:
        ld a,(0xC026)
        ld b,a
        ld a,(0xC000)
        cp b
        jp z,ubt_i_next
        jp c,ubt_i_next
        call ubt_pair
        ld a,(0xC026)
        inc a
        ld (0xC026),a
        jp ubt_j
ubt_i_next:
        ld a,(0xC025)
        inc a
        ld (0xC025),a
        ld b,a
        ld a,(0xC000)
        cp b
        ret z
        ret c
        jp ubt_i

; Did spans i and j swap? i is earlier in new order by construction, so a flip
; is oldslot[i] > oldslot[j], with neither being 0xff.
ubt_pair:
        ld a,(0xC025)
        ld c,a
        ld b,0
        ld hl,0xE300
        add hl,bc
        ld a,(hl)
        cp 0xff
        ret z
        ld (0xC027),a
        ld a,(0xC026)
        ld c,a
        ld b,0
        ld hl,0xE300
        add hl,bc
        ld a,(hl)
        cp 0xff
        ret z
        ld b,a
        ld a,(0xC027)
        cp b
        ret c                        ; oldslot[i] < oldslot[j]: order held
        ret z
        ; flipped: mark the column intersection, in both streams
        ld a,(0xC025)
        call ubt_rec_new
        push hl
        ld a,(0xC026)
        call ubt_rec_new
        pop de
        call ubt_overlap             ; DE = span i, HL = span j
        ld a,(0xC027)
        call ubt_rec_old
        push hl
        ld a,(0xC026)
        ld c,a
        ld b,0
        ld hl,0xE300
        add hl,bc
        ld a,(hl)
        call ubt_rec_old
        pop de
        jp ubt_overlap

; A = slot -> HL = record address
ubt_rec_new:
        ld c,a
        ld b,0
        ld hl,0xC100
        jp ubt_rec_add
ubt_rec_old:
        ld c,a
        ld b,0
        ld hl,0xC800
ubt_rec_add:
        ; HL = base, BC = slot -> HL += slot*64, by shifting rather than by
        ; sixty-four adds
        push hl
        ld h,b
        ld l,c
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld b,h
        ld c,l
        pop hl
        add hl,bc
        ret

; DE and HL are two records: mark every column both occupy, from each one's
; own heights.
ubt_overlap:
        push de
        push hl
        ; lo = max(c0 of both), hi = min(c1 of both)
        inc hl
        ld a,(hl)
        ld b,a
        inc hl
        ld a,(hl)
        ld c,a
        ex de,hl
        inc hl
        ld a,(hl)
        cp b
        jp c,ubt_lo_ok
        ld b,a
ubt_lo_ok:
        inc hl
        ld a,(hl)
        cp c
        jp nc,ubt_hi_ok
        ld c,a
ubt_hi_ok:
        ld a,b
        cp c
        jp z,ubt_go
        jp nc,ubt_none
ubt_go:
        ld a,b
        ld (0xC012),a
        ld a,c
        ld (0xC01E),a
        pop hl
        pop de
        push de
        call ubt_mark_range
        pop hl
        jp ubt_mark_range
ubt_none:
        pop hl
        pop de
        ret

; HL = record, columns (0xC012)..(0xC01E) -> mark each from its own heights
ubt_mark_range:
        push hl
        inc hl
        inc hl
        inc hl
        ld a,(hl)
        ld (0xC018),a                ; profile
        pop hl
ubt_mr_col:
        push hl
        call ub_off
        ld a,(hl)
        ld b,a
        ld c,a
        inc hl
        ld a,(hl)
        call ub_o2acc
        ld a,c
        ld (0xC019),a
        ld a,b
        ld (0xC01A),a
        ld a,b
        call mark_span_a
        pop hl
        ld a,(0xC012)
        ld b,a
        ld a,(0xC01E)
        cp b
        ret z
        ld a,b
        inc a
        ld (0xC012),a
        jp ubt_mr_col

; ---- mark every column of one span, conservatively, from its own heights ----
ub_mark_all:
        push hl
        push hl
        inc hl
        inc hl
        inc hl
        ld a,(hl)
        ld (0xC018),a                ; profile
        pop hl
        inc hl
        ld a,(hl)
        ld b,a                       ; c0
        inc hl
        ld a,(hl)
        ld c,a                       ; c1
        pop hl
        ld a,b
        ld (0xC012),a
uma_col:
        push bc
        push hl
        call ub_off
        ld a,(hl)
        ld b,a
        ld c,a
        inc hl
        ld a,(hl)
        call ub_o2acc
        ld a,c
        ld (0xC019),a
        ld a,b
        ld (0xC01A),a
        ld a,b
        call mark_span_a
        pop hl
        pop bc
        ld a,(0xC012)
        cp c
        ret z
        inc a
        ld (0xC012),a
        jp uma_col

; ---- one span pair. HL = new record, DE = old record (0 if none) ----
ub_pair:
        push hl
        push de
        inc hl
        ld a,(hl)                    ; c0 new
        ld b,a
        inc hl
        ld a,(hl)                    ; c1 new
        ld c,a
        ld a,d
        or e
        jp z,ub_bounds_done
        ex de,hl
        inc hl
        ld a,(hl)                    ; c0 old
        cp b
        jp nc,ub_c0_ok
        ld b,a
ub_c0_ok:
        inc hl
        ld a,(hl)                    ; c1 old
        cp c
        jp c,ub_c1_ok
        ld c,a
ub_c1_ok:
        ex de,hl
ub_bounds_done:
        pop de
        pop hl
        ld a,d
        or e
        jp nz,ub_have_old
        jp ub_mark_all               ; span appeared: all of it is dirty
ub_have_old:
        push hl
        inc hl
        inc hl
        inc hl
        ld a,(hl)
        ld (0xC018),a                ; profile, constant for the whole span
        pop hl
        ld a,b
        ld (0xC012),a
ub_col:
        push bc
        push hl
        push de
        call ub_one_col
        pop de
        pop hl
        pop bc
        ld a,(0xC012)
        cp c
        ret z
        inc a
        ld (0xC012),a
        jp ub_col

; ================= UNION_D: no presence test in the inner loop =================
;
; UNION_C's table-driven marking returned only 3.8%, against a profile that
; attributed 18% to the mark family. The profile share did not translate
; because there are only ~40 marks per update, so the per-call saving is small
; - the same disappointment A24's ROWPTR_B recorded, for the same reason.
;
; The real cost is the per-column machinery: UNION_B sweeps the UNION of both
; column ranges for every span and asks "is this column present in new? in
; old?" with four push/pop pairs and two calls, every column. Re-profiling
; put ub_o_done + ub_n_done + ub_one_col + ub_col at ~18% and ub_off at 6.4%,
; none of which is work.
;
; So split the sweep by what is actually true of each column instead of
; testing it:
;   1. the OVERLAP of the two ranges - both present, 3-byte compare, and the
;      two column pointers just walk by +3
;   2. columns in NEW only  - entered, whole column
;   3. columns in OLD only  - left, whole column
; The inner loop then contains no presence test at all.
;
; scratch: 0xC023 c0n  0xC024 c1n  0xC025 c0o  0xC026 c1o
ub_pair_d:
        push hl
        inc hl
        ld a,(hl)
        ld (0xC023),a
        inc hl
        ld a,(hl)
        ld (0xC024),a
        inc hl
        ld a,(hl)
        ld (0xC018),a                ; profile, constant for the span
        pop hl
        ld a,d
        or e
        jp nz,upd_have_old
        jp ub_mark_all               ; span appeared: all of it
upd_have_old:
        push hl
        ex de,hl
        inc hl
        ld a,(hl)
        ld (0xC025),a
        inc hl
        ld a,(hl)
        ld (0xC026),a
        ex de,hl
        pop hl

        ; ---- 1. overlap ----
        ld a,(0xC023)
        ld b,a
        ld a,(0xC025)
        cp b
        jp nc,upd_lo_ok
        ld a,b
upd_lo_ok:
        ld (0xC012),a                ; lo = max(c0n, c0o)
        ld a,(0xC024)
        ld b,a
        ld a,(0xC026)
        cp b
        jp c,upd_hi_ok
        ld a,b
upd_hi_ok:
        ld (0xC027),a                ; hi = min(c1n, c1o)
        ld a,(0xC012)
        ld b,a
        ld a,(0xC027)
        cp b
        jp c,upd_side                ; no overlap at all
upd_ov:
        push hl
        push de
        call ub_one_col_d
        pop de
        pop hl
        ld a,(0xC012)
        ld b,a
        ld a,(0xC027)
        cp b
        jp z,upd_side
        ld a,b
        inc a
        ld (0xC012),a
        jp upd_ov

        ; ---- 2 and 3. the non-overlapping ends ----
        ; These are short - a span slides zero or one column per update at
        ; realistic cadence (A30: 97.3% at U=1) - so they keep the cheap
        ; in-range test rather than being split into sub-intervals.
upd_side:
        ld a,(0xC023)
        ld (0xC012),a
upd_new_only:
        ; "outside the old range" is col < c0o OR col > c1o. The first version
        ; tested c0o >= col, which is true at col == c0o - a column that IS in
        ; the old range - and marked it anyway. 68 missed cells, because the
        ; column then never got its overlap treatment either.
        ld a,(0xC012)
        ld b,a
        ld a,(0xC025)
        ld c,a
        ld a,b
        cp c
        jp c,upd_no_mark             ; col < c0o
        ld a,(0xC026)
        ld c,a
        ld a,b
        cp c
        jp z,upd_no_skip             ; col == c1o
        jp c,upd_no_skip             ; col < c1o
upd_no_mark:
        push hl
        push de
        ld a,b
        ld (0xC012),a
        call upd_whole_new
        pop de
        pop hl
upd_no_skip:
        ld a,(0xC012)
        ld b,a
        ld a,(0xC024)
        cp b
        jp z,upd_old_side
        ld a,b
        inc a
        ld (0xC012),a
        jp upd_new_only

upd_old_side:
        ld a,(0xC025)
        ld (0xC012),a
upd_old_only:
        ld a,(0xC012)
        ld b,a
        ld a,(0xC023)
        ld c,a
        ld a,b
        cp c
        jp c,upd_oo_mark             ; col < c0n
        ld a,(0xC024)
        ld c,a
        ld a,b
        cp c
        jp z,upd_oo_skip             ; col == c1n
        jp c,upd_oo_skip             ; col < c1n
upd_oo_mark:
        push hl
        push de
        ex de,hl
        call upd_whole_new
        pop de
        pop hl
upd_oo_skip:
        ld a,(0xC012)
        ld b,a
        ld a,(0xC026)
        cp b
        ret z
        ld a,b
        inc a
        ld (0xC012),a
        jp upd_old_only

; whole column from the record in HL, using its own two heights
upd_whole_new:
        call ub_off
        ld a,(hl)
        ld b,a
        ld c,a
        inc hl
        ld a,(hl)
        call ub_o2acc
        ld a,c
        ld (0xC019),a
        ld a,b
        ld (0xC01A),a
        ld a,b
        jp mark_span_a

; one OVERLAP column: both sides present by construction, so no presence test
ub_one_col_d:
        call ub_off                  ; HL -> new col bytes
        ex de,hl
        call ub_off                  ; HL -> old col bytes, DE -> new col
        push hl
        push de
        ld a,(hl)
        ld b,a
        inc hl
        ld a,(hl)
        ld c,a
        inc hl
        ld a,(hl)
        ld (0xC014),a
        ex de,hl
        inc hl
        inc hl
        ld a,(0xC014)
        cp (hl)
        jp nz,upd_bdiff
        dec hl
        dec hl
        ld a,(hl)
        cp b
        jp nz,upd_geom
        inc hl
        ld a,(hl)
        cp c
        jp nz,upd_geom
        pop de
        pop hl
        ret
upd_bdiff:
        pop de
        pop hl
        call ub_off2
        jp mark_span_a
upd_geom:
        pop de
        pop hl
        call ub_off2
        jp mark_span_b

; ---- one column of one span pair. HL = new record, DE = old record ----
; Presence comes from c0/c1, not a sentinel: hl=hr=border=0 is a legal present
; state and would be indistinguishable from absent.
ub_one_col:
        push hl
        push de
        ld a,(0xC012)
        ld c,a
        inc hl
        ld a,(hl)
        cp c
        jp z,ub_n_lo_ok
        jp nc,ub_new_absent
ub_n_lo_ok:
        inc hl
        ld a,c
        cp (hl)
        jp z,ub_n_hi_ok
        jp nc,ub_new_absent
ub_n_hi_ok:
        ld b,1
        jp ub_n_done
ub_new_absent:
        ld b,0
ub_n_done:
        pop de
        push de
        ld a,d
        or e
        jp z,ub_old_absent
        ex de,hl
        inc hl
        ld a,(hl)
        cp c
        jp z,ub_o_lo_ok
        jp nc,ub_old_absent
ub_o_lo_ok:
        inc hl
        ld a,c
        cp (hl)
        jp z,ub_o_hi_ok
        jp nc,ub_old_absent
ub_o_hi_ok:
        ld c,1
        jp ub_o_done
ub_old_absent:
        ld c,0
ub_o_done:
        pop de
        pop hl
        ld a,b
        or c
        ret z                        ; neither side is here
        ld a,b
        and c
        jp z,ub_one_side

        ; --- both present: the 3-byte retained compare ---
        ; The BORDER byte is compared FIRST, deliberately. Comparing heights
        ; first and only noticing the border when it happened to be the first
        ; differing byte cost 151 missed cells: a column whose height AND
        ; border both moved took the edges-only path, but a border bit lives on
        ; the FULL interior tile, so the whole column is dirty.
        push hl
        push de
        call ub_off                  ; HL -> new col bytes
        ex de,hl                     ; DE -> new col, HL -> old record base
        call ub_off                  ; HL -> old col bytes
        push hl
        push de
        ld a,(hl)
        ld b,a                       ; old hl
        inc hl
        ld a,(hl)
        ld c,a                       ; old hr
        inc hl
        ld a,(hl)
        ld (0xC014),a                ; old border
        ex de,hl                     ; HL -> new col base
        inc hl
        inc hl                       ; HL -> new border
        ld a,(0xC014)
        cp (hl)
        jp nz,ubc_bdiff
        xor a
        ld (0xC01E),a
        jp ubc_h
ubc_bdiff:
        ld a,1
        ld (0xC01E),a
        jp ubc_changed
ubc_h:
        dec hl
        dec hl                       ; HL -> new col base
        ld a,(hl)
        cp b
        jp nz,ubc_changed
        inc hl
        ld a,(hl)
        cp c
        jp nz,ubc_changed
        pop de                       ; unchanged: three compares and out
        pop hl
        pop de
        pop hl
        ret
ubc_changed:
        pop de                       ; DE -> new col bytes
        pop hl                       ; HL -> old col bytes
        call ub_off2                 ; hmin/hmax over all four height bytes
        pop de
        pop hl
        ld a,(0xC01E)
        or a
        jp nz,mark_span_a            ; border moved: whole column
        jp ub_mark

ub_one_side:
        ; present on exactly one side: that side's whole column is dirty,
        ; interior included, so this never takes the edges-only path
        push hl
        push de
        ld a,b
        or a
        jp nz,ub_os_new
        ex de,hl
ub_os_new:
        call ub_off
        ld a,(hl)
        ld b,a
        ld c,a
        inc hl
        ld a,(hl)
        call ub_o2acc
        ld a,c
        ld (0xC019),a
        ld a,b
        ld (0xC01A),a
        ld a,b
        pop de
        pop hl
        jp mark_span_a               ; column entered or left: WHOLE column
ub_mark:
        jp UB_MARK_HOOK

; ---- UNION_A's marking: one conservative profile-free range per column ----
; [71-hmax, 72+hmax] contains the drawn rows for every profile, so no branch is
; needed. It is correct - a superset never misses a cell - and it is measured
; here precisely so the cost of that shortcut is a number rather than an
; assumption.
mark_span_a:
        ld a,(0xC01A)
        call row_range
        jp mark_col

; ---- UNION_B's marking: the two EDGE ranges only ----
; The interior stays resident, which is the entire A29 mechanism and the thing
; UNION_A threw away. The interior's own boundary rows need no separate mark:
; the interior start is row_floor(max top)+1, so it moves exactly with the top
; edge and is always inside the top edge's own old/new union. The host measured
; that empirically - interior enter/leave attributed 0.00 cells over 3.5M pose
; pairs because the edge unions had already claimed them.
;
; Profile decides which end of [hmin,hmax] is which, and the signedness trap
; A27 hit twice decides how each is shifted:
;   FULL    top 71-h        (can go negative)  bottom 72+h   (reaches 199)
;   LINTEL  top 72-h        (can go negative)  bottom 72-h>>1 (never negative)
;   RAISED  top 72-h        (can go negative)  bottom 72+h-h>>2 (reaches 168)
;   RISER   top 72+h-h>>2   (never negative)   bottom 72+h   (reaches 199)
mark_span_b:
        ld a,(0xC018)
        cp 3
        jp z,msb_top_riser
        ; decreasing top: lo comes from hmax, hi from hmin
        ld a,(0xC018)
        or a
        ld a,71
        jp z,msb_t_base
        ld a,72
msb_t_base:
        ld (0xC01D),a                ; top base, 71 for FULL else 72
        ld a,(0xC01D)
        ld b,a
        ld a,(0xC01A)                ; hmax
        ld c,a
        ld a,b
        sub c
        call row_signed              ; -> LO
        ld (0xC010),a
        ld a,(0xC01D)
        ld b,a
        ld a,(0xC019)                ; hmin
        ld c,a
        ld a,b
        sub c
        call row_signed
        ld (0xC011),a
        jp msb_top_done
msb_top_riser:
        ld a,(0xC019)                ; increasing top: lo from hmin
        call riser_form
        call row_unsigned
        ld (0xC010),a
        ld a,(0xC01A)
        call riser_form
        call row_unsigned
        ld (0xC011),a
msb_top_done:
        call mark_col
        ; ---- bottom edge ----
        ld a,(0xC018)
        cp 1
        jp z,msb_bot_lintel
        cp 2
        jp z,msb_bot_raised
        ; FULL or RISER: bottom = 72+h, increasing
        ld a,(0xC019)
        add a,72
        call row_unsigned
        ld (0xC010),a
        ld a,(0xC01A)
        add a,72
        call row_unsigned
        ld (0xC011),a
        jp mark_col
msb_bot_raised:
        ld a,(0xC019)
        call riser_form
        call row_unsigned
        ld (0xC010),a
        ld a,(0xC01A)
        call riser_form
        call row_unsigned
        ld (0xC011),a
        jp mark_col
msb_bot_lintel:
        ; bottom = 72-(h>>1), decreasing: lo from hmax
        ld a,(0xC01A)
        srl a
        ld b,a
        ld a,72
        sub b
        call row_unsigned
        ld (0xC010),a
        ld a,(0xC019)
        srl a
        ld b,a
        ld a,72
        sub b
        call row_unsigned
        ld (0xC011),a
        jp mark_col

; A = h -> 72 + h - (h>>2). Reaches 168, so it is UNSIGNED from here on.
riser_form:
        ld b,a
        srl a
        srl a
        ld c,a
        ld a,b
        sub c
        add a,72
        ret

; A holds a value that may be negative: clamp to row 0, else shift.
row_signed:
        bit 7,a
        jp z,row_unsigned
        xor a
        ret

; A holds a non-negative value up to 199: shift unsigned, clamp to row 17.
row_unsigned:
        srl a
        srl a
        srl a
        cp 18
        ret c
        ld a,17
        ret

; HL += 4 + 3*COL -- the offset of this column's three retained bytes
ub_off:
        push bc
        ld a,(0xC012)
        ld c,a
        add a,a
        add a,c
        add a,4
        ld c,a
        ld b,0
        add hl,bc
        pop bc
        ret

; hmax over all four height bytes. HL -> old col bytes, DE -> new col bytes.
; The union needs the LARGEST height either state ever had in this column: the
; rows the old raster occupied and the rows the new one will occupy both have
; to be marked, and one hmax covers both.
ub_off2:
        ld a,(hl)                    ; old hl
        ld b,a                       ; B = running hmax
        ld c,a                       ; C = running hmin
        inc hl
        ld a,(hl)                    ; old hr
        call ub_o2acc
        ex de,hl                     ; HL -> new col bytes
        ld a,(hl)
        call ub_o2acc
        inc hl
        ld a,(hl)
        call ub_o2acc
        ld a,c
        ld (0xC019),a                ; hmin
        ld a,b
        ld (0xC01A),a                ; hmax
        ret
ub_o2acc:
        cp b
        jp c,ub_o2n
        ld b,a
ub_o2n:
        cp c
        jp nc,ub_o2x
        ld c,a
ub_o2x:
        ret

; A = hmax -> LO, HI. 71-hmax can go negative and clamps to row 0; 72+hmax
; reaches 199 so it shifts UNSIGNED - testing bit 7 would read 199 as negative,
; the trap A27 hit twice.
row_range:
        ld b,a
        add a,72
        srl a
        srl a
        srl a
        cp 18
        jp c,rr_hi_ok
        ld a,17
rr_hi_ok:
        ld (0xC011),a
        ld a,71
        sub b
        jp nc,rr_lo_pos
        xor a
        ld (0xC010),a
        ret
rr_lo_pos:
        srl a
        srl a
        srl a
        ld (0xC010),a
        ret

; ---- UNION_C's marking: the same ranges, table-driven ----
; The profile put the mark family at 33% of UNION_B - mk_sh's shift loop 6.3%,
; mk_have's pointer arithmetic 11.8% - and none of that is the work. Three
; small tables remove both, the same treatment A24's ROWPTR_B applied to
; row_addr. Tables at 0xE000: COLBIT[20], COLBYTE[20], ROWBASE[18] as 16-bit
; pointers straight into the mask.
; ---- UNION_G: hoist the per-column setup out of the per-range marking ----
; The profile put mark_col_t at 15.0% against mkt_loop's 8.2%, i.e. most of the
; marking cost is SETUP, not stores - three table lookups and a pointer
; assembly, ~146 T per call over ~37 calls. mark_span_b calls it twice for the
; SAME column, so half of that setup is done twice for nothing. Compute the
; column's bit and byte offset once per column and let both ranges use them.
mark_col_prep:
        push hl
        push bc
        push de
        ld a,(0xC012)
        ld c,a
        ld b,0
        ld hl,0xE000                 ; COLBIT
        add hl,bc
        ld a,(hl)
        ld (0xC013),a
        ld hl,0xE014                 ; COLBYTE
        add hl,bc
        ld a,(hl)
        ld (0xC028),a
        pop de
        pop bc
        pop hl
        ret

; Marks rows LO..HI of the prepared column. No table lookups.
mark_col_p:
        ld a,(0xC011)
        ld b,a
        ld a,(0xC010)
        ld c,a
        ld a,b
        sub c
        ret c
        inc a
        ld (0xC022),a
        ld a,(0xC013)
        ld d,a                       ; column bit
        ld a,(0xC010)
        ld c,a
        ld b,0
        ld hl,0xE028                 ; ROWBASE
        add hl,bc
        add hl,bc
        ld c,(hl)
        inc hl
        ld a,(hl)
        ld h,a
        ld l,c
        ld a,(0xC028)
        ld c,a
        ld b,0
        add hl,bc
        ld a,(0xC022)
        ld b,a
mkp_loop:
        ld a,(hl)
        or d
        ld (hl),a
        inc hl
        inc hl
        inc hl
        djnz mkp_loop
        ret

mark_col_t:
        ld a,(0xC011)
        ld b,a
        ld a,(0xC010)
        ld c,a
        ld a,b
        sub c
        ret c                        ; empty range
        inc a
        ld (0xC022),a                ; row count parked in memory: B is needed
                                     ; as the zero half of BC for every table
                                     ; index below, and holding the count there
                                     ; corrupted all three lookups
        ld a,(0xC012)
        ld c,a
        ld b,0
        ld hl,0xE000                 ; COLBIT
        add hl,bc
        ld a,(hl)
        ld d,a                       ; D = column bit
        ld hl,0xE014                 ; COLBYTE
        add hl,bc
        ld a,(hl)
        ld e,a                       ; E = byte offset inside the row
        ld a,(0xC010)
        ld c,a
        ld b,0
        ld hl,0xE028                 ; ROWBASE, 2 bytes per row
        add hl,bc
        add hl,bc
        ld c,(hl)
        inc hl
        ld a,(hl)
        ld h,a
        ld l,c                       ; HL = &mask[LO][0]
        ld c,e
        ld b,0
        add hl,bc                    ; + byte offset
        ld a,(0xC022)
        ld b,a
mkt_loop:
        ld a,(hl)
        or d
        ld (hl),a
        inc hl
        inc hl
        inc hl
        djnz mkt_loop
        ret

; OR column COL's bit into mask rows LO..HI
mark_col:
        ld a,(0xC012)
        ld c,a
        and 7
        inc a
        ld b,a
        ld a,1
mk_sh:
        dec b
        jp z,mk_have
        add a,a
        jp mk_sh
mk_have:
        ld d,a                       ; D = column bit
        ld a,(0xC010)
        ld l,a
        ld h,0
        add hl,hl
        ld b,0
        ld c,a
        add hl,bc                    ; 3*LO
        ld bc,0xCD00
        add hl,bc
        ld a,(0xC012)
        srl a
        srl a
        srl a
        ld c,a
        ld b,0
        add hl,bc                    ; + COL>>3
        ld a,(0xC011)
        ld b,a
        ld a,(0xC010)
        ld c,a
        ld a,b
        sub c
        ret c                        ; empty range
        inc a
        ld b,a
mk_loop:
        ld a,(hl)
        or d
        ld (hl),a
        inc hl
        inc hl
        inc hl
        djnz mk_loop
        ret
