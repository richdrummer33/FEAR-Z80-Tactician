#!/usr/bin/env python3
from pathlib import Path

p=Path('src/tilesector_symfull_gg.s')
s=p.read_text()

if 'STAGE23_EDGE_DELTA' in s:
    print('Stage 23 edge delta already applied')
    raise SystemExit(0)
if 'STAGE22_RETAINED_FULL' not in s:
    raise SystemExit('apply Stage 22 retained FULL descriptors first')

s=s.replace('; STAGE22_RETAINED_FULL\n',
            '; STAGE22_RETAINED_FULL\n; STAGE23_EDGE_DELTA\n',1)

# Default each column to the normal/full materializer. Only the proven
# same-style/same-clip geometry-change case selects the delta path.
anchor='''sf_aperture_ready$:

        ; Only the TOP endpoint rows are solved.
'''
repl='''sf_aperture_ready$:
        xor     a
        ld      (#sf_ret_mode$), a

        ; Only the TOP endpoint rows are solved.
'''
if anchor not in s:
    raise SystemExit('aperture-ready anchor not found')
s=s.replace(anchor,repl,1)

old='''sf_ret_edge_only$:
        ld      a, (_g_ts_ret_full_edgeonly)
        inc     a
        ld      (_g_ts_ret_full_edgeonly), a

sf_ret_store_current$:
'''
new='''sf_ret_edge_only$:
        ld      a, (_g_ts_ret_full_edgeonly)
        inc     a
        ld      (_g_ts_ret_full_edgeonly), a

        ; Preserve the OLD quantized edge endpoints before overwriting the
        ; retained descriptor. Style and aperture have already been proven equal.
        ld      a, 0 (ix)
        ld      (#sf_old_top_l$), a
        ld      a, 1 (ix)
        ld      (#sf_old_top_r$), a
        ld      a, #1
        ld      (#sf_ret_mode$), a

sf_ret_store_current$:
'''
if old not in s:
    raise SystemExit('edge-only classification block not found')
s=s.replace(old,new,1)

old='''        ld      hl, (#sf_prev_gen_ptr$)
        ld      a, (#sf_ret_frame$)
        ld      (hl), a

sf_ret_not_eligible$:
        ; top slope = topR-topL; run kernel already constrains it to +/-7.
'''
new='''        ld      hl, (#sf_prev_gen_ptr$)
        ld      a, (#sf_ret_frame$)
        ld      (hl), a

        ld      a, (#sf_ret_mode$)
        or      a
        jp      nz, sf_ret_edge_delta$
        ; The normal/full path MUST be explicit. Later stages insert callable
        ; helpers before sf_ret_edge_delta$; fall-through here would enter a
        ; helper and RET out of the entire column materializer.
        jp      sf_ret_not_eligible$

sf_ret_not_eligible$:
        ; top slope = topR-topL; run kernel already constrains it to +/-7.
'''
if old not in s:
    raise SystemExit('retained descriptor store tail not found')
s=s.replace(old,new,1)

# Insert the delta materializer immediately before the ordinary full path.
anchor='''sf_ret_not_eligible$:
        ; top slope = topR-topL; run kernel already constrains it to +/-7.
'''
delta=r'''sf_ret_edge_delta$:
        ; OLD top_max from the two retained signed pixel endpoints.
        ld      a, (#sf_old_top_l$)
        sra     a
        sra     a
        sra     a
        ld      e, a
        ld      a, (#sf_old_top_r$)
        sra     a
        sra     a
        sra     a
        ld      d, a
        xor     #0x80
        ld      c, a
        ld      a, e
        xor     #0x80
        cp      c
        jp      nc, sf_delta_old_l_max$
        ld      a, d
        jp      sf_delta_old_max_ready$
sf_delta_old_l_max$:
        ld      a, e
sf_delta_old_max_ready$:
        ld      (#sf_old_top_max$), a

        ; NEW slope. Everything below this point consumes the same current
        ; quantized geometry as the Stage-21 full materializer.
        ld      hl, (#_g_raster_ctx + 4)
        ld      de, (#_g_raster_ctx + 2)
        or      a
        sbc     hl, de
        ld      a, l
        ld      (#sf_slope$), a

        ; Always emit the NEW edge tiles (normally one, occasionally two) and
        ; their hardware-mirrored bottom mates.
        ld      a, (#sf_top_max$)
        bit     7, a
        jp      nz, sf_delta_edges_done$
        ld      c, a
        ld      a, (#sf_top_min$)
        bit     7, a
        jp      z, sf_delta_edge_first_nonneg$
        xor     a
sf_delta_edge_first_nonneg$:
        cp      #18
        jp      nc, sf_delta_edges_done$
        ld      (#sf_delta_row$), a
        ld      a, c
        cp      #18
        jp      c, sf_delta_edge_last_ready$
        ld      a, #17
sf_delta_edge_last_ready$:
        ld      (#sf_delta_last$), a

sf_delta_edge_loop$:
        ld      a, (#sf_delta_row$)
        call    sf_top_edge_word$
        ld      a, (#sf_delta_row$)
        call    sf_store_if_clip$

        ld      a, d
        or      #0x0c
        ld      d, a
        ld      a, (#sf_delta_row$)
        ld      c, a
        ld      a, #17
        sub     c
        call    sf_store_if_clip$

        ld      a, (#sf_delta_row$)
        ld      c, a
        ld      a, (#sf_delta_last$)
        cp      c
        jp      z, sf_delta_edges_done$
        ld      a, c
        inc     a
        ld      (#sf_delta_row$), a
        jp      sf_delta_edge_loop$

sf_delta_edges_done$:
        ; If the wall expanded toward the screen edge, rows between the NEW
        ; deepest edge row and the OLD deepest edge row become wall interior.
        ; Only those rows need filling. If the wall contracted, Stage-18 stale
        ; coverage restores the rows that left the wall, so there is no fill walk.
        ld      a, (#sf_top_max$)
        xor     #0x80
        ld      c, a
        ld      a, (#sf_old_top_max$)
        xor     #0x80
        cp      c
        jp      c, sf_delta_done$
        jp      z, sf_delta_done$

        ; A negative OLD max means both old/new edges were above the screen:
        ; the visible aperture was already solid fill.
        ld      a, (#sf_old_top_max$)
        bit     7, a
        jp      nz, sf_delta_done$

        ; Reconcile symmetric ROW PAIRS. Do not pre-clamp the top member
        ; to the aperture: it may be clipped while its mirrored bottom mate is
        ; still visible. sf_store_if_clip$ tests each member independently.
        ; first=max(new_top_max+1,0)
        ld      a, (#sf_top_max$)
        inc     a
        bit     7, a
        jp      z, sf_delta_first_nonneg$
        xor     a
sf_delta_first_nonneg$:
        cp      #18
        jp      nc, sf_delta_done$
        ld      (#sf_delta_row$), a

        ; last=min(old_top_max,8). FULL top endpoints are constructed at
        ; y<=71, so retained fill reconciliation operates on top/bottom pairs.
        ld      a, (#sf_old_top_max$)
        cp      #9
        jp      c, sf_delta_last_screen$
        ld      a, #8
sf_delta_last_screen$:
        ld      c, a
        ld      a, (#sf_delta_row$)
        cp      c
        jp      c, sf_delta_fill_have$
        jp      z, sf_delta_fill_have$
        jp      sf_delta_done$

sf_delta_fill_have$:
        ld      a, c
        ld      (#sf_delta_last$), a

        ; Same full-tile identity as Stage 21; style is already known unchanged.
        ld      a, (#_g_raster_ctx + 1)
        or      a
        jp      z, sf_delta_full_far$
        dec     a
        jp      z, sf_delta_full_mid$
        ld      a, #27
        jp      sf_delta_full_border$
sf_delta_full_mid$:
        ld      a, #15
        jp      sf_delta_full_border$
sf_delta_full_far$:
        ld      a, #3
sf_delta_full_border$:
        ld      e, a
        ld      a, (#_g_raster_ctx + 10)
        add     a, e
        ld      (#sf_full_tile$), a

sf_delta_fill_loop$:
        ld      a, (#sf_full_tile$)
        ld      e, a
        ld      d, #0
        ld      a, (#sf_delta_row$)
        call    sf_store_if_clip$

        ; Symmetric interior row uses the same full pattern; unlike the edge,
        ; no palette/VFLIP attribute is required.
        ld      a, (#sf_delta_row$)
        ld      c, a
        ld      a, #17
        sub     c
        call    sf_store_if_clip$

        ld      a, (#sf_delta_row$)
        ld      c, a
        ld      a, (#sf_delta_last$)
        cp      c
        jp      z, sf_delta_done$
        ld      a, c
        inc     a
        ld      (#sf_delta_row$), a
        jp      sf_delta_fill_loop$

sf_delta_done$:
        ; A delta path did real materialization, so return A=0. The run-level
        ; whole-span exact-skip counter must therefore remain false.
        jp      sf_close$

sf_ret_not_eligible$:
        ; top slope = topR-topL; run kernel already constrains it to +/-7.
'''
if anchor not in s:
    raise SystemExit('full-path anchor not found')
s=s.replace(anchor,delta,1)

# Tiny fixed scratch only; no dynamic run/event machinery.
anchor='''sf_ret_style$:            .ds 1
sf_col$:          .ds 1
'''
repl='''sf_ret_style$:            .ds 1
sf_ret_mode$:             .ds 1
sf_old_top_l$:            .ds 1
sf_old_top_r$:            .ds 1
sf_old_top_max$:          .ds 1
sf_delta_row$:            .ds 1
sf_delta_last$:           .ds 1
sf_col$:          .ds 1
'''
if anchor not in s:
    raise SystemExit('Stage22 retained BSS anchor not found')
s=s.replace(anchor,repl,1)

# Generated-Z80 control-flow guard: the non-delta case must never depend on
# source layout/fall-through. This is deliberately checked after all insertions.
dispatch_guard='''        ld      a, (#sf_ret_mode$)
        or      a
        jp      nz, sf_ret_edge_delta$
        ; The normal/full path MUST be explicit. Later stages insert callable
        ; helpers before sf_ret_edge_delta$; fall-through here would enter a
        ; helper and RET out of the entire column materializer.
        jp      sf_ret_not_eligible$
'''
if dispatch_guard not in s:
    raise SystemExit('Stage23 generated dispatch lost explicit normal-path jump')

p.write_text(s)
print('Applied Stage 23 retained FULL edge-delta materialization.')
