#!/usr/bin/env python3
"""CARRY_EDGE_A: run-scoped materializer that carries the edge across columns.

The per-column materializer (`z80_materialize_bench.py`) measured 7,798 T per
column and 82.8% of the whole update. External review identified a specific,
exact redundancy in it, and this tests that claim in isolation.

THE REDUNDANCY
--------------
Per column the old kernel computes BOTH endpoints from scratch:

    invl = clamp((iq       + 32) >> 6, 255)
    invr = clamp((iq + step + 32) >> 6, 255)
    ... then the caller does  iq += step

so the next column's `invl` recomputes exactly the value the previous column
already produced as `invr`. Half of the Q6 decode work is thrown away and
redone. The fix is to carry it:

    invl(c+1) = invr(c)

This is the runtime analogue of what SPANC does in the baker - "the next one
begins where the previous one ended, do not solve it twice."

DELIBERATELY MINIMAL
--------------------
Only the endpoint carry and the per-run hoisting of the shade-dependent tile
bases change. Ownership model, tile choice, edge quantization and the map
addressing are all untouched, so any measured difference is attributable to
this one change and nothing else. That attribution discipline is the point:
a rewrite that changed five things at once would leave us unable to say which
one paid.

Verified against `build/materialize_run_oracle.txt`, a per-RUN dump from the
same self-checking probe whose column loop reproduces `tsp_polar_render`'s
entire name table on 29,824/29,824 poses.

    make materialize-run-bench
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80  # noqa: E402

CODE = 0x0000
IQ, STEP, PROFILE = 0xC000, 0xC002, 0xC004
C0, C1, LREAL, RREAL, SHADE = 0xC005, 0xC006, 0xC007, 0xC008, 0xC009
MAP = 0xC200                      # 20x18 words, row*40 + col*2
TL, TR, BL, BR = 0xC010, 0xC012, 0xC014, 0xC016
YL, YR, BOTTOM = 0xC018, 0xC01A, 0xC01C
SLOPE, MAG, R0, R1 = 0xC01D, 0xC01E, 0xC01F, 0xC020
LOCL, ATTR, TMP = 0xC022, 0xC024, 0xC026
HLH, HRH = 0xC028, 0xC029
EDGEBASE, FULLBASE = 0xC02A, 0xC02C
INVL, INVR, CURC, JQ, BORDER, COLPTR = 0xC02E, 0xC02F, 0xC030, 0xC032, 0xC034, 0xC036
FULLNB, RPTR, FCOUNT = 0xC038, 0xC03A, 0xC03C

HORIZON, ROWS, COLS = 72, 18, 20

_HELPER_VARS = dict(
    TL=0xC010, TR=0xC012, BL=0xC014, BR=0xC016,
    YL=0xC018, YR=0xC01A, BOTTOM=0xC01C,
    SLOPE=0xC01D, MAG=0xC01E, R0=0xC01F, R1=0xC020,
    LOCL=0xC022, ATTR=0xC024, TMP=0xC026,
    EDGEBASE=0xC02A, FULLBASE=0xC02C, COLBUF=0xC036,
    ROWS=18,
)
_HELPER_VARS['ROWS - 1'] = 17
_HELPER_VARS['TMP + 1'] = 0xC027
_HELPER_VARS['FULLBASE + 1'] = 0xC02D
# The helper subroutines (rowfloor / draw_edge / edge_entry / draw_full /
# cmps / sign-extends) are lifted VERBATIM from the per-column kernel, so the
# A/B differs only in the run loop above them - not in how a row is drawn.
HELPERS = open(ROOT / "tools" / "materialize_helpers.asm").read().format(**_HELPER_VARS)

SRC = f"""
; ---- per-RUN setup (hoisted out of the column loop) ----
        ld a,({SHADE:#06x})
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,39
        add hl,de
        ld ({EDGEBASE:#06x}),hl
        ld a,({SHADE:#06x})
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        ld d,h
        ld e,l
        add hl,hl
        add hl,de
        ld de,3
        add hl,de
        ld ({FULLNB:#06x}),hl        ; FULL base WITHOUT border

; ---- invl for the FIRST column only ----
        ld hl,({IQ:#06x})
        ld de,32
        add hl,de
        call shr6_clamp
        ld ({INVL:#06x}),a
        ld hl,({IQ:#06x})
        ld ({JQ:#06x}),hl
        ld a,({C0:#06x})
        ld ({CURC:#06x}),a

run_loop:
; ---- invr: the ONLY Q6 decode per column ----
        ld hl,({JQ:#06x})
        ld de,({STEP:#06x})
        add hl,de
        ld de,32
        add hl,de
        call shr6_clamp
        ld ({INVR:#06x}),a
        srl a
        ld ({HRH:#06x}),a
        ld a,({INVL:#06x})
        srl a
        ld ({HLH:#06x}),a

; ---- border: only the first and last column can carry one ----
        xor a
        ld ({BORDER:#06x}),a
        ld a,({CURC:#06x})
        ld b,a
        ld a,({C0:#06x})
        cp b
        jp nz,bd_not_first
        ld a,({LREAL:#06x})
        or a
        jp z,bd_not_first
        ld a,1
        ld ({BORDER:#06x}),a
bd_not_first:
        ld a,({CURC:#06x})
        ld b,a
        ld a,({C1:#06x})
        cp b
        jp nz,bd_done
        ld a,({RREAL:#06x})
        or a
        jp z,bd_done
        ld a,({BORDER:#06x})
        or 2
        ld ({BORDER:#06x}),a
bd_done:
        ld hl,({FULLNB:#06x})
        ld a,({BORDER:#06x})
        ld e,a
        ld d,0
        add hl,de
        ld ({FULLBASE:#06x}),hl

; ---- column pointer: MAP + c*2 (row stride added per row) ----
        ld a,({CURC:#06x})
        ld l,a
        ld h,0
        add hl,hl
        ld de,{MAP:#06x}
        add hl,de
        ld ({COLPTR:#06x}),hl

; ---- geometry (identical to the per-column kernel) ----
        ld a,{HORIZON}
        ld b,a
        ld a,({HLH:#06x})
        ld c,a
        ld a,b
        sub c
        call sx_tl
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
        ld ({BL:#06x}),hl
        ld a,{HORIZON}
        ld b,a
        ld a,({HRH:#06x})
        add a,b
        ld l,a
        ld h,0
        ld ({BR:#06x}),hl

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

; ---- THE CARRY: this column's right endpoint IS the next column's left ----
        ld a,({INVR:#06x})
        ld ({INVL:#06x}),a
        ld hl,({JQ:#06x})
        ld de,({STEP:#06x})
        add hl,de
        ld ({JQ:#06x}),hl
        ld a,({CURC:#06x})
        ld b,a
        ld a,({C1:#06x})
        cp b
        jp z,run_done
        ld a,b
        inc a
        ld ({CURC:#06x}),a
        jp run_loop
run_done:
        halt
""" + HELPERS


# The helper block addresses the destination as COLBUF + r*2 (single column).
# Here the destination is a full map: COLPTR + r*40. Patch those two sites.
SRC = SRC.replace("""        ld a,(0xc01f)
        ld l,a
        ld h,0
        add hl,hl
        ld de,0xc036
        add hl,de
        ld a,(0xc026)""", """        call row_addr
        ld a,(0xc026)""")
SRC = SRC.replace("""        ld a,(0xc01f)
        ld l,a
        ld h,0
        add hl,hl
        ld de,0xc036
        add hl,de
        ld a,(0xc02c)""", """        call row_addr
        ld a,(0xc02c)""")
SRC += f"""
; HL = COLPTR + R0*40   (row stride is 20 words)
row_addr:
        ld a,({R0:#06x})
        ld l,a
        ld h,0
        ld d,h
        ld e,l                       ; DE = r   (saved BEFORE doubling)
        add hl,hl                    ; 2r
        add hl,hl                    ; 4r
        add hl,de                    ; 5r
        add hl,hl                    ; 10r
        add hl,hl                    ; 20r
        add hl,hl                    ; 40r
        ld de,({COLPTR:#06x})
        add hl,de
        ret
"""


# ---------------------------------------------------------------------------
# ROWPTR_B: carry the name-table pointer down the row loops.
#
# Both hot loops walk rows by +1, so the destination address advances by
# exactly +40 (one name-table row, 20 words). `row_addr` recomputed r*40 from
# scratch every row - 156 T of shifts, a CALL and a RET - and the profile put
# it at 14.6% of the kernel. Compute it ONCE per loop, then add 40.
#
# The store leaves HL at RPTR+1 (after `inc hl` for the high byte), so the
# advance is +39, not +40. Off-by-one there would corrupt every row after the
# first, which is exactly the sort of thing the twin catches.
SRC_ROWPTR = SRC.replace("""de_loop:""", """        call row_addr
        ld (0x{RPTR:04x}),hl
de_loop:""".format(RPTR=RPTR)).replace("""df_loop:""", """        call row_addr
        ld (0x{RPTR:04x}),hl
df_loop:""".format(RPTR=RPTR)).replace("""        call row_addr
        ld a,(0xc026)
        ld (hl),a
        inc hl
        ld a,(0xC027)
        ld (hl),a""", """        ld hl,(0x{RPTR:04x})
        ld a,(0xc026)
        ld (hl),a
        inc hl
        ld a,(0xC027)
        ld (hl),a
        ld de,39
        add hl,de
        ld (0x{RPTR:04x}),hl""".format(RPTR=RPTR)).replace("""        call row_addr
        ld a,(0xc02c)
        ld (hl),a
        inc hl
        ld a,(0xC02D)
        ld (hl),a""", """        ld hl,(0x{RPTR:04x})
        ld a,(0xc02c)
        ld (hl),a
        inc hl
        ld a,(0xC02D)
        ld (hl),a
        ld de,39
        add hl,de
        ld (0x{RPTR:04x}),hl""".format(RPTR=RPTR))

# ---------------------------------------------------------------------------
# INLINECMP_C: inline the signed compare.
#
# `cmps` was 16.3% of the kernel. Almost none of that is the comparison - it
# is CALL (17) + RET (10) + a second push/pop pair (21) wrapping ~70 T of
# actual work. Inlining keeps the identical bias-then-SBC primitive and drops
# 48 T per site.
#
# DE is NOT preserved by the inline form. Every call site loads DE fresh
# immediately before comparing and none reads it afterwards - but that is an
# assertion about the code, so the twin is what actually settles it.
SRC_INLINECMP = SRC_ROWPTR.replace("""        call cmps""", """        push hl
        ld a,h
        xor 0x80
        ld h,a
        ld a,d
        xor 0x80
        ld d,a
        or a
        sbc hl,de
        pop hl""")

# ---------------------------------------------------------------------------
# FILLLOOP_D: make the interior fill a register-resident loop.
#
# After INLINECMP_C the re-profile put `df_loop` on top at 15.7%. Almost none
# of that is the fill itself: per row it reloaded the pointer from memory,
# reloaded both halves of a word that never changes, wrote the pointer back,
# then ran a compare-based loop test costing ~70 T on its own.
#
# Everything the loop needs fits in registers - HL the pointer, BC the word,
# DE the row stride, A the count - so nothing touches memory except the two
# stores that are the actual work. The two `inc hl` from storing the word are
# why the stride added is 38 rather than 40.
SRC_FILLLOOP = SRC_INLINECMP.replace('        call row_addr\n        ld (0xc03a),hl\ndf_loop:\n        ld hl,(0xc03a)\n        ld a,(0xc02c)\n        ld (hl),a\n        inc hl\n        ld a,(0xC02D)\n        ld (hl),a\n        ld de,39\n        add hl,de\n        ld (0xc03a),hl\n        ld a,(0xc01f)\n        ld b,a\n        ld a,(0xc020)\n        cp b\n        ret z\n        ld a,b\n        inc a\n        ld (0xc01f),a\n        jp df_loop', '        ld a,(0xc020)\n        ld b,a\n        ld a,(0xc01f)\n        neg\n        add a,b\n        inc a                        ; A = R1 - R0 + 1 = row count\n        ld (0xc03c),a\n        call row_addr                ; HL = first row address (clobbers A, DE)\n        ld bc,(0xc02c)               ; BC = the FULL word (C=lo, B=hi)\n        ld de,38                     ; two stores advance +2; +38 = one row\n        ld a,(0xc03c)\ndf_loop:\n        ld (hl),c\n        inc hl\n        ld (hl),b\n        inc hl\n        add hl,de\n        dec a\n        jp nz,df_loop\n        ret')
assert SRC_FILLLOOP != SRC_INLINECMP, "FILLLOOP_D anchor did not match"

SRC_NOCARRY = SRC.replace("""run_loop:
; ---- invr: the ONLY Q6 decode per column ----""",
"""run_loop:
; ---- NOCARRY twin: recompute invl from scratch, as the per-column kernel
;      did. Everything else is byte-identical to CARRY_EDGE_A, so the A/B
;      isolates the carry and nothing else.
        ld hl,(0xc032)
        ld de,32
        add hl,de
        call shr6_clamp
        ld (0xc02e),a
; ---- invr ----""")


def background_map():
    m = []
    for r in range(ROWS):
        v = 0 if r < 9 else (2 if r == 9 else 1)
        m.extend([v] * COLS)
    return m


def main():
    dump = ROOT / "build" / "materialize_run_oracle.txt"
    if not dump.exists():
        raise SystemExit(f"missing {dump} - run `make materialize-bench` first")
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
    lines = [l.split() for l in dump.read_text().splitlines() if l.strip()]
    stepn = max(1, len(lines) // limit)
    cases = lines[::stepn]

    variants = [("CARRY_EDGE_A", SRC), ("NOCARRY twin", SRC_NOCARRY),
                ("ROWPTR_B", SRC_ROWPTR), ("INLINECMP_C", SRC_INLINECMP),
                ("FILLLOOP_D", SRC_FILLLOOP)]
    results = {}
    print(f"oracle: {len(cases)} runs strided across {len(lines)}\n")

    bgm = background_map()
    for vname, vsrc in variants:
        code, _ = assemble(vsrc, CODE)
        base = bytearray(0x10000)
        base[CODE:CODE + len(code)] = code
        results[vname] = run_variant(vname, code, base, bgm, cases)
    a, b, rp = (results["CARRY_EDGE_A"], results["NOCARRY twin"],
                results["ROWPTR_B"])
    ic = results["INLINECMP_C"]
    fl = results["FILLLOOP_D"]
    print(f"\n=== A/B: does carrying the endpoint pay? ===")
    print(f"  NOCARRY twin   {b:8.1f} T/column")
    print(f"  CARRY_EDGE_A   {a:8.1f} T/column   {(a/b - 1):+.1%}")
    print(f"\nBoth use IDENTICAL map addressing, helpers and tile logic, so this")
    print(f"difference is the carry and nothing else.")
    OLD_PER_COL = 7798.0
    print(f"\n=== ROWPTR_B: carry the name-table pointer down the row loops ===")
    print(f"  CARRY_EDGE_A   {a:8.1f} T/column   (baseline)")
    print(f"  ROWPTR_B       {rp:8.1f} T/column   {(rp/a - 1):+.1%}")
    print(f"\n=== INLINECMP_C: inline the signed compare ===")
    print(f"  ROWPTR_B       {rp:8.1f} T/column   (baseline)")
    print(f"  INLINECMP_C    {ic:8.1f} T/column   {(ic/rp - 1):+.1%}")
    print(f"\n=== FILLLOOP_D: register-resident interior fill ===")
    print(f"  INLINECMP_C    {ic:8.1f} T/column   (baseline)")
    print(f"  FILLLOOP_D     {fl:8.1f} T/column   {(fl/ic - 1):+.1%}")
    print(f"\ncumulative from CARRY_EDGE_A: {(fl/a - 1):+.1%}")
    print(f"\nFor reference the per-COLUMN kernel measured {OLD_PER_COL:.0f} T/column,")
    print(f"but it wrote into a single-column buffer (r*2 addressing) rather")
    print(f"than a real 20x18 map (r*40 + c*2). That baseline was therefore")
    print(f"OPTIMISTIC - it never paid realistic addressing - so it is not a")
    print(f"fair comparison point and is not used as one here.")
    cols = 873084.0 / 29824.0
    other = 5833.0 + 11036.0 + 2339.0 + 26820.0 + 1314.0
    line = fl * cols
    print(f"\nmaterialize (FILLLOOP_D, real addressing)    {line:,.0f} T/update")
    print(f"whole update                                 {line+other:,.0f} T")
    print(f"updates/frame                                {59736.0/(line+other):.2f}")
    return


def run_variant(vname, code, base, bgm, cases):
    fails = 0
    ts = []
    cols_total = 0
    for row in cases:
        iq, stp, c0, c1, prof, lr, rr, sh = (int(v) for v in row[:8])
        want = [int(v) for v in row[8:]]
        mem = bytearray(base)
        mem[IQ] = iq & 0xFF; mem[IQ + 1] = (iq >> 8) & 0xFF
        mem[STEP] = stp & 0xFF; mem[STEP + 1] = (stp >> 8) & 0xFF
        mem[PROFILE], mem[C0], mem[C1] = prof, c0, c1
        mem[LREAL], mem[RREAL], mem[SHADE] = lr, rr, sh
        for i, v in enumerate(bgm):
            mem[MAP + 2 * i] = v & 0xFF
            mem[MAP + 2 * i + 1] = v >> 8
        cpu = Z80(mem)
        cpu.run(CODE)
        got = []
        for c in range(c0, c1 + 1):
            for r in range(ROWS):
                a = MAP + r * 40 + c * 2
                got.append(cpu.m[a] | (cpu.m[a + 1] << 8))
        ts.append(cpu.t)
        cols_total += (c1 - c0 + 1)
        if got != want:
            fails += 1
            if fails <= 3:
                print(f"  MISMATCH iq={iq} step={stp} c0={c0} c1={c1} prof={prof}")
                print(f"    want {want[:20]}")
                print(f"    got  {got[:20]}")
    if fails:
        raise SystemExit(f"FAIL: {vname}: {fails} mismatches")

    per_col = sum(ts) / cols_total
    print(f"{vname:14s} VERIFIED {len(cases)-fails}/{len(cases)} runs exact   "
          f"{per_col:8.1f} T/column  ({stt.mean(ts):,.0f} T/run, "
          f"{cols_total/len(cases):.2f} cols/run)")
    return per_col


if __name__ == "__main__":
    main()
