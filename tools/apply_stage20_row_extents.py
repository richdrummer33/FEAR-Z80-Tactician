#!/usr/bin/env python3
from pathlib import Path
import re

nt_p = Path('src/tilesector_ntstate_gg.s')
op_p = Path('src/tilesector_opaque_gg.s')
nt = nt_p.read_text()
op = op_p.read_text()

if 'STAGE20_ROW_EXTENTS' in nt:
    print('Stage 20 row extents already applied')
    raise SystemExit(0)
if 'STAGE19_ROW_BURST_UPLOAD' not in nt:
    raise SystemExit('apply Stage 19 row-burst uploader first')
if 'STAGE18_DIRECT_OPAQUE' not in op:
    raise SystemExit('apply Stage 18 direct opaque materializer first')

# ---------------------------------------------------------------------------
# Initialization: first frame marks every visible row [0..19] dirty.
# ---------------------------------------------------------------------------
old_init = '''        ; First visible frame uploads the whole 20x18 name table once.
        ld      hl, #_g_nt_dirty
        ld      b, #18
nti_dirty_rows$:
        ld      (hl), #0xff
        inc     hl
        ld      (hl), #0xff
        inc     hl
        ld      (hl), #0x0f
        inc     hl
        djnz    nti_dirty_rows$
'''
new_init = '''        ; STAGE20_ROW_EXTENTS: first visible frame uploads all 20 columns.
        ld      hl, #_g_nt_row_min
        ld      b, #18
nti_min_rows$:
        ld      (hl), #0
        inc     hl
        djnz    nti_min_rows$
        ld      hl, #_g_nt_row_max
        ld      b, #18
nti_max_rows$:
        ld      (hl), #19
        inc     hl
        djnz    nti_max_rows$
'''
if old_init not in nt:
    raise SystemExit('Stage18 initial dirty bitmap block not found')
nt = nt.replace(old_init, new_init, 1)

# ---------------------------------------------------------------------------
# Generic/cold dirty marker: A=row, B=column.
# ---------------------------------------------------------------------------
m = re.search(r'; A=row 0\.\.17, B=column 0\.\.19\. Called only when a visible word changed\.\n'
              r'_ts_nt_mark_dirty::.*?\n\s*ret\n', nt, re.S)
if not m:
    raise SystemExit('Stage18 mark-dirty routine not found')
new_mark = r'''; A=row 0..17, B=column 0..19. Expand the row's dirty interval.
; min=0xff denotes clean. max is irrelevant while min is clean.
_ts_nt_mark_dirty::
        push    af
        push    bc
        push    de
        push    hl

        ld      c, b
        ld      e, a
        ld      d, #0
        ld      hl, #_g_nt_row_min
        add     hl, de

        ld      a, (hl)
        cp      #0xff
        jr      z, ntd20_set_min$
        ld      a, c
        cp      (hl)
        jr      nc, ntd20_min_done$
ntd20_set_min$:
        ld      (hl), c
ntd20_min_done$:
        ld      de, #18
        add     hl, de                  ; corresponding row_max
        ld      a, c
        cp      (hl)
        jr      c, ntd20_done$
        jr      z, ntd20_done$
        ld      (hl), a
ntd20_done$:
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret
'''
nt = nt[:m.start()] + new_mark + nt[m.end():]

# ---------------------------------------------------------------------------
# Stage-19 uploader -> direct row extents. Same one-burst-per-dirty-row policy.
# ---------------------------------------------------------------------------
m = re.search(r'; STAGE19_ROW_BURST_UPLOAD.*?_ts_nt_upload_dirty::.*?\n\s*ret\n\n'
              r'; A = non-zero dirty byte\..*?ntu19_last_bit\$::?.*?\n\s*ret\n',
              nt, re.S)
if not m:
    # The labels are local ($), so use a broader boundary ending before .area _CODE.
    m = re.search(r'; STAGE19_ROW_BURST_UPLOAD.*?_ts_nt_upload_dirty::.*?\n\s*ret\n'
                  r'.*?(?=\n\s*\.area _CODE)', nt, re.S)
if not m:
    raise SystemExit('Stage19 uploader block not found')

new_upload = r'''; STAGE19_ROW_BURST_UPLOAD
; STAGE20_ROW_EXTENTS
; Dirty state is already the hardware transaction shape:
;   row_min[row], row_max[row]
; VBlank performs one OTIR burst directly from that interval.
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
        ld      ix, #_g_nt_row_min
        ld      iy, #_g_nt_row_max

ntu20_row_loop$:
        ld      a, 0 (ix)
        cp      #0xff
        jp      z, ntu20_next_row$
        ld      (#ntu20_first$), a
        ld      0 (ix), #0xff          ; clean immediately after consuming

        ld      a, 0 (iy)
        ld      (#ntu20_last$), a
        ld      0 (iy), #0

        ; words = last-first+1.
        ld      c, a
        ld      a, (#ntu20_first$)
        ld      e, a
        ld      a, c
        sub     e
        inc     a
        ld      (#ntu20_words$), a

        ld      e, a
        ld      a, (_g_ts_dirty_words)
        add     a, e
        ld      (_g_ts_dirty_words), a
        jr      nc, ntu20_count_ready$
        ld      a, (_g_ts_dirty_words+1)
        inc     a
        ld      (_g_ts_dirty_words+1), a
ntu20_count_ready$:

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
        ld      a, (#ntu20_first$)
        add     a, a
        ld      e, a
        ld      d, #0
        add     hl, de

        ; DE = VDP row base + first*2.
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
        ld      a, (#ntu20_first$)
        add     a, a
        ld      c, a
        ld      b, #0
        ex      de, hl
        add     hl, bc
        ex      de, hl
        pop     hl

        ; Exactly the Stage-19 hardware policy: one contiguous horizontal burst.
        di
        ld      a, e
        out     (#0xBF), a
        ld      a, d
        or      #0x40
        out     (#0xBF), a
        ld      c, #0xBE
        ld      a, (#ntu20_words$)
        add     a, a
        ld      b, a
        otir
        ei

ntu20_next_row$:
        inc     ix
        inc     iy
        ld      a, (#ntu_row$)
        inc     a
        ld      (#ntu_row$), a
        cp      #18
        jp      c, ntu20_row_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret
'''
nt = nt[:m.start()] + new_upload + nt[m.end():]

# Delete Stage19 first/last-bit LUTs if present: Stage20 never scans bits.
nt = re.sub(r'ntu19_first_lut\$:\n(?:\s*\.db.*\n)+\n'
            r'ntu19_last_lut\$:\n(?:\s*\.db.*\n)+\n', '', nt, count=1)

# BSS: replace the 54-byte bitmap with 36 bytes of row extents.
nt = nt.replace('_g_nt_dirty::      .ds 54',
                '_g_nt_row_min::    .ds 18\n_g_nt_row_max::    .ds 18', 1)

# Remove now-unused Stage19 scratch and add Stage20 scratch.
nt = re.sub(r'ntu19_d0\$:\s*\.ds 1\n'
            r'ntu19_d1\$:\s*\.ds 1\n'
            r'ntu19_d2\$:\s*\.ds 1\n'
            r'ntu19_first\$:\s*\.ds 1\n'
            r'ntu19_last\$:\s*\.ds 1\n'
            r'ntu19_words\$:\s*\.ds 1', '', nt, count=1)
anchor = 'ntu_mask$:         .ds 1'
if anchor in nt:
    nt = nt.replace(anchor, anchor + '\nntu20_first$:      .ds 1\nntu20_last$:       .ds 1\nntu20_words$:      .ds 1', 1)
else:
    # Put scratch after ntu_byte if Stage19 reshaped this region.
    anchor = 'ntu_byte$:         .ds 1'
    if anchor not in nt:
        raise SystemExit('Stage20 BSS scratch anchor not found')
    nt = nt.replace(anchor, anchor + '\nntu20_first$:      .ds 1\nntu20_last$:       .ds 1\nntu20_words$:      .ds 1', 1)

# ---------------------------------------------------------------------------
# Direct opaque hot path: stop computing dirty bitmap mask/group per column.
# Changed cells expand an extent directly. This preserves the Stage18 lifetime
# coverage experiment while making change notification row-native.
# ---------------------------------------------------------------------------
op = op.replace('        .globl  _g_nt_dirty\n',
                '        .globl  _g_nt_row_min\n        .globl  _g_nt_row_max\n', 1)

old_pre = '''        ; Precompute the dirty-bit address components once for the whole vertical
        ; column. A changed row is then just row*3 + group and one OR.
        and     #7
        ld      e, a
        ld      d, #0
        ld      hl, #op_mask_lut$
        add     hl, de
        ld      a, (hl)
        ld      (#op_dirty_mask$), a
        ld      a, (#op_col$)
        srl     a
        srl     a
        srl     a
        ld      (#op_dirty_group$), a

'''
if old_pre not in op:
    raise SystemExit('opaque dirty precompute block not found')
op = op.replace(old_pre, '', 1)

old_edge = '''        call    op_dirty_ptr_for_row$
        ld      a, (#op_dirty_mask$)
        or      (hl)
        ld      (hl), a
        ret
'''
new_edge = '''        call    op_dirty_extent_for_row$
        ret
'''
if old_edge not in op:
    raise SystemExit('opaque edge dirty block not found')
op = op.replace(old_edge, new_edge, 1)

old_setup = '''        ; IY = first name-table word; IX = matching dirty-byte group.
        ld      a, (#op_fill_first$)
        ld      (#op_row$), a
        call    op_map_ptr_for_row$
        push    hl
        pop     iy
        call    op_dirty_ptr_for_row$
        push    hl
        pop     ix
'''
new_setup = '''        ; IY = first name-table word. Dirty state is now row extents.
        ld      a, (#op_fill_first$)
        ld      (#op_row$), a
        call    op_map_ptr_for_row$
        push    hl
        pop     iy
'''
if old_setup not in op:
    raise SystemExit('opaque interior setup block not found')
op = op.replace(old_setup, new_setup, 1)

old_int_dirty = '''        ld      a, (#op_dirty_mask$)
        or      0 (ix)
        ld      0 (ix), a
op_int_advance$:
        ld      de, #40
        add     iy, de
        ld      de, #3
        add     ix, de
        djnz    op_int_loop$
'''
new_int_dirty = '''        call    op_dirty_extent_for_row$
op_int_advance$:
        ld      de, #40
        add     iy, de
        ld      a, (#op_row$)
        inc     a
        ld      (#op_row$), a
        djnz    op_int_loop$
'''
if old_int_dirty not in op:
    raise SystemExit('opaque interior dirty block not found')
op = op.replace(old_int_dirty, new_int_dirty, 1)

# Replace bitmap address helper with the much smaller min/max update helper.
m = re.search(r'; Uses op_row\. Return HL=&dirty\[row\*3 \+ column_group\]\.\n'
              r'op_dirty_ptr_for_row\$:.*?\n\s*ret\n', op, re.S)
if not m:
    raise SystemExit('opaque dirty helper not found')
new_helper = r'''; Uses op_row/op_col. Expand one row's horizontal dirty interval.
; Preserve B because the interior fill loop uses DJNZ.
op_dirty_extent_for_row$:
        push    bc
        ld      a, (#op_row$)
        ld      e, a
        ld      d, #0
        ld      hl, #_g_nt_row_min
        add     hl, de

        ld      a, (#op_col$)
        ld      c, a
        ld      a, (hl)
        cp      #0xff
        jr      z, op_de_set_min$
        ld      a, c
        cp      (hl)
        jr      nc, op_de_min_done$
op_de_set_min$:
        ld      (hl), c
op_de_min_done$:
        ld      de, #18
        add     hl, de
        ld      a, c
        cp      (hl)
        jr      c, op_de_done$
        jr      z, op_de_done$
        ld      (hl), a
op_de_done$:
        pop     bc
        ret
'''
op = op[:m.start()] + new_helper + op[m.end():]

# Remove bitmap-only LUT/BSS fields if present.
op = re.sub(r'op_mask_lut\$:\n\s*\.db 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80\n', '', op, count=1)
op = re.sub(r'op_dirty_mask\$:\s*\.ds 1\n', '', op, count=1)
op = re.sub(r'op_dirty_group\$:\s*\.ds 1\n', '', op, count=1)

nt_p.write_text(nt)
op_p.write_text(op)
print('Applied Stage 20 direct per-row dirty extents.')
