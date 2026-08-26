#!/usr/bin/env python3
from pathlib import Path

H = 72

def geom(profile, inv):
    # Match the current GG/C pixel equations after reciprocal is quantized to the
    # nearest byte. The quantization error is < 0.25 wall-height pixel because
    # wall half-height is inv/2; final output is integer screen Y anyway.
    top_full = H - (inv // 2)
    bot_full = H + ((inv + 1) // 2)
    if profile == 0:  # FULL
        return top_full, bot_full
    if profile == 2:  # RAISED_FULL: 3/4 of lower half extent
        hq6 = (inv << 5)
        hq6 = hq6 - (hq6 >> 2)
        bot = H + ((hq6 + 32) >> 6)
        return top_full, bot
    if profile == 1:  # LINTEL: snapped bottom boundary above horizon
        hq6 = (inv << 5) >> 1
        topish = H - ((hq6 + 31) >> 6)
        bot = ((topish + 4) & ~7) - 1
        return top_full, bot
    # RISER: snapped top boundary below horizon, full lower extent
    hq6 = (inv << 5)
    hq6 = hq6 - (hq6 >> 2)
    top = (H + ((hq6 + 32) >> 6) + 4) & ~7
    return top, bot_full

asm = r'''        .module tilesector_run_gg
        .area   _CODE

        .globl  _g_name_run_ctx
        .globl  _g_raster_ctx
        .globl  _ts_raster_surface_column_fast

; TSNameRunCtx ABI:
;   +0 c0, +1 c1, +2 profile, +3 signed shade_bias,
;   +4 original_c0, +5 original_c1,
;   +6 inv_q6, +8 step_q6, +10 clip_top*, +12 clip_bottom*.
;
; One call consumes an entire contiguous visible surface run. The old GG path
; rebuilt 13 bytes of raster context and did Q6 profile/edge math in SDCC C for
; every 8px column. Here the reciprocal accumulator, edge carry, aperture
; pointers and profile stay live across the run. The existing hand-written
; column writer remains the final name-table-word kernel for this experiment.

_ts_raster_surface_run_fast::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy

        ld      a, (#_g_name_run_ctx + 0)
        ld      (#nr_col$), a
        ld      a, (#_g_name_run_ctx + 1)
        ld      (#nr_end$), a
        ld      a, (#_g_name_run_ctx + 2)
        ld      (#nr_profile$), a
        ld      (#_g_raster_ctx + 0), a
        ld      a, (#_g_name_run_ctx + 3)
        ld      (#nr_bias$), a
        ld      a, (#_g_name_run_ctx + 4)
        ld      (#nr_orig0$), a
        ld      a, (#_g_name_run_ctx + 5)
        ld      (#nr_orig1$), a
        ld      hl, (#_g_name_run_ctx + 6)
        ld      (#nr_invq$), hl
        ld      hl, (#_g_name_run_ctx + 8)
        ld      (#nr_step$), hl
        ld      hl, (#_g_name_run_ctx + 10)
        ld      (#_g_raster_ctx + 11), hl
        ld      hl, (#_g_name_run_ctx + 12)
        ld      (#_g_raster_ctx + 13), hl
        xor     a
        ld      (#nr_have_carry$), a

        ; geometry table is 1024 bytes/profile: H += profile*4 pages.
        ld      hl, #nr_geom_lut$
        ld      a, (#nr_profile$)
        add     a, a
        add     a, a
        add     a, h
        ld      h, a
        ld      (#nr_geom_base$), hl

nr_loop$:
        ; A closed aperture costs only two pointer reads and resets line carry.
        ld      hl, (#_g_raster_ctx + 11)
        ld      a, (hl)
        ld      c, a
        ld      hl, (#_g_raster_ctx + 13)
        ld      a, (hl)
        cp      c
        jr      nc, nr_visible$
        xor     a
        ld      (#nr_have_carry$), a
        jr      nr_advance$

nr_visible$:
        ; next reciprocal = current + per-column step.
        ld      hl, (#nr_invq$)
        ld      de, (#nr_step$)
        add     hl, de
        ld      (#nr_nextq$), hl

        ; Left ideal geometry. If this run is continuous, the actual left edge
        ; is the exact endpoint emitted by the preceding tile instead.
        ld      hl, (#nr_invq$)
        call    nr_round_inv$
        call    nr_lookup_geom$
        ld      a, (#nr_have_carry$)
        or      a
        jr      nz, nr_use_carry$
        ld      (_g_raster_ctx + 2), bc
        ld      (_g_raster_ctx + 6), de
        jr      nr_left_ready$
nr_use_carry$:
        ld      hl, (#nr_carry_top$)
        ld      (_g_raster_ctx + 2), hl
        ld      hl, (#nr_carry_bot$)
        ld      (_g_raster_ctx + 6), hl
nr_left_ready$:

        ; Right ideal target from the next reciprocal. Clamp the emitted rise to
        ; +/-7 pixels, exactly the precomputed edge vocabulary, and carry that
        ; actual endpoint into the next column.
        ld      hl, (#nr_nextq$)
        call    nr_round_inv$
        call    nr_lookup_geom$        ; BC=top target, DE=bottom target
        ld      (#nr_target_bot$), de

        ld      hl, (#_g_raster_ctx + 2)
        ld      d, b
        ld      e, c
        call    nr_connect$            ; DE = connected top right
        ld      (_g_raster_ctx + 4), de
        ld      (#nr_carry_top$), de

        ld      hl, (#_g_raster_ctx + 6)
        ld      de, (#nr_target_bot$)
        call    nr_connect$
        ld      (_g_raster_ctx + 8), de
        ld      (#nr_carry_bot$), de
        ld      a, #1
        ld      (#nr_have_carry$), a

        ; Exact midpoint reciprocal for distance shade: (left+right)>>7.
        ld      hl, (#nr_invq$)
        ld      de, (#nr_nextq$)
        add     hl, de
        add     hl, hl
        ld      a, h
        cp      #82
        jr      nc, nr_shade2$
        cp      #46
        jr      nc, nr_shade1$
        xor     a
        jr      nr_bias_shade$
nr_shade1$:
        ld      a, #1
        jr      nr_bias_shade$
nr_shade2$:
        ld      a, #2
nr_bias_shade$:
        ld      c, a
        ld      a, (#nr_bias$)
        add     a, c
        bit     7, a
        jr      z, nr_shade_nonneg$
        xor     a
        jr      nr_shade_ready$
nr_shade_nonneg$:
        cp      #3
        jr      c, nr_shade_ready$
        ld      a, #2
nr_shade_ready$:
        ld      (#_g_raster_ctx + 1), a

        ; Endpoint borders are properties of the world segment endpoints, not
        ; per-column state; reconstruct them from the current column.
        xor     a
        ld      (#nr_border$), a
        ld      a, (#nr_col$)
        ld      c, a
        ld      a, (#nr_orig0$)
        cp      c
        jr      nz, nr_border_right$
        ld      a, #1
        ld      (#nr_border$), a
nr_border_right$:
        ld      a, (#nr_orig1$)
        cp      c
        jr      nz, nr_border_done$
        ld      a, (#nr_border$)
        or      #2
        ld      (#nr_border$), a
nr_border_done$:
        ld      a, (#nr_border$)
        ld      (#_g_raster_ctx + 10), a

        ld      a, (#nr_col$)
        call    _ts_raster_surface_column_fast

nr_advance$:
        ; Reciprocal always advances, including through a closed aperture, so a
        ; later reopened column remains geometrically aligned.
        ld      hl, (#nr_invq$)
        ld      de, (#nr_step$)
        add     hl, de
        ld      (#nr_invq$), hl

        ld      hl, (#_g_raster_ctx + 11)
        inc     hl
        ld      (#_g_raster_ctx + 11), hl
        ld      hl, (#_g_raster_ctx + 13)
        inc     hl
        ld      (#_g_raster_ctx + 13), hl

        ld      hl, #nr_col$
        inc     (hl)
        ld      a, (hl)
        ld      c, a
        ld      a, (#nr_end$)
        cp      c
        jp      nc, nr_loop$

        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

; HL = positive Q6 reciprocal. Return nearest integer reciprocal in A without
; six shift iterations: ((HL+32)>>6) = (H<<2) | (L>>6).
nr_round_inv$:
        ld      de, #32
        add     hl, de
        ld      a, l
        rlca
        rlca
        and     #3
        ld      e, a
        ld      a, h
        add     a, a
        add     a, a
        or      e
        ret

; A=0..255 reciprocal. Return BC=top Y, DE=bottom Y from the profile table.
nr_lookup_geom$:
        ld      l, a
        ld      h, #0
        add     hl, hl
        add     hl, hl
        ld      de, (#nr_geom_base$)
        add     hl, de
        ld      c, (hl)
        inc     hl
        ld      b, (hl)
        inc     hl
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ret

; HL=actual left, DE=ideal right. Return DE=connected right where delta is
; clamped to [-7,+7]. This is the tile-scale Bresenham continuity rule.
nr_connect$:
        ex      de, hl                  ; HL=target, DE=left
        or      a
        sbc     hl, de                  ; signed delta
        ld      a, h
        or      a
        jr      nz, nr_conn_negative$
        ld      a, l
        cp      #8
        jr      c, nr_conn_within$
        ld      hl, #7
        add     hl, de
        ex      de, hl
        ret
nr_conn_negative$:
        cp      #0xff
        jr      nz, nr_conn_minus7$
        ld      a, l
        cp      #0xf9                  ; -7
        jr      nc, nr_conn_within$
nr_conn_minus7$:
        ex      de, hl                 ; HL=left (DE contains irrelevant delta)
        ld      de, #-7
        add     hl, de
        ex      de, hl
        ret
nr_conn_within$:
        add     hl, de                  ; left + delta = target
        ex      de, hl
        ret

; Four bytes per reciprocal: signed 16-bit top Y, signed 16-bit bottom Y.
; Four 1024-byte profile pages let the run choose its geometry table once.
nr_geom_lut$:
'''

for profile in range(4):
    asm += f'        ; profile {profile}\n'
    vals=[]
    for inv in range(256):
        top, bot = geom(profile, inv)
        vals.append((top & 0xffff, bot & 0xffff))
    for i in range(0, 256, 4):
        row = vals[i:i+4]
        asm += '        .dw ' + ', '.join(f'0x{t:04X}, 0x{b:04X}' for t,b in row) + '\n'

asm += r'''
        .area   _BSS
nr_col$:          .ds 1
nr_end$:          .ds 1
nr_profile$:      .ds 1
nr_bias$:         .ds 1
nr_orig0$:        .ds 1
nr_orig1$:        .ds 1
nr_border$:       .ds 1
nr_have_carry$:   .ds 1
nr_invq$:         .ds 2
nr_step$:         .ds 2
nr_nextq$:        .ds 2
nr_carry_top$:    .ds 2
nr_carry_bot$:    .ds 2
nr_target_bot$:   .ds 2
nr_geom_base$:    .ds 2
'''

Path('src/tilesector_run_gg.s').write_text(asm)
print('Generated Stage 12 contiguous name-table run rasterizer.')
