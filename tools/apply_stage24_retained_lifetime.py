#!/usr/bin/env python3
from pathlib import Path

core_p=Path('src/tilesector_core.c')
run_p=Path('src/tilesector_run_gg.s')
sym_p=Path('src/tilesector_symfull_gg.s')
nt_p=Path('src/tilesector_ntstate_gg.s')

core=core_p.read_text()
run=run_p.read_text()
sym=sym_p.read_text()
nt=nt_p.read_text()

if 'STAGE24_RETAINED_LIFETIME' in sym:
    print('Stage 24 retained lifetime already applied')
    raise SystemExit(0)
if 'STAGE23_EDGE_DELTA' not in sym:
    raise SystemExit('apply Stage 23 edge delta first')
if 'STAGE18_COLUMN_LIFETIME' not in nt:
    raise SystemExit('Stage 18 exception lifetime state missing')

# ===========================================================================
# CORE: retained FULL unseen cleanup happens after depth-0 solids but BEFORE
# portal faces. Also move begin-frame accounting under the lifetime stage.
# ===========================================================================
old='''void ts_raster_surface_run_fast(void);
void ts_retained_full_begin_frame(void);
#endif
'''
new='''void ts_raster_surface_run_fast(void);
void ts_retained_full_begin_frame(void);
void ts_retained_full_finalize_unseen(void);
#endif
'''
if old not in core:
    raise SystemExit('Stage22 retained declarations not found')
core=core.replace(old,new,1)

old='''    g_ts_render_stage=4u;
    render_sector_candidates(depth,view_c0,view_c1,out_map,cols);
    g_ts_render_stage=5u;
'''
new='''    g_ts_render_stage=4u;
    render_sector_candidates(depth,view_c0,view_c1,out_map,cols);
#ifdef __SDCC
    if (depth==0u) {
        g_ts_render_stage=7u; /* retained FULL lifetime / unseen columns */
        ts_retained_full_finalize_unseen();
    }
#endif
    g_ts_render_stage=5u;
'''
if old not in core:
    raise SystemExit('render_sector stage transition not found')
core=core.replace(old,new,1)

old='''    uint8_t sector;
#ifdef __SDCC
    ts_retained_full_begin_frame();
#endif
    g_ts_render_stage=1u;
    clear_frame(out_map,cols);
'''
new='''    uint8_t sector;
    g_ts_render_stage=1u;
#ifdef __SDCC
    ts_retained_full_begin_frame();
#endif
    clear_frame(out_map,cols);
'''
if old not in core:
    raise SystemExit('Stage22 begin-frame placement not found')
core=core.replace(old,new,1)

# ===========================================================================
# RUN KERNEL: a depth-0 non-FULL run must retire any previous retained FULL
# columns BEFORE the new materializer writes them. Empty/no-wall columns are
# retired by finalize_unseen before portals.
# ===========================================================================
run=run.replace('; STAGE22_RETAINED_FULL\n',
                '; STAGE22_RETAINED_FULL\n; STAGE24_RETAINED_LIFETIME\n',1)

run=run.replace(
'''        .globl  _g_ts_ret_span_skip
''',
'''        .globl  _g_ts_ret_span_skip
        .globl  _ts_retained_full_invalidate_range
''',1)

anchor='''        ld      a, (#_g_name_run_ctx + 2)
        ld      (#nr_profile$), a
        ld      (#_g_raster_ctx + 0), a

        ; A whole contiguous run is the natural retained-span unit.
'''
insert='''        ld      a, (#_g_name_run_ctx + 2)
        ld      (#nr_profile$), a
        ld      (#_g_raster_ctx + 0), a

        ; If a previous retained FULL wall is replaced by a depth-0 asymmetric
        ; solid, clear the old FULL hardware state before the replacement draws.
        ld      a, (#_g_name_run_ctx + 14)
        or      a
        jp      z, nr_ret_life_ready$
        ld      a, (#nr_profile$)
        or      a
        jp      z, nr_ret_life_ready$
        ld      a, (#nr_end$)
        ld      b, a
        ld      a, (#nr_col$)
        call    _ts_retained_full_invalidate_range
nr_ret_life_ready$:

        ; A whole contiguous run is the natural retained-span unit.
'''
if anchor not in run:
    raise SystemExit('run retained-span anchor not found')
run=run.replace(anchor,insert,1)

# ===========================================================================
# SYMMETRIC FULL RETAINED STATE
# Descriptor expands from 5 to 7 bytes:
#   0 top_l, 1 top_r, 2 style, 3 clip_first, 4 clip_last,
#   5 final coverage first row (0xff = none), 6 coverage last row.
# Those last two bytes ARE the ordinary-wall lifetime metadata.
# ===========================================================================
sym=sym.replace('; STAGE23_EDGE_DELTA\n',
                '; STAGE23_EDGE_DELTA\n; STAGE24_RETAINED_LIFETIME\n',1)

# Generation wrap: preserve descriptors valid on frame 254 by normalizing their
# generation to 0 when the next logical frame becomes 1.
old=r'''        ; Generation 0xFF is permanently reserved as INVALID. On first use and
        ; once every 254 builds, invalidate the tiny 20-byte generation array.
        ld      a, (#sf_ret_frame$)
        or      a
        jr      z, sf_ret_reset_generation$
        inc     a
        cp      #0xff
        jr      nz, sf_ret_store_generation$
sf_ret_reset_generation$:
        ld      a, #1
        ld      (#sf_ret_frame$), a
        ld      hl, #sf_prev_gen$
        ld      (hl), #0xff
        ld      de, #sf_prev_gen$+1
        ld      bc, #19
        ldir
        jr      sf_ret_begin_done$
sf_ret_store_generation$:
        ld      (#sf_ret_frame$), a
sf_ret_begin_done$:
'''
new=r'''        ; Generation 0xFF is INVALID. At wrap, current-frame 254 entries
        ; normalize to generation 0 so frame 1 can still recognize them as its
        ; immediate predecessor. Unseen columns are invalidated every frame.
        ld      a, (#sf_ret_frame$)
        or      a
        jp      z, sf_ret_init_generation$
        inc     a
        cp      #0xff
        jp      nz, sf_ret_store_generation$

        ld      a, #1
        ld      (#sf_ret_frame$), a
        ld      hl, #sf_prev_gen$
        ld      b, #20
sf_ret_wrap_loop$:
        ld      a, (hl)
        cp      #0xfe
        jp      nz, sf_ret_wrap_next$
        xor     a
        ld      (hl), a
sf_ret_wrap_next$:
        inc     hl
        djnz    sf_ret_wrap_loop$
        jp      sf_ret_begin_done$

sf_ret_init_generation$:
        ld      a, #1
        ld      (#sf_ret_frame$), a
        ld      hl, #sf_prev_gen$
        ld      (hl), #0xff
        ld      de, #sf_prev_gen$+1
        ld      bc, #19
        ldir
        jp      sf_ret_begin_done$

sf_ret_store_generation$:
        ld      (#sf_ret_frame$), a
sf_ret_begin_done$:
'''
if old not in sym:
    raise SystemExit('Stage22 generation block not found')
sym=sym.replace(old,new,1)

# Export range invalidation/finalization/ownership before the column entry.
anchor='_ts_raster_symfull_column_fast::\n'
if anchor not in sym:
    raise SystemExit('symfull raster entry not found')

lifetime=r'''_ts_retained_full_finalize_unseen::
        ld      a, #0
        ld      b, #19
        jp      _ts_retained_full_invalidate_range

; A=first column, B=last column. Retire only descriptors belonging to the
; immediately preceding frame. Refreshed current FULL columns have current gen.
_ts_retained_full_invalidate_range::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ld      (#sf_inv_col$), a
        ld      a, b
        ld      (#sf_inv_last$), a

sf_inv_loop$:
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
        ld      a, (#sf_inv_col$)
        ld      e, a
        add     a, a
        add     a, a
        add     a, a
        sub     e
        ld      e, a
        ld      d, #0
        ld      ix, #sf_prev_desc$
        add     ix, de

        ld      a, 5 (ix)
        cp      #0xff
        jp      z, sf_inv_mark_invalid$
        ld      c, 6 (ix)
        call    sf_restore_range$

sf_inv_mark_invalid$:
        ld      0 (iy), #0xff

sf_inv_next$:
        ld      a, (#sf_inv_col$)
        ld      c, a
        ld      a, (#sf_inv_last$)
        cp      c
        jp      z, sf_inv_done$
        ld      a, c
        inc     a
        ld      (#sf_inv_col$), a
        jp      sf_inv_loop$

sf_inv_done$:
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; A=row, E=column. Return A=1 iff a CURRENT retained depth-0 FULL wall owns
; this hardware tile row. Called only on rare stale exception cells.
_ts_retained_full_owns_cell::
        push    bc
        push    de
        push    hl
        push    ix

        ld      c, a                    ; queried row
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
        add     a, a
        add     a, a
        add     a, a
        sub     b
        ld      e, a
        ld      d, #0
        ld      ix, #sf_prev_desc$
        add     ix, de
        ld      a, 5 (ix)
        cp      #0xff
        jp      z, sf_owns_no$
        cp      c
        jp      nc, sf_owns_first_ge$
        jp      sf_owns_first_ok$
sf_owns_first_ge$:
        jp      nz, sf_owns_no$
sf_owns_first_ok$:
        ld      a, 6 (ix)
        cp      c
        jp      c, sf_owns_no$
        ld      a, #1
        jp      sf_owns_done$
sf_owns_no$:
        xor     a
sf_owns_done$:
        pop     ix
        pop     hl
        pop     de
        pop     bc
        ret

''' + anchor
sym=sym.replace(anchor,lifetime,1)

# Current coverage: compute it for retained FULL, but only exception/recursive
# FULL walls call the Stage-18 coverage marker.
anchor='''sf_aperture_ready$:
        xor     a
        ld      (#sf_ret_mode$), a

        ; Only the TOP endpoint rows are solved.
'''
repl='''sf_aperture_ready$:
        xor     a
        ld      (#sf_ret_mode$), a
        ld      (#sf_cov_valid$), a

        ; Only the TOP endpoint rows are solved.
'''
if anchor not in sym:
    raise SystemExit('Stage23 aperture init not found')
sym=sym.replace(anchor,repl,1)

old='''sf_cov_emit$:
        ld      (#sf_cov_first$), a
        ld      a, (#sf_col$)
        ld      b, a
        ld      a, (#sf_cov_first$)
        call    _ts_nt_mark_span
sf_cov_done$:
'''
new='''sf_cov_emit$:
        ld      (#sf_cov_first$), a
        ld      a, c
        ld      (#sf_cov_last$), a
        ld      a, #1
        ld      (#sf_cov_valid$), a

        ; Ordinary retained depth-0 FULL walls own their lifetime directly.
        ; Recursive/exception FULL walls retain Stage-18 generic coverage.
        ld      a, (#_g_name_run_ctx + 14)
        or      a
        jp      nz, sf_cov_done$
        ld      a, (#sf_col$)
        ld      b, a
        ld      a, (#sf_cov_first$)
        ld      c, (#sf_cov_last$) ; generator fixes this illegal load below
        call    _ts_nt_mark_span
sf_cov_done$:
'''
if old not in sym:
    raise SystemExit('Stage21 coverage emit not found')
sym=sym.replace(old,new,1)
# Legal Z80 load for C.
sym=sym.replace('        ld      c, (#sf_cov_last$) ; generator fixes this illegal load below',
                '        ld      a, (#sf_cov_last$)\n        ld      c, a\n        ld      a, (#sf_cov_first$)',1)

# Descriptor pointer: 5 bytes -> 7 bytes.
old='''        ; IX = five-byte previous final-quantized descriptor for this column.
        ld      a, (#sf_col$)
        ld      e, a
        add     a, a                   ; 2x
        add     a, a                   ; 4x
        add     a, e                   ; 5x
        ld      e, a
'''
new='''        ; IX = seven-byte retained hardware descriptor for this column.
        ld      a, (#sf_col$)
        ld      e, a
        add     a, a                   ; 2x
        add     a, a                   ; 4x
        add     a, a                   ; 8x
        sub     e                      ; 7x
        ld      e, a
'''
if old not in sym:
    raise SystemExit('Stage22 descriptor index not found')
sym=sym.replace(old,new,1)

# Once previous generation is proven, capture its retained coverage range.
anchor='''        ld      a, (hl)
        cp      c
        jp      nz, sf_ret_store_current$

        ; First classify non-geometric state.
'''
repl='''        ld      a, (hl)
        cp      c
        jp      nz, sf_ret_store_current$

        ld      a, 5 (ix)
        ld      (#sf_old_cov_first$), a
        ld      a, 6 (ix)
        ld      (#sf_old_cov_last$), a

        ; First classify non-geometric state.
'''
if anchor not in sym:
    raise SystemExit('previous-generation compare anchor not found')
sym=sym.replace(anchor,repl,1)

# General state changes must reconcile OLD stale FULL rows before overwriting desc.
for old_branch in (
    '        jp      nz, sf_ret_store_current$\n',
):
    pass
# Replace exactly the first three mismatch branches in non-geometric comparisons.
needle='''        ld      a, (#sf_ret_style$)
        cp      c
        jp      nz, sf_ret_store_current$
        ld      a, 3 (ix)
        ld      c, a
        ld      a, (#sf_clip_first$)
        cp      c
        jp      nz, sf_ret_store_current$
        ld      a, 4 (ix)
        ld      c, a
        ld      a, (#sf_clip_last$)
        cp      c
        jp      nz, sf_ret_store_current$
'''
replacement='''        ld      a, (#sf_ret_style$)
        cp      c
        jp      nz, sf_ret_general_change$
        ld      a, 3 (ix)
        ld      c, a
        ld      a, (#sf_clip_first$)
        cp      c
        jp      nz, sf_ret_general_change$
        ld      a, 4 (ix)
        ld      c, a
        ld      a, (#sf_clip_last$)
        cp      c
        jp      nz, sf_ret_general_change$
'''
if needle not in sym:
    raise SystemExit('retained non-geometric compare block not found')
sym=sym.replace(needle,replacement,1)

# Edge-only and general-change paths reconcile old-new lifetime.
old='''sf_ret_edge_only$:
        ld      a, (_g_ts_ret_full_edgeonly)
'''
new='''sf_ret_general_change$:
        call    sf_restore_old_stale$
        jp      sf_ret_store_current$

sf_ret_edge_only$:
        call    sf_restore_old_stale$
        ld      a, (_g_ts_ret_full_edgeonly)
'''
if old not in sym:
    raise SystemExit('Stage23 edge-only label not found')
sym=sym.replace(old,new,1)

# Store the retained coverage range alongside the 5 display fields.
old='''        ld      a, (#sf_clip_last$)
        ld      4 (ix), a
        ld      hl, (#sf_prev_gen_ptr$)
'''
new='''        ld      a, (#sf_clip_last$)
        ld      4 (ix), a
        ld      a, (#sf_cov_valid$)
        or      a
        jp      z, sf_ret_store_no_cov$
        ld      a, (#sf_cov_first$)
        ld      5 (ix), a
        ld      a, (#sf_cov_last$)
        ld      6 (ix), a
        jp      sf_ret_cov_stored$
sf_ret_store_no_cov$:
        ld      5 (ix), #0xff
        ld      6 (ix), #0
sf_ret_cov_stored$:
        ld      hl, (#sf_prev_gen_ptr$)
'''
if old not in sym:
    raise SystemExit('retained descriptor store block not found')
sym=sym.replace(old,new,1)

# Restore helper suite just before the Stage23 delta materializer.
anchor='''sf_ret_edge_delta$:
        ; OLD top_max from the two retained signed pixel endpoints.
'''
helpers=r'''; Restore OLD retained coverage that lies outside CURRENT coverage.
sf_restore_old_stale$:
        ld      a, (#sf_old_cov_first$)
        cp      #0xff
        ret     z
        ld      e, a                    ; old first
        ld      a, (#sf_old_cov_last$)
        ld      d, a                    ; old last

        ld      a, (#sf_cov_valid$)
        or      a
        jp      z, sf_restore_old_all$

        ; Prefix: old_first .. min(old_last,current_first-1)
        ld      a, (#sf_cov_first$)
        ld      c, a
        ld      a, e
        cp      c
        jp      nc, sf_restore_suffix_check$
        ld      a, c
        dec     a
        cp      d
        jp      c, sf_restore_prefix_end_ready$
        ld      a, d
sf_restore_prefix_end_ready$:
        ld      c, a
        ld      a, e
        call    sf_restore_range$

sf_restore_suffix_check$:
        ld      a, (#sf_cov_last$)
        ld      c, a
        ld      a, d
        cp      c
        ret     c
        ret     z
        ld      a, c
        inc     a
        cp      e
        jp      nc, sf_restore_suffix_start_ready$
        ld      a, e
sf_restore_suffix_start_ready$:
        ld      c, d
        call    sf_restore_range$
        ret

sf_restore_old_all$:
        ld      a, e
        ld      c, d
        call    sf_restore_range$
        ret

; A=start row, C=end row. Restore static ceiling/horizon/floor name-table words
; for this retained column and expand Stage-20 dirty extents.
sf_restore_range$:
        ld      (#sf_restore_row$), a
        ld      a, c
        ld      (#sf_restore_last$), a
sf_restore_range_loop$:
        ld      a, (#sf_restore_row$)
        call    sf_restore_base_row$
        ld      a, (#sf_restore_row$)
        ld      c, a
        ld      a, (#sf_restore_last$)
        cp      c
        ret     z
        ld      a, c
        inc     a
        ld      (#sf_restore_row$), a
        jp      sf_restore_range_loop$

sf_restore_base_row$:
        ld      (#sf_store_row$), a
        cp      #9
        jp      c, sf_restore_ceil$
        jp      z, sf_restore_horizon$
        ld      e, #1
        jp      sf_restore_base_ready$
sf_restore_horizon$:
        ld      e, #2
        jp      sf_restore_base_ready$
sf_restore_ceil$:
        ld      e, #0
sf_restore_base_ready$:
        ld      d, #0
        ld      a, (#sf_store_row$)
        call    sf_map_ptr_for_row$
        ld      (hl), e
        inc     hl
        ld      (hl), d
        call    sf_dirty_store_row$
        ret

sf_ret_edge_delta$:
        ; OLD top_max from the two retained signed pixel endpoints.
'''
if anchor not in sym:
    raise SystemExit('Stage23 delta anchor not found')
sym=sym.replace(anchor,helpers,1)

# Descriptor BSS expands; add lifetime scratch.
sym=sym.replace('sf_prev_desc$:            .ds 100',
                'sf_prev_desc$:            .ds 140',1)
anchor='''sf_ret_style$:            .ds 1
sf_ret_mode$:             .ds 1
'''
repl='''sf_ret_style$:            .ds 1
sf_cov_valid$:            .ds 1
sf_cov_last$:             .ds 1
sf_old_cov_first$:        .ds 1
sf_old_cov_last$:         .ds 1
sf_restore_row$:          .ds 1
sf_restore_last$:         .ds 1
sf_inv_col$:              .ds 1
sf_inv_last$:             .ds 1
sf_ret_mode$:             .ds 1
'''
if anchor not in sym:
    raise SystemExit('Stage23 BSS retained anchor not found')
sym=sym.replace(anchor,repl,1)

# ===========================================================================
# EXCEPTION COVERAGE: stale Stage-18 exception cells must not restore base over
# a CURRENT retained FULL wall. The test is only paid on actual stale bits.
# ===========================================================================
nt=nt.replace('; STAGE18_COLUMN_LIFETIME\n',
              '; STAGE18_COLUMN_LIFETIME\n; STAGE24_EXCEPTION_COVERAGE_ONLY\n',1)
nt=nt.replace(
'''        .globl  _g_ts_dirty_words
''',
'''        .globl  _g_ts_dirty_words
        .globl  _ts_retained_full_owns_cell
''',1)

old='''nte_bit_loop$:
        srl     c
        jr      nc, nte_next_bit$

        ; Restore static base tile for this exact stale cell.
'''
new='''nte_bit_loop$:
        srl     c
        jp      nc, nte_next_bit$

        ; Generic coverage now tracks exceptions only. A stale exception cell
        ; must not erase a CURRENT retained FULL wall occupying the same tile.
        ld      a, (#nte_col$)
        ld      e, a
        ld      a, (#nte_row$)
        call    _ts_retained_full_owns_cell
        or      a
        jp      nz, nte_next_bit$

        ; Restore static base tile for this exact stale exception cell.
'''
if old not in nt:
    raise SystemExit('Stage18 stale bit loop anchor not found')
nt=nt.replace(old,new,1)

core_p.write_text(core)
run_p.write_text(run)
sym_p.write_text(sym)
nt_p.write_text(nt)
print('Applied Stage 24 retained FULL lifetime separation.')
