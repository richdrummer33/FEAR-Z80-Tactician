#!/usr/bin/env python3
from pathlib import Path
import re

core_p=Path('src/tilesector_core.c')
run_p=Path('src/tilesector_run_gg.s')
sym_p=Path('src/tilesector_symfull_gg.s')

core=core_p.read_text()
run=run_p.read_text()
sym=sym_p.read_text()

if 'STAGE22_RETAINED_FULL' in sym:
    print('Stage 22 retained FULL state already applied')
    raise SystemExit(0)
if 'STAGE21_FULL_SYMMETRY' not in sym or 'STAGE21_FULL_SYMMETRY' not in run:
    raise SystemExit('apply Stage 21 symmetry first')

# ---------------------------------------------------------------------------
# Core: expose one retain-eligibility byte in the run descriptor. For the first
# experiment only depth-0 ordinary FULL walls are retained; portal-child FULL
# walls stay on the exact Stage-21 path until ordering/reuse is proven.
# ---------------------------------------------------------------------------
old='''    uint8_t *clip_top;
    uint8_t *clip_bottom;
} TSNameRunCtx;'''
new='''    uint8_t *clip_top;
    uint8_t *clip_bottom;
    uint8_t retain_ok;
} TSNameRunCtx;'''
if old not in core:
    raise SystemExit('TSNameRunCtx tail not found')
core=core.replace(old,new,1)

old='''void ts_candidate_span_lite_fast(void);
void ts_raster_surface_run_fast(void);
#endif
'''
new='''void ts_candidate_span_lite_fast(void);
void ts_raster_surface_run_fast(void);
void ts_retained_full_begin_frame(void);
#endif
'''
if old not in core:
    raise SystemExit('Stage12 SDCC declaration block not found')
core=core.replace(old,new,1)

old='''        g_name_run_ctx.clip_top=&g_clip_top[depth][run_c0];
        g_name_run_ctx.clip_bottom=&g_clip_bottom[depth][run_c0];
        ts_raster_surface_run_fast();
'''
new='''        g_name_run_ctx.clip_top=&g_clip_top[depth][run_c0];
        g_name_run_ctx.clip_bottom=&g_clip_bottom[depth][run_c0];
        g_name_run_ctx.retain_ok=(uint8_t)(depth==0u);
        ts_raster_surface_run_fast();
'''
if old not in core:
    raise SystemExit('GG solid run assignment block not found')
core=core.replace(old,new,1)

old='''    g_name_run_ctx.clip_top=&g_clip_top[depth][(uint8_t)c0];
    g_name_run_ctx.clip_bottom=&g_clip_bottom[depth][(uint8_t)c0];
    ts_raster_surface_run_fast();
'''
new='''    g_name_run_ctx.clip_top=&g_clip_top[depth][(uint8_t)c0];
    g_name_run_ctx.clip_bottom=&g_clip_bottom[depth][(uint8_t)c0];
    g_name_run_ctx.retain_ok=0u;
    ts_raster_surface_run_fast();
'''
if old not in core:
    raise SystemExit('GG portal run assignment block not found')
core=core.replace(old,new,1)

old='''void ts_build_tilemap(const TSState *s, uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {
    uint8_t sector;
    g_ts_render_stage=1u;
'''
new='''void ts_build_tilemap(const TSState *s, uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {
    uint8_t sector;
#ifdef __SDCC
    ts_retained_full_begin_frame();
#endif
    g_ts_render_stage=1u;
'''
if old not in core:
    raise SystemExit('ts_build_tilemap entry not found')
core=core.replace(old,new,1)

# ---------------------------------------------------------------------------
# Run kernel: count depth-0 FULL spans, and treat A=1 from the Stage22
# materializer as "this column required zero tile materialization".
# ---------------------------------------------------------------------------
run=run.replace(
'''        .globl  _ts_raster_symfull_column_fast
''',
'''        .globl  _ts_raster_symfull_column_fast
        .globl  _g_ts_ret_span_total
        .globl  _g_ts_ret_span_skip
''',1)

old='''        ld      a, (#_g_name_run_ctx + 2)
        ld      (#nr_profile$), a
        ld      (#_g_raster_ctx + 0), a
'''
new='''        ld      a, (#_g_name_run_ctx + 2)
        ld      (#nr_profile$), a
        ld      (#_g_raster_ctx + 0), a

        ; A whole contiguous run is the natural retained-span unit. Only
        ; depth-0 FULL runs participate in this first correctness experiment.
        xor     a
        ld      (#nr_ret_span_all$), a
        ld      a, (#nr_profile$)
        or      a
        jp      nz, nr_ret_span_ready$
        ld      a, (#_g_name_run_ctx + 14)
        or      a
        jp      z, nr_ret_span_ready$
        ld      a, #1
        ld      (#nr_ret_span_all$), a
        ld      a, (_g_ts_ret_span_total)
        inc     a
        ld      (_g_ts_ret_span_total), a
nr_ret_span_ready$:
'''
if old not in run:
    raise SystemExit('run profile load block not found')
run=run.replace(old,new,1)

old='''nr_symmetric_full_column$:
        ld      a, (#nr_col$)
        call    _ts_raster_symfull_column_fast
        jp      nr_column_done$
'''
new='''nr_symmetric_full_column$:
        ld      a, (#nr_col$)
        call    _ts_raster_symfull_column_fast
        or      a
        jp      nz, nr_column_done$
        ; Any materialized column means the containing retained span was not an
        ; exact whole-span hit this frame.
        xor     a
        ld      (#nr_ret_span_all$), a
        jp      nr_column_done$
'''
if old not in run:
    raise SystemExit('symfull call site not found')
run=run.replace(old,new,1)

old='''        ld      a, (#nr_end$)
        cp      c
        jp      nc, nr_loop$

        pop     iy
'''
new='''        ld      a, (#nr_end$)
        cp      c
        jp      nc, nr_loop$

        ld      a, (#nr_ret_span_all$)
        or      a
        jp      z, nr_ret_span_done$
        ld      a, (_g_ts_ret_span_skip)
        inc     a
        ld      (_g_ts_ret_span_skip), a
nr_ret_span_done$:

        pop     iy
'''
if old not in run:
    raise SystemExit('run epilogue loop boundary not found')
run=run.replace(old,new,1)

if 'nr_ret_span_all$:' not in run:
    bss_anchor='        .area   _BSS\\n'
    if bss_anchor not in run:
        raise SystemExit('run BSS area not found')
    run=run.replace(bss_anchor,bss_anchor+'nr_ret_span_all$:  .ds 1\\n',1)
run=run.replace('; STAGE21_FULL_SYMMETRY\n',
                '; STAGE21_FULL_SYMMETRY\n; STAGE22_RETAINED_FULL\n',1)

# ---------------------------------------------------------------------------
# Symmetric FULL materializer: compare the FINAL quantized inputs that actually
# determine its name-table output. Descriptor per eligible column:
#   +0 top_l, +1 top_r, +2 (shade | border<<2), +3 clip_first, +4 clip_last.
# Generation is separate and prevents false reuse after a column disappears.
# ---------------------------------------------------------------------------
sym=sym.replace(
'''        .globl  _g_raster_ctx
''',
'''        .globl  _g_raster_ctx
        .globl  _g_name_run_ctx
''',1)

sym=sym.replace(
'''; STAGE21_FULL_SYMMETRY
''',
'''; STAGE21_FULL_SYMMETRY
; STAGE22_RETAINED_FULL
''',1)

# Insert begin-frame function before the raster entry.
anchor='_ts_raster_symfull_column_fast::\n'
if anchor not in sym:
    raise SystemExit('symfull entry not found')
begin=r'''_ts_retained_full_begin_frame::
        push    af
        push    bc
        push    de
        push    hl

        ; Per-frame coherence counters consumed by the external profiler.
        xor     a
        ld      (_g_ts_ret_full_total), a
        ld      (_g_ts_ret_full_skip), a
        ld      (_g_ts_ret_full_edgeonly), a
        ld      (_g_ts_ret_span_total), a
        ld      (_g_ts_ret_span_skip), a

        ; Generation 0xFF is permanently reserved as INVALID. On first use and
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
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

''' + anchor
sym=sym.replace(anchor,begin,1)

# Retention check occurs after Stage18 coverage is marked, so correctness/lifetime
# remains identical to Stage21. A hit skips edge LUT, edge stores and all fill
# comparisons but still pays the transitional lifetime mechanism.
anchor='''sf_cov_done$:

        ; top slope = topR-topL; run kernel already constrains it to +/-7.
'''
if anchor not in sym:
    raise SystemExit('symfull coverage boundary not found')
retain=r'''sf_cov_done$:

        ; Retention is deliberately restricted to ordinary depth-0 FULL runs.
        ld      a, (#_g_name_run_ctx + 14)
        or      a
        jp      z, sf_ret_not_eligible$

        ld      a, (_g_ts_ret_full_total)
        inc     a
        ld      (_g_ts_ret_full_total), a

        ; IX = five-byte previous final-quantized descriptor for this column.
        ld      a, (#sf_col$)
        ld      e, a
        add     a, a                   ; 2x
        add     a, a                   ; 4x
        add     a, e                   ; 5x
        ld      e, a
        ld      d, #0
        ld      ix, #sf_prev_desc$
        add     ix, de

        ; HL = previous generation byte for this column.
        ld      a, (#sf_col$)
        ld      e, a
        ld      d, #0
        ld      hl, #sf_prev_gen$
        add     hl, de
        ld      (#sf_prev_gen_ptr$), hl

        ; style = shade | border<<2.
        ld      a, (#_g_raster_ctx + 10)
        add     a, a
        add     a, a
        ld      c, a
        ld      a, (#_g_raster_ctx + 1)
        or      c
        ld      (#sf_ret_style$), a

        ; Only a descriptor written on the immediately preceding frame may hit.
        ld      a, (#sf_ret_frame$)
        dec     a
        ld      c, a
        ld      hl, (#sf_prev_gen_ptr$)
        ld      a, (hl)
        cp      c
        jp      nz, sf_ret_store_current$

        ; First classify non-geometric state. If these differ, materialization
        ; may alter fills/clip and is intentionally not treated as edge-local.
        ld      a, 2 (ix)
        ld      c, a
        ld      a, (#sf_ret_style$)
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

        ; Geometry is already quantized to signed integer pixel endpoints.
        ld      a, 0 (ix)
        ld      c, a
        ld      a, (#_g_raster_ctx + 2)
        cp      c
        jp      nz, sf_ret_edge_only$
        ld      a, 1 (ix)
        ld      c, a
        ld      a, (#_g_raster_ctx + 4)
        cp      c
        jp      nz, sf_ret_edge_only$

        ; Exact final hardware descriptor hit: refresh generation, skip all
        ; tile materialization, and merely close the already-accounted aperture.
        ld      hl, (#sf_prev_gen_ptr$)
        ld      a, (#sf_ret_frame$)
        ld      (hl), a
        ld      a, (_g_ts_ret_full_skip)
        inc     a
        ld      (_g_ts_ret_full_skip), a
        jp      sf_skip_close$

sf_ret_edge_only$:
        ld      a, (_g_ts_ret_full_edgeonly)
        inc     a
        ld      (_g_ts_ret_full_edgeonly), a

sf_ret_store_current$:
        ld      a, (#_g_raster_ctx + 2)
        ld      0 (ix), a
        ld      a, (#_g_raster_ctx + 4)
        ld      1 (ix), a
        ld      a, (#sf_ret_style$)
        ld      2 (ix), a
        ld      a, (#sf_clip_first$)
        ld      3 (ix), a
        ld      a, (#sf_clip_last$)
        ld      4 (ix), a
        ld      hl, (#sf_prev_gen_ptr$)
        ld      a, (#sf_ret_frame$)
        ld      (hl), a

sf_ret_not_eligible$:
        ; top slope = topR-topL; run kernel already constrains it to +/-7.
'''
sym=sym.replace(anchor,retain,1)

# Make normal close report A=0, retained skip close report A=1.
old='''sf_close$:
        ; FULL is opaque: farther portal traversal cannot contribute here.
        ld      hl, (#_g_raster_ctx + 11)
        ld      (hl), #1
        ld      hl, (#_g_raster_ctx + 13)
        ld      (hl), #0
        ret
'''
new='''sf_skip_close$:
        ld      hl, (#_g_raster_ctx + 11)
        ld      (hl), #1
        ld      hl, (#_g_raster_ctx + 13)
        ld      (hl), #0
        ld      a, #1
        ret

sf_close$:
        ; FULL is opaque: farther portal traversal cannot contribute here.
        ld      hl, (#_g_raster_ctx + 11)
        ld      (hl), #1
        ld      hl, (#_g_raster_ctx + 13)
        ld      (hl), #0
        xor     a
        ret
'''
if old not in sym:
    raise SystemExit('symfull close routine not found')
sym=sym.replace(old,new,1)

# Export counters + retained state.
old='''        .area _BSS
sf_col$:          .ds 1
'''
new='''        .area _BSS
_g_ts_ret_full_total::    .ds 1
_g_ts_ret_full_skip::     .ds 1
_g_ts_ret_full_edgeonly:: .ds 1
_g_ts_ret_span_total::    .ds 1
_g_ts_ret_span_skip::     .ds 1
sf_ret_frame$:            .ds 1
sf_prev_desc$:            .ds 100
sf_prev_gen$:             .ds 20
sf_prev_gen_ptr$:         .ds 2
sf_ret_style$:            .ds 1
sf_col$:          .ds 1
'''
if old not in sym:
    raise SystemExit('symfull BSS anchor not found')
sym=sym.replace(old,new,1)

core_p.write_text(core)
run_p.write_text(run)
sym_p.write_text(sym)
print('Applied Stage 22 retained quantized depth-0 FULL descriptors.')
