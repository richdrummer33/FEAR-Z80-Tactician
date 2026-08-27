#!/usr/bin/env python3
from pathlib import Path
import re

p = Path('src/tilesector_ntstate_gg.s')
s = p.read_text()

if 'STAGE19_ROW_BURST_UPLOAD' in s:
    print('Stage 19 row-burst uploader already applied')
    raise SystemExit(0)

m = re.search(r'; Scan the existing 54-byte row-major dirty bitset and upload only changed\n'
              r'; name-table words\..*?_ts_nt_upload_dirty::.*?\n\s*ret\n',
              s, re.S)
if not m:
    raise SystemExit('Stage 18 dirty uploader block not found')

first = []
last = []
for v in range(256):
    if v == 0:
        first.append(0)
        last.append(0)
    else:
        first.append((v & -v).bit_length() - 1)
        last.append(v.bit_length() - 1)

def db_rows(vals):
    out=[]
    for i in range(0,256,16):
        out.append('        .db ' + ','.join(f'0x{x:02x}' for x in vals[i:i+16]))
    return '\n'.join(out)

new = r'''; STAGE19_ROW_BURST_UPLOAD
; Dirty state is still exact at tile granularity, but VBlank consumes it as at
; most ONE horizontal burst per visible row.
;
; For each 20-bit dirty row:
;   first = first dirty column
;   last  = last dirty column
;   upload the authoritative shadow [first..last] with one VDP address setup
;   and one OTIR byte stream.
;
; This intentionally rewrites clean words between separated dirty islands.
; On a 20-column GG viewport that can be cheaper than paying another VDP control
; address transaction and per-word setup. The profiler decides whether a later
; two-burst split heuristic is worthwhile.
_ts_nt_upload_dirty::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        xor     a
        ld      (_g_ts_dirty_words), a
        ld      (_g_ts_dirty_words+1), a
        ld      (#ntu_row$), a
        ld      ix, #_g_nt_dirty

ntu19_row_loop$:
        ; Consume and clear this row's compact 20-bit dirty mask.
        ld      a, 0 (ix)
        ld      (#ntu19_d0$), a
        ld      0 (ix), #0
        ld      d, a
        ld      a, 1 (ix)
        ld      (#ntu19_d1$), a
        ld      1 (ix), #0
        or      d
        ld      d, a
        ld      a, 2 (ix)
        and     #0x0f
        ld      (#ntu19_d2$), a
        ld      2 (ix), #0
        or      d
        jp      z, ntu19_next_row$

        ; Find first dirty column from the first non-zero 8-bit group.
        ld      a, (#ntu19_d0$)
        or      a
        jr      nz, ntu19_first_g0$
        ld      a, (#ntu19_d1$)
        or      a
        jr      nz, ntu19_first_g1$
        ld      a, (#ntu19_d2$)
        call    ntu19_first_bit$
        add     a, #16
        jr      ntu19_first_ready$
ntu19_first_g1$:
        call    ntu19_first_bit$
        add     a, #8
        jr      ntu19_first_ready$
ntu19_first_g0$:
        call    ntu19_first_bit$
ntu19_first_ready$:
        ld      (#ntu19_first$), a

        ; Find last dirty column from the last non-zero 8-bit group.
        ld      a, (#ntu19_d2$)
        or      a
        jr      z, ntu19_last_not_g2$
        call    ntu19_last_bit$
        add     a, #16
        jr      ntu19_last_ready$
ntu19_last_not_g2$:
        ld      a, (#ntu19_d1$)
        or      a
        jr      z, ntu19_last_g0$
        call    ntu19_last_bit$
        add     a, #8
        jr      ntu19_last_ready$
ntu19_last_g0$:
        ld      a, (#ntu19_d0$)
        call    ntu19_last_bit$
ntu19_last_ready$:
        ld      (#ntu19_last$), a

        ; words = last-first+1, bytes = words*2.
        ld      c, a
        ld      a, (#ntu19_first$)
        ld      e, a
        ld      a, c
        sub     e
        inc     a
        ld      (#ntu19_words$), a

        ; Maintain profiler-visible upload word count, including deliberately
        ; bridged clean gap words.
        ld      e, a
        ld      a, (_g_ts_dirty_words)
        add     a, e
        ld      (_g_ts_dirty_words), a
        jr      nc, ntu19_count_ready$
        ld      a, (_g_ts_dirty_words+1)
        inc     a
        ld      (_g_ts_dirty_words+1), a
ntu19_count_ready$:

        ; HL = authoritative RAM row base + first*2.
        ld      a, (#ntu_row$)
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #nt_map_rows$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ex      de, hl
        ld      a, (#ntu19_first$)
        add     a, a
        ld      e, a
        ld      d, #0
        add     hl, de

        ; DE = VDP name-table row base + first*2.
        push    hl
        ld      a, (#ntu_row$)
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #nt_vdp_rows$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ld      a, (#ntu19_first$)
        add     a, a
        ld      c, a
        ld      b, #0
        ex      de, hl
        add     hl, bc
        ex      de, hl
        pop     hl

        ; One hardware transaction for the complete horizontal interval.
        ; Game Gear VDP data port auto-increments VRAM after every byte.
        di
        ld      a, e
        out     (#0xBF), a
        ld      a, d
        or      #0x40
        out     (#0xBF), a
        ld      c, #0xBE
        ld      a, (#ntu19_words$)
        add     a, a
        ld      b, a
        otir
        ei

ntu19_next_row$:
        inc     ix
        inc     ix
        inc     ix
        ld      a, (#ntu_row$)
        inc     a
        ld      (#ntu_row$), a
        cp      #18
        jp      c, ntu19_row_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; A = non-zero dirty byte. LUT lookup is faster and smaller in hot VBlank code
; than a variable-length shift/search loop.
ntu19_first_bit$:
        ld      l, a
        ld      h, #0
        ld      de, #ntu19_first_lut$
        add     hl, de
        ld      a, (hl)
        ret

ntu19_last_bit$:
        ld      l, a
        ld      h, #0
        ld      de, #ntu19_last_lut$
        add     hl, de
        ld      a, (hl)
        ret
'''
s = s[:m.start()] + new + s[m.end():]

# Tables live in ROM. Keep the existing map/vdp row tables untouched.
anchor = 'nt_map_rows$:'
if anchor not in s:
    raise SystemExit('nt_map_rows table anchor not found')
tables = 'ntu19_first_lut$:\n' + db_rows(first) + '\n\nntu19_last_lut$:\n' + db_rows(last) + '\n\n'
s = s.replace(anchor, tables + anchor, 1)

# Extend BSS scratch.
anchor = 'ntu_mask$:         .ds 1'
if anchor not in s:
    # Stage18 may no longer use ntu_mask label, insert before final quote/BSS end.
    anchor = 'ntu_byte$:         .ds 1'
if anchor not in s:
    raise SystemExit('uploader BSS anchor not found')
extra = anchor + '''
ntu19_d0$:         .ds 1
ntu19_d1$:         .ds 1
ntu19_d2$:         .ds 1
ntu19_first$:      .ds 1
ntu19_last$:       .ds 1
ntu19_words$:      .ds 1'''
s = s.replace(anchor, extra, 1)

p.write_text(s)
print('Applied Stage 19 one-burst-per-dirty-row VDP uploader.')
