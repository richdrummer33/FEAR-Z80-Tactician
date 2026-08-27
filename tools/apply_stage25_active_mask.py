#!/usr/bin/env python3
from pathlib import Path

p=Path('src/tilesector_symfull_gg.s')
s=p.read_text()

if 'STAGE25_ACTIVE_MASK' in s:
    print('Stage 25 active mask already applied')
    raise SystemExit(0)
if 'STAGE24_RETAINED_LIFETIME' not in s:
    raise SystemExit('apply Stage 24 retained lifetime first')

s=s.replace('; STAGE24_RETAINED_LIFETIME\n',
            '; STAGE24_RETAINED_LIFETIME\n; STAGE25_ACTIVE_MASK\n',1)

# Current FULL occupancy is a 20-bit mask. Keep it alive through portal-face
# rendering so exception stale tests can query it; clear it at next begin-frame.
anchor='''        ld      (_g_ts_ret_span_skip), a
'''
repl=anchor+'''
        ld      (#sf_active_cur$), a
        ld      (#sf_active_cur$+1), a
        ld      (#sf_active_cur$+2), a
'''
if anchor not in s:
    raise SystemExit('Stage24 begin-frame counter anchor not found')
s=s.replace(anchor,repl,1)

# Helpers + sparse finalizer replace the Stage24 unconditional 20-column scan.
old='''_ts_retained_full_finalize_unseen::
        ld      a, #0
        ld      b, #19
        jp      _ts_retained_full_invalidate_range

; A=first column, B=last column. Retire only descriptors belonging to the
'''
new=r'''; A=column. Set CURRENT retained-FULL active bit.
sf_mark_active_cur$:
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #sf_active_lut$
        add     hl, de
        ld      e, (hl)                 ; group 0..2
        inc     hl
        ld      c, (hl)                 ; mask
        ld      d, #0
        ld      hl, #sf_active_cur$
        add     hl, de
        ld      a, (hl)
        or      c
        ld      (hl), a
        ret

; A=column. Return NZ iff PREVIOUS active bit was set, and clear that bit.
sf_take_active_prev$:
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #sf_active_lut$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      c, (hl)
        ld      d, #0
        ld      hl, #sf_active_prev$
        add     hl, de
        ld      a, (hl)
        and     c
        jr      z, sf_take_prev_no$
        ld      a, c
        cpl
        and     (hl)
        ld      (hl), a
        ld      a, #1
        or      a
        ret
sf_take_prev_no$:
        xor     a
        ret

; A=column. Return NZ iff CURRENT active bit is set.
sf_test_active_cur$:
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #sf_active_lut$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      c, (hl)
        ld      d, #0
        ld      hl, #sf_active_cur$
        add     hl, de
        ld      a, (hl)
        and     c
        ret

; Stage 24 walked all twenty retained columns every frame. Stage 25 computes
; stale = previousActive & ~currentActive over only THREE bytes, and only calls
; the existing restore path for actual disappeared bits.
_ts_retained_full_finalize_unseen::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ld      ix, #sf_active_prev$
        ld      iy, #sf_active_cur$
        xor     a
        ld      (#sf_active_group$), a
        ld      (#sf_active_base$), a

sf_active_group_loop$:
        ld      a, 0 (iy)
        ld      d, a                    ; current byte
        ld      a, 0 (ix)
        ld      e, a                    ; previous byte
        ld      a, d
        cpl
        and     e
        ld      (#sf_active_stale$), a
        or      a
        jr      z, sf_active_group_commit$

        ld      a, (#sf_active_base$)
        ld      (#sf_active_scan_col$), a

sf_active_bit_loop$:
        ld      a, (#sf_active_stale$)
        srl     a
        ld      (#sf_active_stale$), a
        jr      nc, sf_active_bit_next$
        ld      a, (#sf_active_scan_col$)
        ld      b, a
        call    _ts_retained_full_invalidate_range

sf_active_bit_next$:
        ld      a, (#sf_active_scan_col$)
        inc     a
        ld      (#sf_active_scan_col$), a
        ld      a, (#sf_active_stale$)
        or      a
        jr      nz, sf_active_bit_loop$

sf_active_group_commit$:
        ; The current mask becomes next frame's previous set. Do NOT clear
        ; current here: portal exception reconciliation still queries it.
        ld      a, 0 (iy)
        ld      0 (ix), a
        inc     ix
        inc     iy

        ld      a, (#sf_active_base$)
        add     a, #8
        ld      (#sf_active_base$), a
        ld      a, (#sf_active_group$)
        inc     a
        ld      (#sf_active_group$), a
        cp      #3
        jr      c, sf_active_group_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; A=first column, B=last column. Retire only descriptors belonging to the
'''
if old not in s:
    raise SystemExit('Stage24 finalize scan anchor not found')
s=s.replace(old,new,1)

# Range invalidation now consults the previous active mask first. No inactive
# column pays generation-pointer/descriptor work, and consuming a previous FULL
# clears its mask bit immediately.
old=r'''sf_inv_loop$:
        ld      a, (#sf_inv_col$)
        ld      (#sf_col$), a

        ; HL -> generation byte.
        ld      e, a
        ld      d, #0
        ld      hl, #sf_prev_gen$
        add     hl, de
        push    hl
        pop     iy

        ld      a, (#sf_ret_frame$)
        dec     a
        ld      c, a
        ld      a, 0 (iy)
        cp      c
        jp      nz, sf_inv_next$

        ; IX -> seven-byte retained descriptor: col*7 = col*8-col.
'''
new=r'''sf_inv_loop$:
        ld      a, (#sf_inv_col$)
        ld      (#sf_col$), a
        call    sf_take_active_prev$
        jp      z, sf_inv_next$

        ; HL -> generation byte. Active-prev is authoritative for whether this
        ; is a prior FULL; generation remains only the retained-descriptor reuse tag.
        ld      a, (#sf_inv_col$)
        ld      e, a
        ld      d, #0
        ld      hl, #sf_prev_gen$
        add     hl, de
        push    hl
        pop     iy

        ; IX -> seven-byte retained descriptor: col*7 = col*8-col.
'''
if old not in s:
    raise SystemExit('Stage24 invalidate generation scan block not found')
s=s.replace(old,new,1)

# Mark every eligible current depth-0 FULL column once before retained compare.
anchor='''        ld      a, (#_g_name_run_ctx + 14)
        or      a
        jp      z, sf_ret_not_eligible$

        ld      a, (_g_ts_ret_full_total)
'''
repl='''        ld      a, (#_g_name_run_ctx + 14)
        or      a
        jp      z, sf_ret_not_eligible$

        ld      a, (#sf_col$)
        call    sf_mark_active_cur$

        ld      a, (_g_ts_ret_full_total)
'''
if anchor not in s:
    raise SystemExit('Stage24 retained eligibility anchor not found')
s=s.replace(anchor,repl,1)

# Exception stale ownership uses the current active mask instead of another
# generation-array lookup.
old=r'''        ld      c, a                    ; queried row
        ld      d, #0
        ld      hl, #sf_prev_gen$
        add     hl, de
        ld      a, (#sf_ret_frame$)
        ld      b, a
        ld      a, (hl)
        cp      b
        jp      nz, sf_owns_no$

        ; IX = descriptor for E column.
        ld      a, e
        ld      b, a
'''
new=r'''        ld      c, a                    ; queried row
        ld      b, e                    ; preserve queried column
        ld      a, e
        call    sf_test_active_cur$
        jp      z, sf_owns_no$
        ld      e, b

        ; IX = descriptor for E column.
        ld      a, e
        ld      b, a
'''
if old not in s:
    raise SystemExit('Stage24 owns-cell generation block not found')
s=s.replace(old,new,1)

# 20 entries: [byte-group, bit-mask].
lut='''sf_active_lut$:
        .db 0,0x01, 0,0x02, 0,0x04, 0,0x08
        .db 0,0x10, 0,0x20, 0,0x40, 0,0x80
        .db 1,0x01, 1,0x02, 1,0x04, 1,0x08
        .db 1,0x10, 1,0x20, 1,0x40, 1,0x80
        .db 2,0x01, 2,0x02, 2,0x04, 2,0x08

'''
anchor='_ts_retained_full_finalize_unseen::\n'
if anchor not in s:
    raise SystemExit('Stage25 finalizer label missing after replacement')
s=s.replace(anchor,lut+anchor,1)

# Tiny fixed mask state.
anchor='''sf_prev_gen_ptr$:         .ds 2
sf_ret_style$:            .ds 1
'''
repl='''sf_prev_gen_ptr$:         .ds 2
sf_active_prev$:          .ds 3
sf_active_cur$:           .ds 3
sf_active_group$:         .ds 1
sf_active_base$:          .ds 1
sf_active_stale$:         .ds 1
sf_active_scan_col$:      .ds 1
sf_ret_style$:            .ds 1
'''
if anchor not in s:
    raise SystemExit('Stage24 BSS insertion anchor not found')
s=s.replace(anchor,repl,1)

p.write_text(s)
print('Applied Stage 25 20-bit retained FULL active/stale masks.')
