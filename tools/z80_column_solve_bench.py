#!/usr/bin/env python3
"""Cycle-exact COLUMN-SOLVE kernel on real Z80, verified against the shipped C.

This is the last unbuilt line of the span-interpreter budget. Every other
stage is measured; column-solve has been carried as a projection since
`column_solve_workload.py` counted its operations, and this replaces that
projection with a measurement.

WHAT IT COMPUTES
----------------
Given a span that survived the yaw clip, produce the inverse-depth endpoints
the Q6 ramp is built from (`src/tilesector_polar_renderer.c`):

    dq4  = wall_d_q4(sid, seg_anchor[sid], camera)
    invd = inv_for_dq4(dq4)
    inv0 = inv_at_invd(sid, invd, (yawq+lo) & 4095, lo)
    inv1 = inv_at_invd(sid, invd, (yawq+hi) & 4095, hi)

THE ORACLE IS THE SHIPPED C, NOT A PORT OF IT
----------------------------------------------
`tools/column_solve_probe.c` includes `tilesector_polar_renderer.c` directly,
which makes its `static` internals callable without modifying a line of
shipped source, and dumps what the real code computes on real poses. It also
self-checks: the `lo`/`hi` it re-derives must reproduce the `inv0`/`inv1` that
the real `project_key` returned, or it aborts. That check passed on all
215,292 visible spans.

NOTE ON THE CORPUS SIZE. The probe walks all 71 keys per pose, where the real
runtime walks only the keys in the current cell's block. So its row count is a
CORRECTNESS CORPUS, deliberately broader than the runtime's own workload - do
NOT divide 215,292 by the pose count and read that as spans/update. The
workload number is 4.30 visible spans/update, from `span_decode_workload.py`,
which respects the block structure.

    make column-solve-bench
"""
from __future__ import annotations
import collections
import pathlib
import re
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80  # noqa: E402

GEN = ROOT / "src" / "generated"

# --- page-aligned LUTs (A5's recommendation, applied) ---------------------
UP_LO, UP_HI = 0x2000, 0x2200      # quarter-square byte planes, 512 B each
UP_LO_PAGE = UP_LO >> 8
SIN = 0x2400                        # int8[256]  - exactly one page
SEC = 0x2500                        # uint8[513] - 9-bit index, 16-bit add
INVZ = 0x2800                       # uint8[128]
NX, NY, ANCHOR, VX, VY = 0x2900, 0x2A00, 0x2B00, 0x2C00, 0x2D00

# --- I/O ------------------------------------------------------------------
SID = 0xD000
XQ4, YQ4, YAWQ, LO, HI = 0xD002, 0xD004, 0xD006, 0xD008, 0xD00A
INVD, INV0, INV1 = 0xD010, 0xD011, 0xD012
# --- scratch --------------------------------------------------------------
DQ4 = 0xD020
MA, MB, MPROD, MSIGN = 0xD022, 0xD023, 0xD024, 0xD026
REL, BEARING, OUTB = 0xD028, 0xD02A, 0xD02C
SNX, SNY, ANCHV = 0xD02E, 0xD02F, 0xD030

CODE = 0x0000


def arr(text, name):
    m = re.search(r"static\s+const\s+[^;=]+?\b" + re.escape(name)
                  + r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise SystemExit("missing generated array " + name)
    return [int(x, 0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+", m.group(1))]


def load_tables():
    text = "\n".join(p.read_text() for p in sorted(GEN.glob("tilesector_polar_data_part*.inc")))
    return {n: arr(text, "k_tspf_" + n) for n in
            ("nx_q5", "ny_q5", "seg_anchor", "vx", "vy", "invz", "sin_q7", "sec_q7")}


# ==========================================================================
# The kernel.
#
# Structured as subroutines because the multiply is called up to six times
# per span and inv_at_invd twice; `call`/`ret` at 17/10 T is far cheaper than
# six inlined copies of a 44-byte primitive, and it keeps the thing readable
# enough to audit against the C line by line.
# ==========================================================================
SRC = f"""
        call wall_d
        call inv_for

        ld hl,({LO:#06x})
        ld ({REL:#06x}),hl
        ld de,({YAWQ:#06x})
        add hl,de
        ld a,h
        and 0x0f
        ld h,a
        ld ({BEARING:#06x}),hl
        call inv_at
        ld a,({OUTB:#06x})
        ld ({INV0:#06x}),a

        ld hl,({HI:#06x})
        ld ({REL:#06x}),hl
        ld de,({YAWQ:#06x})
        add hl,de
        ld a,h
        and 0x0f
        ld h,a
        ld ({BEARING:#06x}),hl
        call inv_at
        ld a,({OUTB:#06x})
        ld ({INV1:#06x}),a
        halt

; ---------------------------------------------------------------- UMUL ----
; MA (u8) * MB (u8) -> MPROD (u16), via the A13 quarter-square byte planes.
umul:
        ld a,({MB:#06x})
        ld c,a
        ld a,({MA:#06x})
        add a,c
        ld l,a
        ld a,{UP_LO_PAGE:#04x}
        adc a,0
        ld h,a
        ld e,(hl)
        inc h
        inc h
        ld d,(hl)
        ld a,({MA:#06x})
        sub c
        jp nc,um_dok
        neg
um_dok:
        ld l,a
        ld h,{UP_LO_PAGE:#04x}
        ld c,(hl)
        inc h
        inc h
        ld a,(hl)
        ld h,a
        ld l,c
        ex de,hl
        or a
        sbc hl,de
        ld ({MPROD:#06x}),hl
        ret

; ---------------------------------------------------------------- SMUL ----
; MA (i8) * MB (i8) -> MPROD (i16). Magnitudes go through UMUL; the sign is
; the XOR of the operand signs, applied once at the end.
smul:
        ld a,({MA:#06x})
        ld c,0
        bit 7,a
        jp z,sm_a_pos
        neg
        ld c,1
sm_a_pos:
        ld ({MA:#06x}),a
        ld a,({MB:#06x})
        bit 7,a
        jp z,sm_b_pos
        neg
        ld b,a
        ld a,c
        xor 1
        ld c,a
        ld a,b
sm_b_pos:
        ld ({MB:#06x}),a
        ld a,c
        ld ({MSIGN:#06x}),a
        call umul
        ld a,({MSIGN:#06x})
        or a
        ret z
        ld hl,({MPROD:#06x})
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
        ld ({MPROD:#06x}),hl
        ret

; -------------------------------------------------------------- wall_d ----
; dq4 = wall_d_q4(sid, seg_anchor[sid], camera)  ->  DQ4 (i16)
wall_d:
        ld a,({SID:#06x})
        ld l,a
        ld h,{NX >> 8:#04x}
        ld a,(hl)
        ld ({SNX:#06x}),a
        ld a,({SID:#06x})
        ld l,a
        ld h,{NY >> 8:#04x}
        ld a,(hl)
        ld ({SNY:#06x}),a
        ld a,({SID:#06x})
        ld l,a
        ld h,{ANCHOR >> 8:#04x}
        ld a,(hl)
        ld ({ANCHV:#06x}),a          ; anchor vid - in MEMORY, not C: the
                                     ; multiply subroutines clobber C

; cardinal X: ny == 0 and |nx| == 32
        ld a,({SNY:#06x})
        or a
        jp nz,wd_try_y
        ld a,({SNX:#06x})
        cp 32
        jp z,wd_cx
        cp 0xe0
        jp nz,wd_general
wd_cx:
        ld a,({ANCHV:#06x})
        ld l,a
        ld h,{VX >> 8:#04x}
        ld a,(hl)
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl                    ; HL = vx[anchor] << 4
        ld de,({XQ4:#06x})
        ld a,({SNX:#06x})
        bit 7,a
        jp nz,wd_cx_neg
        or a
        sbc hl,de                    ; nx > 0: wall - x
        jp wd_store
wd_cx_neg:
        ex de,hl
        or a
        sbc hl,de                    ; nx < 0: x - wall
        jp wd_store

wd_try_y:
        ld a,({SNX:#06x})
        or a
        jp nz,wd_general
        ld a,({SNY:#06x})
        cp 32
        jp z,wd_cy
        cp 0xe0
        jp nz,wd_general
wd_cy:
        ld a,({ANCHV:#06x})
        ld l,a
        ld h,{VY >> 8:#04x}
        ld a,(hl)
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,({YQ4:#06x})
        ld a,({SNY:#06x})
        bit 7,a
        jp nz,wd_cy_neg
        or a
        sbc hl,de
        jp wd_store
wd_cy_neg:
        ex de,hl
        or a
        sbc hl,de
wd_store:
        ld ({DQ4:#06x}),hl
        ret

; general: whole = nx*dx + ny*dy ; frac = nx*fx + ny*fy
;          dq4   = (whole >>> 1) - (frac >>> 5)      [>>> = toward zero]
wd_general:
        ld hl,({XQ4:#06x})
        call shr4_signed             ; HL = xi = x_q4 >> 4
        ex de,hl
        ld a,({ANCHV:#06x})
        ld l,a
        ld h,{VX >> 8:#04x}
        ld a,(hl)
        ld l,a
        ld h,0
        or a
        sbc hl,de                    ; HL = dx = vx[anchor] - xi
        ld a,({SNX:#06x})
        ld ({MA:#06x}),a
        call smulw                   ; nx*dx   (dx needs 16 bits: |dx| <= 255)
        ld hl,({MPROD:#06x})
        push hl

        ld hl,({YQ4:#06x})
        call shr4_signed
        ex de,hl
        ld a,({ANCHV:#06x})
        ld l,a
        ld h,{VY >> 8:#04x}
        ld a,(hl)
        ld l,a
        ld h,0
        or a
        sbc hl,de                    ; HL = dy
        ld a,({SNY:#06x})
        ld ({MA:#06x}),a
        call smulw
        pop de
        ld hl,({MPROD:#06x})
        add hl,de                    ; HL = whole
        push hl

        ld a,({XQ4:#06x})
        and 0x0f
        ld l,a
        ld h,0
        ld a,({SNX:#06x})
        ld ({MA:#06x}),a
        call smulw
        ld hl,({MPROD:#06x})
        push hl
        ld a,({YQ4:#06x})
        and 0x0f
        ld l,a
        ld h,0
        ld a,({SNY:#06x})
        ld ({MA:#06x}),a
        call smulw
        pop de
        ld hl,({MPROD:#06x})
        add hl,de                    ; HL = frac
        ld b,5
        call shrn_signed
        ex de,hl
        pop hl                       ; whole
        push de
        ld b,1
        call shrn_signed             ; HL = whole >>> 1
        pop de
        or a
        sbc hl,de
        ld ({DQ4:#06x}),hl
        ret

; --------------------------------------------------------------- SMULW ----
; MA (i8) * HL (i16, |HL| <= 255) -> MPROD (i16).
;
; The 8x8 form is not enough here: dx = vx[anchor] - xi runs to +/-255, which
; does not fit a signed byte. Feeding it as one anyway was the bug this
; subroutine exists to fix - it passed 5,366 of 5,383 oracle rows, failing
; only on the 12.5% general path, which is exactly how a truncation bug hides.
smulw:
        ld c,0
        bit 7,h
        jp z,sw_b_pos
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
        ld c,1
sw_b_pos:
        ld a,l
        ld ({MB:#06x}),a
        ld a,({MA:#06x})
        bit 7,a
        jp z,sw_a_pos
        neg
        ld b,a
        ld a,c
        xor 1
        ld c,a
        ld a,b
sw_a_pos:
        ld ({MA:#06x}),a
        ld a,c
        ld ({MSIGN:#06x}),a
        call umul
        ld a,({MSIGN:#06x})
        or a
        ret z
        ld hl,({MPROD:#06x})
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
        ld ({MPROD:#06x}),hl
        ret

; ------------------------------------------------------------- inv_for ----
; INVD = inv_for_dq4(DQ4)
inv_for:
        ld hl,({DQ4:#06x})
        bit 7,h
        jp z,if_pos
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
if_pos:
        ld de,161
        push hl
        or a
        sbc hl,de
        pop hl
        jp nc,if_notnear
        ld a,255
        ld ({INVD:#06x}),a
        ret
if_notnear:
        ld de,2032
        push hl
        or a
        sbc hl,de
        pop hl
        jp c,if_interp
        ld a,(0x{INVZ + 127:04X})
        ld ({INVD:#06x}),a
        ret
if_interp:
        ld a,l
        and 0x0f
        ld c,a                       ; C = f
        call shr4_u                  ; HL = a >> 4 = z
        ld a,l
        ld l,a
        ld h,{INVZ >> 8:#04x}
        ld e,(hl)                    ; E = invz[z] = x0
        inc l
        ld a,(hl)                    ; A = invz[z+1] = x1
        sub e                        ; A = d = x1 - x0   (signed)
        ld ({MA:#06x}),a
        ld a,c
        ld ({MB:#06x}),a
        push de
        call smul                    ; MPROD = d*f
        ld hl,({MPROD:#06x})
        ; smul overwrote MA with |d|, so the sign comes from MSIGN
        ld a,({MSIGN:#06x})
        or a
        jp nz,if_dneg
        ld de,8
        add hl,de
        jp if_rnd
if_dneg:
        ld de,-8
        add hl,de
if_rnd:
        ld b,4
        call shrn_signed             ; HL = (d*f +/- 8) >>> 4
        pop de
        ld d,0
        add hl,de                    ; HL = x0 + ...
        ld a,l
        ld ({INVD:#06x}),a
        ret

; -------------------------------------------------------------- inv_at ----
; OUTB = inv_at_invd(SID, INVD, BEARING, REL)
inv_at:
        ld hl,({BEARING:#06x})
        call shr4_u
        ld a,l                       ; A = bi = bearing >> 4
        ld l,a
        ld h,{SIN >> 8:#04x}
        ld c,(hl)                    ; C = sn = sin_q7[bi]
        ld a,l
        add a,64
        ld l,a
        ld a,(hl)                    ; A = cs = sin_q7[bi+64]
        ld b,a                       ; B = cs

; cardinal shortcuts: the final magnitude discards the normal's sign, so
; (+/-32 * trig) >> 5 == +/-trig and no multiply is needed at all.
        ld a,({SNY:#06x})
        or a
        jp nz,ia_try_y
        ld a,({SNX:#06x})
        cp 32
        jp z,ia_dot_cs
        cp 0xe0
        jp nz,ia_general
ia_dot_cs:
        ld a,b
        jp ia_have_dot
ia_try_y:
        ld a,({SNX:#06x})
        or a
        jp nz,ia_general
        ld a,({SNY:#06x})
        cp 32
        jp z,ia_dot_sn
        cp 0xe0
        jp nz,ia_general
ia_dot_sn:
        ld a,c
        jp ia_have_dot

ia_general:
        push bc
        ld a,({SNX:#06x})
        ld ({MA:#06x}),a
        ld a,b
        ld ({MB:#06x}),a
        call smul
        ld hl,({MPROD:#06x})
        push hl
        pop de
        pop bc
        push de
        ld a,({SNY:#06x})
        ld ({MA:#06x}),a
        ld a,c
        ld ({MB:#06x}),a
        call smul
        pop de
        ld hl,({MPROD:#06x})
        add hl,de
        ld b,5
        call shrn_signed
        ld a,l

ia_have_dot:
        bit 7,a
        jp z,ia_dot_pos
        neg
ia_dot_pos:
        cp 128
        jp c,ia_dot_ok
        ld a,127
ia_dot_ok:
        ld ({MB:#06x}),a
        ld a,({INVD:#06x})
        ld ({MA:#06x}),a
        call umul                    ; MPROD = invd * dot
        ld hl,({MPROD:#06x})
        ld de,64
        add hl,de
        call shr7_u                  ; HL = q
        ld a,l
        ld ({MA:#06x}),a
        ld hl,({REL:#06x})
        bit 7,h
        jp z,ia_rel_pos
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
ia_rel_pos:
        ld de,{SEC:#06x}
        add hl,de
        ld a,(hl)
        ld ({MB:#06x}),a             ; MB = sec_q7[|rel|]
        call umul
        ld hl,({MPROD:#06x})
        ld de,64
        add hl,de
        call shr7_u
        ld a,h
        or a
        jp z,ia_fits
        ld a,255
        ld ({OUTB:#06x}),a
        ret
ia_fits:
        ld a,l
        ld ({OUTB:#06x}),a
        ret

; ------------------------------------------------------------- helpers ----
; HL >>= 7, unsigned, WITHOUT shifting left first.
;
; The `shift left by 8-n then take H` trick used in the bearing kernel needs
; the shifted value to stay inside 16 bits, and here it does not: q*sec+64
; reaches 64,579, so a left shift would overflow and silently corrupt the
; result. Decompose instead - v>>7 == (v>>8)*2 + (low>>7) - which is exact
; for any 16-bit v and still 59 T against 203 T for the djnz loop.
shr7_u:
        ld a,l
        rlca
        and 1
        ld e,a
        ld d,0
        ld l,h
        ld h,0
        add hl,hl
        add hl,de
        ret

; HL >>= 4, unsigned. Callers here only pass values < 4096, so the left-shift
; form cannot overflow; asserted by the oracle rather than by comment alone.
shr4_u:
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld a,h
        ld l,a
        ld h,0
        ret

; HL >>= B, unsigned.
shrn_u:
        srl h
        rr l
        djnz shrn_u
        ret

; HL >>= B, signed, rounded TOWARD ZERO (not an arithmetic shift).
shrn_signed:
        bit 7,h
        jp z,ss_pos
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
        call shrn_u
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
        ret
ss_pos:
        call shrn_u
        ret

; HL >>= 4, signed, toward zero.
shr4_signed:
        ld b,4
        jp shrn_signed
"""


def build_mem(T):
    mem = bytearray(0x10000)
    vals = [(n * n) // 4 for n in range(512)]
    mem[UP_LO:UP_LO + 512] = bytes(v & 0xFF for v in vals)
    mem[UP_HI:UP_HI + 512] = bytes(v >> 8 for v in vals)
    mem[SIN:SIN + 256] = bytes(v & 0xFF for v in T["sin_q7"])
    mem[SEC:SEC + 513] = bytes(T["sec_q7"])
    mem[INVZ:INVZ + 128] = bytes(T["invz"])
    for base, key in ((NX, "nx_q5"), (NY, "ny_q5"), (ANCHOR, "seg_anchor"),
                      (VX, "vx"), (VY, "vy")):
        d = T[key]
        mem[base:base + len(d)] = bytes(v & 0xFF for v in d)
    return mem


def w16(mem, addr, v):
    v &= 0xFFFF
    mem[addr] = v & 0xFF
    mem[addr + 1] = v >> 8


def main():
    T = load_tables()
    code, labels = assemble(SRC, CODE)
    print(f"=== COLUMN-SOLVE KERNEL (invd / inv0 / inv1), {len(code)} bytes ===")

    dump = ROOT / "build" / "column_solve_oracle.txt"
    if not dump.exists():
        raise SystemExit(f"missing {dump} - run `make column-solve-bench`")
    rows = [line.split() for line in dump.read_text().splitlines() if line.strip()]
    stride = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    rows = rows[::stride]
    print(f"oracle rows: {len(rows)} (stride {stride} over "
          f"{sum(1 for _ in dump.open())} C-verified spans)\n")

    base = build_mem(T)
    fails = collections.Counter()
    ts = []
    shown = 0
    for r in rows:
        (px, py, yaw, sid, lo, hi, x0, x1, invd, inv0, inv1,
         c0, c1, n, iq, step) = (int(v) for v in r)
        mem = bytearray(base)
        mem[CODE:CODE + len(code)] = code
        mem[SID] = sid
        w16(mem, XQ4, px)
        w16(mem, YQ4, py)
        w16(mem, YAWQ, (yaw << 4) & 0xFFFF)
        w16(mem, LO, lo)
        w16(mem, HI, hi)
        cpu = Z80(mem)
        cpu.run(CODE)
        ts.append(cpu.t)
        got = (cpu.m[INVD], cpu.m[INV0], cpu.m[INV1])
        want = (invd, inv0, inv1)
        if got != want:
            for i, nm in enumerate(("invd", "inv0", "inv1")):
                if got[i] != want[i]:
                    fails[nm] += 1
            if shown < 8:
                shown += 1
                print(f"  MISMATCH pose=({px},{py},yaw={yaw}) sid={sid} "
                      f"lo={lo} hi={hi}: want {want} got {got}")

    if fails:
        print(f"\nFAILURES by field: {dict(fails)}")
        raise SystemExit(f"kernel is WRONG")
    print(f"VERIFIED: {len(rows)}/{len(rows)} exact - invd, inv0 and inv1 all "
          f"match the shipped C on every sampled span")

    mean_t = stt.mean(ts)
    print(f"\nT-states per span: mean={mean_t:.1f}  min={min(ts)}  max={max(ts)}")

    spans = 4.30
    line = mean_t * spans
    print(f"\ncolumn-solve (invd/inv0/inv1) at {spans} visible spans/update: "
          f"{line:,.0f} T/update")
    print(f"  projection this replaces (A13 re-cost)      7,636 T")
    print(f"  earlier projection (shift-add primitive)   17,729 T")

    bearing, decode_clip, gate, emit = 5833.0, 11036.0, 2339.0, 21756.0
    tot = bearing + decode_clip + gate + line + emit
    frame = 59736.0
    print(f"\n=== WHOLE-UPDATE BUDGET ===")
    print(f"  bearing lookup (A12)  {bearing:9,.0f} T   [cycle-exact]")
    print(f"  decode-clip           {decode_clip:9,.0f} T   [cycle-exact]")
    print(f"  GATE                  {gate:9,.0f} T   [cycle-exact, old primitive]")
    print(f"  column-solve          {line:9,.0f} T   [CYCLE-EXACT, this file]")
    print(f"  emit                  {emit:9,.0f} T   [cycle-exact]")
    print(f"  ------------------------------------")
    print(f"  TOTAL                 {tot:9,.0f} T   {frame/tot:.2f} updates/frame")
    print(f"\n  Every line is now measured. Still missing from this total:")
    print(f"  angle_x (2 lookups/span, between decode and column-solve) and")
    print(f"  the Q6 iq/step computation - both small, both uncosted.")


if __name__ == "__main__":
    main()
