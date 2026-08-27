#!/usr/bin/env python3
from pathlib import Path
import subprocess

raster_p=Path('src/tilesector_raster_gg.s')
opaque_p=Path('src/tilesector_opaque_gg.s')
raster=raster_p.read_text()

if 'STAGE18_DIRECT_OPAQUE' in raster:
    print('Stage 18 direct opaque materializer already applied')
    raise SystemExit(0)
if 'STAGE18_COLUMN_LIFETIME' not in raster:
    raise SystemExit('apply Stage 18 column lifetime first')

# Reuse the proven Stage-16 generator for run wiring/edge vocabulary. It only
# checks this marker to know that its store ABI is generation-aware; Stage 18
# immediately replaces those generated store semantics below.
if 'STAGE15_SHADOW_GENERATION' not in raster:
    raster=raster.replace('; STAGE18_COLUMN_LIFETIME\n',
                          '; STAGE18_COLUMN_LIFETIME\n; STAGE15_SHADOW_GENERATION generator-compat only\n',1)
    raster_p.write_text(raster)

subprocess.run(['python3','tools/apply_stage16_direct_opaque.py'],check=True)

raster=raster_p.read_text()
opaque=opaque_p.read_text()

if 'STAGE18_DIRECT_OPAQUE' in opaque:
    raise SystemExit('unexpected duplicate Stage18 marker')

opaque=opaque.replace(
'''        .globl  _g_nt_dirty
        .globl  _g_nt_meta
        .globl  _ts_edge_lut
''',
'''        .globl  _g_nt_dirty
        .globl  _ts_nt_mark_span
        .globl  _ts_edge_lut

; STAGE18_DIRECT_OPAQUE: visible name-table words carry no lifetime metadata.
''',1)

# Mark the complete vertical surface once. The Stage-18 implementation turns
# this into three prefix-mask XOR/OR stores regardless of wall height.
anchor='''op_bot_mm_done$:

        ; Top vector edge.
'''
coverage=r'''op_bot_mm_done$:

        ; Column-major lifetime coverage: first=max(top_min,clip_first,0),
        ; last=min(bottom_max,clip_last,17). One call marks all rows.
        ld      a, (#op_top_min$)
        bit     7, a
        jr      z, op_cov_first_nonneg$
        xor     a
op_cov_first_nonneg$:
        ld      e, a
        ld      a, (#op_clip_first$)
        ld      c, a
        ld      a, e
        cp      c
        jr      nc, op_cov_first_ready$
        ld      e, c
op_cov_first_ready$:

        ld      a, (#op_bot_max$)
        bit     7, a
        jr      nz, op_cov_done$
        cp      #18
        jr      c, op_cov_last_screen$
        ld      a, #17
op_cov_last_screen$:
        ld      c, a
        ld      a, (#op_clip_last$)
        cp      c
        jr      nc, op_cov_last_ready$
        ld      c, a
op_cov_last_ready$:
        ld      a, e
        cp      c
        jr      c, op_cov_emit$
        jr      nz, op_cov_done$
op_cov_emit$:
        ld      a, (#op_col$)
        ld      b, a
        ld      a, e
        call    _ts_nt_mark_span
op_cov_done$:

        ; Top vector edge.
'''
if anchor not in opaque:
    raise SystemExit('opaque coverage insertion anchor not found')
opaque=opaque.replace(anchor,coverage,1)

old_edge=r'''        ld      de, (#op_word$)
        ld      a, (hl)
        cp      e
        jp      nz, op_edge_changed$
        inc     hl
        ld      a, (hl)
        and     #0x1f
        cp      d
        dec     hl
        jp      nz, op_edge_changed$
        ; Visible word unchanged: only refresh RAM-private generation metadata.
        inc     hl
        ld      a, d
        and     #0x1f
        or      #0x20
        ld      c, a
        ld      a, (_g_nt_meta)
        or      c
        ld      (hl), a
        ret
op_edge_changed$:
        ld      (hl), e
        inc     hl
        ld      a, d
        and     #0x1f
        or      #0x20
        ld      c, a
        ld      a, (_g_nt_meta)
        or      c
        ld      (hl), a
        call    op_dirty_ptr_for_row$
        ld      a, (#op_dirty_mask$)
        or      (hl)
        ld      (hl), a
        ret
'''
new_edge=r'''        ld      de, (#op_word$)
        ld      a, (hl)
        cp      e
        jp      nz, op_edge_changed$
        inc     hl
        ld      a, (hl)
        cp      d
        dec     hl
        ret     z
op_edge_changed$:
        ld      (hl), e
        inc     hl
        ld      (hl), d
        call    op_dirty_ptr_for_row$
        ld      a, (#op_dirty_mask$)
        or      (hl)
        ld      (hl), a
        ret
'''
if old_edge not in opaque:
    raise SystemExit('Stage16 edge metadata store block not found')
opaque=opaque.replace(old_edge,new_edge,1)

old_int=r'''op_int_loop$:
        ld      a, (#op_full_tile$)
        ld      e, a
        ld      a, 0 (iy)
        cp      e
        jp      nz, op_int_changed$
        ld      a, 1 (iy)
        and     #0x1f
        jp      nz, op_int_changed$
        ; Same visible full tile: generation metadata only.
        ld      a, (_g_nt_meta)
        or      #0x20
        ld      1 (iy), a
        jp      op_int_advance$
op_int_changed$:
        ld      0 (iy), e
        ld      a, (_g_nt_meta)
        or      #0x20
        ld      1 (iy), a
        ld      a, (#op_dirty_mask$)
        or      0 (ix)
        ld      0 (ix), a
'''
new_int=r'''op_int_loop$:
        ld      a, (#op_full_tile$)
        ld      e, a
        ld      a, 0 (iy)
        cp      e
        jp      nz, op_int_changed$
        ld      a, 1 (iy)
        or      a
        jp      z, op_int_advance$
op_int_changed$:
        ld      0 (iy), e
        xor     a
        ld      1 (iy), a
        ld      a, (#op_dirty_mask$)
        or      0 (ix)
        ld      0 (ix), a
'''
if old_int not in opaque:
    raise SystemExit('Stage16 interior metadata block not found')
opaque=opaque.replace(old_int,new_int,1)

# There must be no runtime dependency on the Stage-15 generation byte.
if '_g_nt_meta' in opaque:
    raise SystemExit('unpatched Stage15 metadata reference remains in opaque kernel')

opaque=opaque.replace(
'; Only profile FULL (0) and RAISED_FULL (2) enter here.',
'; Only profile FULL (0) and RAISED_FULL (2) enter here.\n; STAGE18_DIRECT_OPAQUE',
1)

# Remove the compatibility marker from the linked raster source and record the
# real Stage-18 marker instead.
raster=raster_p.read_text()
raster=raster.replace('; STAGE15_SHADOW_GENERATION generator-compat only\n','')
raster=raster.replace('; STAGE18_COLUMN_LIFETIME\n',
                      '; STAGE18_COLUMN_LIFETIME\n; STAGE18_DIRECT_OPAQUE\n',1)

opaque_p.write_text(opaque)
raster_p.write_text(raster)
print('Applied Stage 18 direct opaque materializer on column-major lifetime state.')
