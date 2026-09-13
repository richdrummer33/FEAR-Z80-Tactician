"""WALK_PROG: play back a ROM-resident exact edge program.

A44's census: with RISER demoted to a slow path (exact, 6.0% of cells) and the
horizon cut to the 20 columns a Game Gear can actually show, the exact program
set is 1.07 MB at L=8.  That fits the stated 4 MB envelope, so this measures
what the playback costs.

The distinction from A40's EMIT_B matters.  EMIT_B streams (dest, word) pairs
that something must GENERATE every frame - 259 bytes per update, and the
generator was the unsolved problem.  WALK_PROG reads from ROM: nothing
generates it, and the destination is advanced by a delta the program carries,
so the whole accumulator-to-tile derivation disappears.

Entry: 4 bytes - the 16-bit name-table word, then the 16-bit destination delta
already folded (row movement * 40, plus the column step, minus the one `inc hl`
the store leaves behind).  4 bytes keeps both fields `pop`-aligned.
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80                       # noqa: E402
import z80_materialize_run_bench as rb                  # noqa: E402
import z80_materialize_masked_bench as mb               # noqa: E402
import z80_edge_lut_bench as el                         # noqa: E402
import z80_semantic_profile as sp                       # noqa: E402

CODE = 0x0000
PROG = 0x8000
CNT, DEST = 0xBE00, 0xBE02
ROWS, COLS = rb.ROWS, rb.COLS

SRC_PROG = f"""
        ld a,(0x{CNT:04x})
        ld hl,(0x{DEST:04x})
        ld sp,{PROG}
wp_loop:
        pop de                       ; the baked name-table word
        ld (hl),e
        inc hl
        ld (hl),d
        pop bc                       ; the baked destination delta
        add hl,bc
        dec a                        ; NOT djnz: `pop bc` destroys B, which is
        jp nz,wp_loop                ; the counter.  Caught by the kernel
        halt                         ; never halting.
"""

# A 2-byte variant for the common case where the edge stays in the same tile
# row, so the delta is the constant +1 and need not be stored.
SRC_FLATROW = f"""
        ld a,(0x{CNT:04x})
        ld b,a
        ld hl,(0x{DEST:04x})
        ld sp,{PROG}
wq_loop:
        pop de
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        djnz wq_loop
        halt
"""


def capture(img, runs, bgm, labels):
    """The edge cells the shipped kernel writes, per run-edge, in order."""
    L_R1, L_DL = labels["de_r1ok"], labels["de_loop"]
    BOTTOM, DESTP = 0xc01c, 0xc03a

    class W(Z80):
        def _step(self):
            if self.pc == L_R1:
                self.cur = dict(bot=self.m[BOTTOM], cells=[])
                self.calls.append(self.cur)
            if self.pc == L_DL:
                self.cur["cells"].append(
                    [self.m[DESTP] | (self.m[DESTP + 1] << 8), None])
            Z80._step(self)

    mem = bytearray(img)
    for i, v in enumerate(bgm):
        mem[mb.MAP + 2 * i] = v & 0xFF
        mem[mb.MAP + 2 * i + 1] = v >> 8
    for i in range(COLS * 3):
        mem[mb.COV + i] = 0
    out = []
    for iq, stp, c0, c1, prof, lr, rr, sh in reversed(runs):
        mem[rb.IQ] = iq & 0xFF; mem[rb.IQ + 1] = (iq >> 8) & 0xFF
        mem[rb.STEP] = stp & 0xFF; mem[rb.STEP + 1] = (stp >> 8) & 0xFF
        mem[rb.PROFILE], mem[rb.C0], mem[rb.C1] = prof, c0, c1
        mem[rb.LREAL], mem[rb.RREAL], mem[rb.SHADE] = lr, rr, sh
        cpu = W(mem); cpu.calls = []; cpu.cur = None; cpu.run(CODE)
        mem = cpu.m
        for c in cpu.calls:
            for cell in c["cells"]:
                cell[1] = mem[cell[0]] | (mem[cell[0] + 1] << 8)
        # ONE program per run-edge, not per draw_edge call.  Restarting per
        # call is the structure this architecture exists to remove: those calls
        # average 0.97 rows, so a per-call harness charges the 43 T chain setup
        # to almost every row and measures the old shape, not the new one.
        for which in (0, 1):
            cells = [cell for c in cpu.calls if c["bot"] == which
                     for cell in c["cells"]]
            if cells:
                out.append({"bot": which, "cells": cells})
    return out


def build_program(cells, four_byte):
    """Bake the exact program for one run-edge from the cells the renderer
    produced.  The deltas are what a ROM-baked program would carry; the words
    are the renderer's own, so playback is exact by construction and the
    comparison below is a real check of the KERNEL, not of the baker."""
    b = bytearray()
    for i, (dest, word) in enumerate(cells):
        b += bytes((word & 0xFF, word >> 8))
        if four_byte:
            nxt = cells[i + 1][0] if i + 1 < len(cells) else dest + 2
            d = (nxt - dest - 1) & 0xFFFF
            b += bytes((d & 0xFF, d >> 8))
    return b


def run_kernel(src, prog, count, dest, base):
    code, _ = assemble(src, CODE)
    mem = bytearray(0x10000)
    mem[CODE:CODE + len(code)] = code
    mem[PROG:PROG + len(prog)] = prog
    mem[CNT] = count
    mem[DEST] = dest & 0xFF; mem[DEST + 1] = dest >> 8
    for i, v in enumerate(base):
        mem[mb.MAP + 2 * i] = v & 0xFF
        mem[mb.MAP + 2 * i + 1] = v >> 8
    cpu = Z80(mem); cpu.run(CODE)
    got = [cpu.m[mb.MAP + 2 * i] | (cpu.m[mb.MAP + 2 * i + 1] << 8)
           for i in range(ROWS * COLS)]
    return cpu.t, got, len(code)


def main():
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    lines = [l.split() for l in
             open(ROOT / "build" / "coverage_pose_oracle.txt") if l.strip()]
    step = max(1, len(lines) // limit)
    cases = []
    for f in lines[::step]:
        n = int(f[0])
        cases.append([tuple(int(v) for v in f[1 + 8 * k: 9 + 8 * k])
                      for k in range(n)])

    img, labels, _ = sp.base_image(sp.instrumented_source(), el.load_table())
    bgm = mb.background_map()

    t4, t2, nrows4, nrows2 = 0, 0, 0, 0
    fails4 = fails2 = 0
    flatrow_rows = 0
    total_rows = 0
    nbytes = 0
    for runs in cases:
        for ce in capture(img, runs, bgm, labels):
            cells = ce["cells"]
            if len(cells) > 255:
                continue
            base = list(bgm)
            expect = list(base)
            for dest, word in cells:
                expect[(dest - mb.MAP) // 2] = word
            total_rows += len(cells)
            same_row = all(cells[i + 1][0] == cells[i][0] + 2
                           for i in range(len(cells) - 1))
            prog = build_program(cells, True)
            t, got, nbytes = run_kernel(SRC_PROG, prog, len(cells),
                                        cells[0][0], base)
            t4 += t; nrows4 += len(cells)
            if got != expect:
                fails4 += 1
            if same_row:
                flatrow_rows += len(cells)
                prog = build_program(cells, False)
                t, got, _ = run_kernel(SRC_FLATROW, prog, len(cells),
                                       cells[0][0], base)
                t2 += t; nrows2 += len(cells)
                if got != expect:
                    fails2 += 1

    print(f"WALK_PROG - {len(cases)} poses, {total_rows} edge rows, "
          f"kernel {nbytes} bytes\n")
    print(f"{'kernel':14} {'T/edge row':>11} {'exact':>8}  {'entry':>7}  what it is")
    print(f"{'WALK_PROG':14} {t4/nrows4:11.1f} "
          f"{'EXACT' if not fails4 else str(fails4)+' WRONG':>8}  {'4 B':>7}"
          f"  ROM program, word + destination delta")
    if nrows2:
        print(f"{'WALK_FLATROW':14} {t2/nrows2:11.1f} "
              f"{'EXACT' if not fails2 else str(fails2)+' WRONG':>8}  {'2 B':>7}"
              f"  same tile row throughout ({100.0*flatrow_rows/total_rows:.1f}% of rows)")
    print(f"{'EMIT_B (A40)':14} {49.2:11.1f} {'--':>8}  {'4 B':>7}"
          f"  stream something must GENERATE each frame")
    print(f"{'WALK_FLAT(A41)':14} {66.2:11.1f} {'--':>8}  {'--':>7}")
    print(f"{'shipped (A39)':14} {1023.0:11.1f} {'--':>8}  {'--':>7}"
          f"  stages 6+7+8")

    EDGE = 66252.0
    GEOM, EXT = 28163.0, 25357.0
    v = t4 / nrows4
    newedge = EDGE * v / 1023.0
    print(f"\n  edge path 66,252 -> {newedge:,.0f} T/pose")
    print(f"  a ROM program also removes endpoint geometry ({GEOM:,.0f}) and")
    print(f"  row extents ({EXT:,.0f}): both are what it replaces.")
    print(f"  materializer 175,827 -> {175827-EDGE+newedge-GEOM-EXT:,.0f} T/pose")
    print(f"  3x sequence line needs 58,600 T")
    return 0


if __name__ == "__main__":
    sys.exit(main())
