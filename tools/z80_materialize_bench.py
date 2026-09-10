#!/usr/bin/env python3
"""Cycle-exact Z80 COLUMN MATERIALIZER - the last missing kernel.

Closes the seam the code review named: the Z80 kernels stopped at the Q6 depth
ramp `(iq, step)` and the emit kernel started from finished name-table words,
with nothing in between. This is that conversion.

Per column it reproduces `draw_run`'s body plus `draw_edge` x2 and `draw_full`
(`src/tilesector_polar_renderer.c:446-500`):

    invl = clamp((iq+32)>>6, 255)      invr = clamp((iq+step+32)>>6, 255)
    hl = invl>>1                       hr = invr>>1
    tl/tr = 72 - h                     bl/br = 72 + h
    profile adjusts FULL / LINTEL / RAISED / RISER
    draw_edge(tl,tr, bottom=0) ; draw_edge(bl,br, bottom=1)
    draw_full(row_floor(max tl,tr)+1 .. row_floor(min bl,br)-1)

ORACLE
------
`tools/materialize_probe.c` dumps the shipped C's own per-column output. It is
self-checking: its column loop must rebuild `tsp_polar_render`'s ENTIRE name
table for every pose or it aborts. That check passed on 29,824/29,824 poses,
so the dumped rows are the real renderer's behaviour rather than a plausible
re-derivation.

THE ONE ARITHMETIC TRICK
------------------------
`(iq+32)>>6` is an arithmetic shift on a value that reaches ~16,400, so the
"shift left then take H" trick used elsewhere in this project would overflow
16 bits and silently corrupt it. Decompose instead, exactly:

    v>>6  ==  (int8)H * 4  +  (L >> 6)

because 256/64 is exactly 4 and the low byte contributes no carry into it.
Valid for any 16-bit v, arithmetic semantics preserved via the signed H.

SCOPE: this build runs `g_tspf_appearance_mode == 0` (the GG geometry fast
path), where shade is the constant 1. The kernel still takes shade as an
input and computes the tile encodings generally, so it does not bake in that
constant - but the oracle only exercises shade==1, and that limit is stated
rather than hidden.

    make materialize-bench
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80  # noqa: E402

CODE = 0x0000
IQ, STEP, PROFILE, BORDER, SHADE = 0xC000, 0xC002, 0xC004, 0xC005, 0xC006
COLBUF = 0xC100                      # 18 words, pre-filled with background
TL, TR, BL, BR = 0xC010, 0xC012, 0xC014, 0xC016
YL, YR, BOTTOM = 0xC018, 0xC01A, 0xC01C
SLOPE, MAG, R0, R1 = 0xC01D, 0xC01E, 0xC01F, 0xC020
LOCL, ATTR, TMP = 0xC022, 0xC024, 0xC026
HLH, HRH = 0xC028, 0xC029
EDGEBASE, FULLBASE = 0xC02A, 0xC02C   # precomputed shade-dependent bases

HORIZON = 72
ROWS = 18

SRC = f"""
; ---- invl = clamp((iq+32)>>6,255), invr = clamp((iq+step+32)>>6,255) ----
        ld hl,({IQ:#06x})
        ld de,32
        add hl,de
        call shr6_clamp
        srl a                        ; hl_half = invl>>1
        ld ({HLH:#06x}),a
        ld hl,({IQ:#06x})
        ld de,({STEP:#06x})
        add hl,de
        ld de,32
        add hl,de
        call shr6_clamp
        srl a
        ld ({HRH:#06x}),a

; ---- tl/tr/bl/br ----
        ld a,{HORIZON}
        ld b,a
        ld a,({HLH:#06x})
        ld c,a
        ld a,b
        sub c
        call sx_tl                   ; TL = 72 - hl  (sign-extended)
        ld a,{HORIZON}
        ld c,a
        ld a,({HRH:#06x})
        ld b,a
        ld a,c
        sub b
        call sx_tr
        ld a,{HORIZON}
        ld b,a
        ld a,({HLH:#06x})
        add a,b
        ld l,a
        ld h,0
        ld ({BL:#06x}),hl            ; BL = 72 + hl  (always >= 72, positive)
        ld a,{HORIZON}
        ld b,a
        ld a,({HRH:#06x})
        add a,b
        ld l,a
        ld h,0
        ld ({BR:#06x}),hl

; ---- profile ----
        ld a,({PROFILE:#06x})
        or a
        jp nz,pf_not_full
        ld hl,({TL:#06x})
        dec hl
        ld ({TL:#06x}),hl
        ld hl,({TR:#06x})
        dec hl
        ld ({TR:#06x}),hl
        jp pf_done
pf_not_full:
        cp 1
        jp nz,pf_not_lintel
        ld a,({HLH:#06x})
        srl a
        ld b,a
        ld a,{HORIZON}
        sub b
        call sx_bl
        ld a,({HRH:#06x})
        srl a
        ld b,a
        ld a,{HORIZON}
        sub b
        call sx_br
        jp pf_done
pf_not_lintel:
        cp 2
        jp nz,pf_not_raised
        ld a,({HLH:#06x})
        ld b,a
        srl a
        srl a
        ld c,a
        ld a,b
        sub c
        add a,{HORIZON}
        call zx_bl
        ld a,({HRH:#06x})
        ld b,a
        srl a
        srl a
        ld c,a
        ld a,b
        sub c
        add a,{HORIZON}
        call zx_br
        jp pf_done
pf_not_raised:
        cp 3
        jp nz,pf_done
        ld a,({HLH:#06x})
        ld b,a
        srl a
        srl a
        ld c,a
        ld a,b
        sub c
        add a,{HORIZON}
        call zx_tl
        ld a,({HRH:#06x})
        ld b,a
        srl a
        srl a
        ld c,a
        ld a,b
        sub c
        add a,{HORIZON}
        call zx_tr
pf_done:

; ---- shade-dependent tile bases ----
;   EDGE = 39 + shade*128        (off_index and slope added per cell)
;   FULL = 3  + shade*12 + border
        ld a,({SHADE:#06x})
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl                    ; shade*128
        ld de,39
        add hl,de
        ld ({EDGEBASE:#06x}),hl
        ld a,({SHADE:#06x})
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl                    ; *4
        ld d,h
        ld e,l
        add hl,hl                    ; *8
        add hl,de                    ; *12
        ld de,3
        add hl,de
        ld a,({BORDER:#06x})
        ld e,a
        ld d,0
        add hl,de
        ld ({FULLBASE:#06x}),hl

; ---- the two edges and the fill ----
        ld hl,({TL:#06x})
        ld ({YL:#06x}),hl
        ld hl,({TR:#06x})
        ld ({YR:#06x}),hl
        xor a
        ld ({BOTTOM:#06x}),a
        call draw_edge

        ld hl,({BL:#06x})
        ld ({YL:#06x}),hl
        ld hl,({BR:#06x})
        ld ({YR:#06x}),hl
        ld a,1
        ld ({BOTTOM:#06x}),a
        call draw_edge

        call draw_full
        halt

; =========================================================== helpers ====
; carry = (HL < DE) SIGNED. HL and DE preserved.
;
; `sbc hl,de` gives an UNSIGNED comparison. Using its carry directly on
; possibly-negative operands reads -7 as 65529, which is exactly the bug that
; made every edge tile clamp to its maximum here. A7 already solved this for
; the decode-clip kernel - XOR bit 15 of both operands maps two's-complement
; ordering onto unsigned ordering - and this is the same primitive reused
; rather than re-derived.
cmps:
        push hl
        push de
        ld a,h
        xor 0x80
        ld h,a
        ld a,d
        xor 0x80
        ld d,a
        or a
        sbc hl,de
        pop de
        pop hl
        ret
; HL(i16) -> A = clamp(HL>>6, 0, 255).
; v>>6 == (int8)H*4 + (L>>6). Exact for any 16-bit v; the left-shift form
; would overflow here (iq+step+32 reaches ~16,400, and <<2 exceeds 16 bits).
shr6_clamp:
        ld a,l
        rlca
        rlca
        and 3
        ld c,a                       ; C = L>>6
        ld a,h
        ld e,a
        ld d,0
        bit 7,e
        jp z,s6_pos
        ld d,0xff
s6_pos:
        ex de,hl                     ; HL = sign-extended H
        add hl,hl
        add hl,hl                    ; *4
        ld e,c
        ld d,0
        add hl,de
        bit 7,h
        jp z,s6_nonneg
        xor a                        ; negative -> 0
        ret
s6_nonneg:
        ld a,h
        or a
        jp z,s6_fits
        ld a,255                     ; >255 -> 255
        ret
s6_fits:
        ld a,l
        ret

; A(i8) -> sign-extended i16 stored at TL / TR / BL / BR
sx_tl:
        call sx_hl
        ld ({TL:#06x}),hl
        ret
sx_tr:
        call sx_hl
        ld ({TR:#06x}),hl
        ret
sx_bl:
        call sx_hl
        ld ({BL:#06x}),hl
        ret
sx_br:
        call sx_hl
        ld ({BR:#06x}),hl
        ret
sx_hl:
        ld l,a
        ld h,0
        bit 7,l
        ret z
        ld h,0xff
        ret

; ZERO-extending stores. RAISED's bl = 72+h-(h>>2) and RISER's tl reach 168,
; so sign-extending them turns every value over 127 negative - which is what
; blanked every RAISED column. Only `72 - h` can actually go negative.
zx_bl:
        ld l,a
        ld h,0
        ld ({BL:#06x}),hl
        ret
zx_br:
        ld l,a
        ld h,0
        ld ({BR:#06x}),hl
        ret
zx_tl:
        ld l,a
        ld h,0
        ld ({TL:#06x}),hl
        ret
zx_tr:
        ld l,a
        ld h,0
        ld ({TR:#06x}),hl
        ret

; HL(i16) -> A = row_floor(HL) = HL>=0 ? HL>>3 : -(((-HL)+7)>>3)
rowfloor:
        bit 7,h
        jp z,rf_pos
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a                       ; HL = -HL
        ld de,7
        add hl,de
        call shr3_u
        neg
        ret
rf_pos:
        call shr3_u
        ret
shr3_u:
        srl h
        rr l
        srl h
        rr l
        srl h
        rr l
        ld a,l
        ret

; ---- draw_edge: YL, YR, BOTTOM ----
draw_edge:
        ld hl,({YR:#06x})
        ld de,({YL:#06x})
        or a
        sbc hl,de                    ; HL = yr - yl (exact, sign irrelevant here)
        ld de,0xfff9                 ; -7
        call cmps
        jp nc,de_sl_lo
        ld hl,0xfff9
de_sl_lo:
        ld de,8                      ; > 7  <=>  < 8 is false
        call cmps
        jp c,de_sl_hi
        ld hl,7
de_sl_hi:
        ld a,l
        ld ({SLOPE:#06x}),a
de_rows2:
; r0 = row_floor(min(yl,yr)), r1 = row_floor(max(yl,yr))
        ld hl,({YL:#06x})
        ld de,({YR:#06x})
        call cmps
        jp c,de_yl_min
        ld hl,({YR:#06x})
de_yl_min:
        call rowfloor
        ld ({R0:#06x}),a
        ld hl,({YL:#06x})
        ld de,({YR:#06x})
        call cmps
        jp nc,de_yl_max
        ld hl,({YR:#06x})
de_yl_max:
        call rowfloor
        ld ({R1:#06x}),a
        ld a,({R0:#06x})
        bit 7,a
        jp z,de_r0ok
        xor a
        ld ({R0:#06x}),a
de_r0ok:
        ld a,({R1:#06x})
        bit 7,a
        jp nz,de_r1neg
        cp {ROWS}
        jp c,de_r1ok
        ld a,{ROWS - 1}
        ld ({R1:#06x}),a
        jp de_r1ok
de_r1neg:
        ret                          ; r1 < 0 -> nothing to draw
de_r1ok:
        ld a,({R0:#06x})
        ld b,a
        ld a,({R1:#06x})
        cp b
        ret c                        ; r1 < r0 -> nothing
de_loop:
; local_left = YL - (r<<3)
        ld a,({R0:#06x})
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ex de,hl
        ld hl,({YL:#06x})
        or a
        sbc hl,de
        ld ({LOCL:#06x}),hl
        call edge_entry
        ld a,({R0:#06x})
        ld l,a
        ld h,0
        add hl,hl
        ld de,{COLBUF:#06x}
        add hl,de
        ld a,({TMP:#06x})
        ld (hl),a
        inc hl
        ld a,(0x{TMP + 1:04X})
        ld (hl),a
        ld a,({R0:#06x})
        ld b,a
        ld a,({R1:#06x})
        cp b
        ret z
        ld a,b
        inc a
        ld ({R0:#06x}),a
        jp de_loop

; ---- edge_entry: LOCL, SLOPE, BOTTOM, EDGEBASE -> TMP (word) ----
edge_entry:
        ld hl,0
        ld ({ATTR:#06x}),hl
        ld a,({SLOPE:#06x})
        ld c,a                       ; C = slope (i8)
        ld a,({BOTTOM:#06x})
        or a
        jp z,ee_top
        ld hl,7
        ld de,({LOCL:#06x})
        or a
        sbc hl,de
        ld ({LOCL:#06x}),hl          ; local_left = 7 - local_left
        ld a,c
        neg
        ld c,a                       ; slope = -slope
        ld hl,0x0c00                 ; FLIPY | PALETTE
        ld ({ATTR:#06x}),hl
ee_top:
        ld a,c
        bit 7,a
        jp z,ee_pos
        neg
        ld ({MAG:#06x}),a
        ld e,a
        ld d,0
        ld hl,({LOCL:#06x})
        or a
        sbc hl,de
        ld ({LOCL:#06x}),hl          ; local_left -= mag
        ld hl,({ATTR:#06x})
        ld de,0x0200                 ; FLIPX
        add hl,de
        ld ({ATTR:#06x}),hl
        jp ee_mag_ok
ee_pos:
        ld ({MAG:#06x}),a
ee_mag_ok:
        ld a,({MAG:#06x})
        cp 8
        jp c,ee_mag2
        ld a,7
        ld ({MAG:#06x}),a
ee_mag2:
; off = clamp(local_left, -7, 8) ; off_index = off + 7
        ld hl,({LOCL:#06x})
        ld de,0xfff9                 ; -7
        call cmps
        jp nc,ee_lo_ok
        ld hl,0xfff9                 ; local_left < -7
ee_lo_ok:
        ld de,9                      ; > 8  <=>  < 9 is false
        call cmps
        jp c,ee_hi_ok
        ld hl,8
ee_hi_ok:
        ld de,7
        add hl,de                    ; off_index 0..15
        add hl,hl
        add hl,hl
        add hl,hl                    ; *8
        ld de,({EDGEBASE:#06x})
        add hl,de
        ld a,({MAG:#06x})
        ld e,a
        ld d,0
        add hl,de
        ld de,({ATTR:#06x})
        add hl,de
        ld ({TMP:#06x}),hl
        ret

; ---- draw_full: rows row_floor(max tl,tr)+1 .. row_floor(min bl,br)-1 ----
draw_full:
        ld hl,({TL:#06x})
        ld de,({TR:#06x})
        call cmps
        jp nc,df_tmax
        ld hl,({TR:#06x})
df_tmax:
        call rowfloor
        inc a
        ld ({R0:#06x}),a
        ld hl,({BL:#06x})
        ld de,({BR:#06x})
        call cmps
        jp c,df_bmin
        ld hl,({BR:#06x})
df_bmin:
        call rowfloor
        dec a
        ld ({R1:#06x}),a
        ld a,({R0:#06x})
        bit 7,a
        jp z,df_r0ok
        xor a
        ld ({R0:#06x}),a
df_r0ok:
        ld a,({R1:#06x})
        bit 7,a
        ret nz
        cp {ROWS}
        jp c,df_r1ok
        ld a,{ROWS - 1}
        ld ({R1:#06x}),a
df_r1ok:
        ld a,({R0:#06x})
        ld b,a
        ld a,({R1:#06x})
        cp b
        ret c
df_loop:
        ld a,({R0:#06x})
        ld l,a
        ld h,0
        add hl,hl
        ld de,{COLBUF:#06x}
        add hl,de
        ld a,({FULLBASE:#06x})
        ld (hl),a
        inc hl
        ld a,(0x{FULLBASE + 1:04X})
        ld (hl),a
        ld a,({R0:#06x})
        ld b,a
        ld a,({R1:#06x})
        cp b
        ret z
        ld a,b
        inc a
        ld ({R0:#06x}),a
        jp df_loop
"""


def background():
    """map_init's per-row background: ceiling / horizon / floor."""
    out = []
    for r in range(ROWS):
        out.append(0 if r < 9 else (2 if r == 9 else 1))
    return out


def main():
    dump = ROOT / "build" / "materialize_oracle.txt"
    if not dump.exists():
        raise SystemExit(f"missing {dump} - run `make materialize-bench`")
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 15000
    lines = [l.split() for l in dump.read_text().splitlines() if l.strip()]
    step_n = max(1, len(lines) // limit)
    cases = lines[::step_n]

    code, _ = assemble(SRC, CODE)
    print(f"=== Z80 COLUMN MATERIALIZER, {len(code)} bytes ===")
    print(f"oracle cases: {len(cases)} columns strided across {len(lines)}, "
          f"from the shipped draw_run/draw_edge/draw_full\n")

    bg = background()
    base = bytearray(0x10000)
    base[CODE:CODE + len(code)] = code

    fails = 0
    ts = []
    for row in cases:
        iq, stp, prof, bor, sh = (int(v) for v in row[:5])
        want = [int(v) for v in row[5:5 + ROWS]]
        mem = bytearray(base)
        mem[IQ] = iq & 0xFF; mem[IQ + 1] = (iq >> 8) & 0xFF
        mem[STEP] = stp & 0xFF; mem[STEP + 1] = (stp >> 8) & 0xFF
        mem[PROFILE], mem[BORDER], mem[SHADE] = prof, bor, sh
        for r, v in enumerate(bg):
            mem[COLBUF + 2 * r] = v & 0xFF
            mem[COLBUF + 2 * r + 1] = v >> 8
        cpu = Z80(mem)
        cpu.run(CODE)
        got = [cpu.m[COLBUF + 2 * r] | (cpu.m[COLBUF + 2 * r + 1] << 8)
               for r in range(ROWS)]
        ts.append(cpu.t)
        if got != want:
            fails += 1
            if fails <= 4:
                print(f"  MISMATCH iq={iq} step={stp} profile={prof} "
                      f"border={bor} shade={sh}")
                print(f"    want {want}")
                print(f"    got  {got}")
    print(f"VERIFIED: {len(cases) - fails}/{len(cases)} columns exact")
    if fails:
        raise SystemExit(f"FAIL: {fails} mismatches")

    mean_t = stt.mean(ts)
    print(f"\nT-states per COLUMN: mean={mean_t:.1f} min={min(ts)} max={max(ts)}")
    # Columns MATERIALIZED per update, not columns covered on screen: runs
    # overlap, so a screen column is materialized once per run that spans it.
    # 873,084 column-materializations over 29,824 poses (materialize_probe).
    cols = 873084.0 / 29824.0
    line = mean_t * cols
    print(f"\ncolumns materialized per update: {cols:.2f}")
    print(f"  (NOT the 20.00 covered-columns figure - runs overlap, and each")
    print(f"   overlapping run materializes the column again)")
    print(f"\n  materialize   {line:,.0f} T/update")
    print(f"\nThis REPLACES the emit line rather than adding to it. The emit")
    print(f"bench measured 21,756 T reconstructing a name table from finished")
    print(f"run words - but those words are pose-dependent, so the runtime")
    print(f"cannot be handed them. Producing them is this kernel's job, and")
    print(f"that is what it costs.")
    bearing, decode_clip, gate, colsolve, sort = 5833.0, 11036.0, 2339.0, 26820.0, 1314.0
    tot = bearing + decode_clip + gate + colsolve + sort + line
    print(f"\n=== WHOLE-UPDATE BUDGET ===")
    for nm, v in (("bearing lookup", bearing), ("decode-clip", decode_clip),
                  ("GATE", gate), ("column-solve", colsolve),
                  ("depth sort", sort), ("MATERIALIZE", line)):
        print(f"  {nm:16s} {v:10,.0f} T   {v/tot:6.1%}")
    print(f"  {'TOTAL':16s} {tot:10,.0f} T   {59736.0/tot:.2f} updates/frame")
    print(f"\nUNTUNED. This is the first kernel written purely for correctness")
    print(f"with no optimisation pass: every operand goes through memory, the")
    print(f"signed compare is a CALL, and edge_entry re-reads its inputs per")
    print(f"row. At {mean_t/18:.0f} T per name-table word against emit's measured")
    print(f"60.43, there is a lot of headroom - but per standing direction the")
    print(f"number is recorded as measured, not as hoped.")


if __name__ == "__main__":
    main()
