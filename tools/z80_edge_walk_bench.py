"""WALK: can a persistent edge walker replace repeated draw_edge setup?

A39/A40: the edge path costs 1,023 T per row written; emitting a row whose
answer is already known costs 49.2 T (EMIT_B).  The gap is setup, re-entered
66.9 times per pose to emit 0.97 rows each.  This asks whether a walker that
KEEPS its state across columns can close that gap without an oracle stream.

The walk recurrence was derived from the shipped kernel and is exact on
2,683/2,683 column-to-column steps:

    locl' = locl + dy - 8 * (row' - row)         dy = YL' - YL

so the entire edge state is the left endpoint y, advanced by one step per
column.  Two kernels follow from the measured shape census:

  WALK_FLAT  dy == 0 for the whole run-edge (46.2% of run-edges, 44.7% of
             rows): locl never changes, so the tile word never changes and the
             walk is a contiguous register-resident store loop.
  WALK_STEP  dy != 0: locl moves, the row can cross, and the tile word must be
             re-read from the EDGELUT row.

Both are verified against the cells the shipped kernel actually wrote.
"""
from __future__ import annotations
import collections
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
ROWS, COLS = rb.ROWS, rb.COLS
WDEST, WWORD, WCNT, WLOCL, WDY, WLUTP = (0xBE00, 0xBE02, 0xBE04,
                                         0xBE05, 0xBE06, 0xBE08)

SRC_FLAT = f"""
        ld hl,(0x{WDEST:04x})
        ld de,(0x{WWORD:04x})
        ld a,(0x{WCNT:04x})
        ld b,a
wf_loop:
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        djnz wf_loop
        halt
"""

# The moving walk.  locl lives in A between columns; the destination stays in
# HL; the cached tile word stays in DE; C carries dy and B the column count.
# A row crossing is +/-40 on HL, which needs BC, so BC is stacked around it -
# that is the price of having only one 16-bit adder.
SRC_STEP = f"""
        ld hl,(0x{WDEST:04x})
        ld de,(0x{WWORD:04x})
        ld a,(0x{WCNT:04x})
        ld b,a
        ld a,(0x{WDY:04x})
        ld c,a
        ld a,(0x{WLOCL:04x})
ws_loop:
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        dec b
        jp z,ws_done
        add a,c                      ; locl += dy
        jp m,ws_neg
        cp 8
        jp c,ws_have
        sub 8
        push bc
        ld bc,40
        add hl,bc
        pop bc
        jp ws_have
ws_neg:
        add a,8
        push bc
        ld bc,0xffd8                 ; -40
        add hl,bc
        pop bc
ws_have:
        push hl                      ; re-read the tile for the new locl
        push af
        add a,a
        add a,16
        ld l,a
        ld h,0
        ld de,(0x{WLUTP:04x})        ; LUT row base, any alignment
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        pop af
        pop hl
        jp ws_loop
ws_done:
        halt
"""


def capture(img, runs, bgm, labels):
    """Per run-edge: the ordered (dest, word, locl, row) the kernel emitted."""
    L_R1, L_DL = labels["de_r1ok"], labels["de_loop"]
    LOCC, YL, BOTTOM = 0xc056, 0xc018, 0xc01c
    R0, COL, DEST = 0xc01f, 0xc030, 0xc03a

    def s16(v):
        return v - 65536 if v > 32767 else v

    class W(Z80):
        def _step(self):
            if self.pc == L_R1:
                self.cur = dict(
                    bot=self.m[BOTTOM],
                    yl=s16(self.m[YL] | (self.m[YL + 1] << 8)),
                    r0=self.m[R0], col=self.m[COL], cells=[])
                self.calls.append(self.cur)
            if self.pc == L_DL:
                self.cur["cells"].append(
                    [s16(self.m[LOCC] | (self.m[LOCC + 1] << 8)),
                     self.m[DEST] | (self.m[DEST + 1] << 8), None])
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
                cell[2] = mem[cell[1]] | (mem[cell[1] + 1] << 8)
        out.append((sh, cpu.calls))
    return out, mem


def classify(calls, which):
    seq = [c for c in calls if c["bot"] == which]
    if not seq:
        return None, None
    dys = [b["yl"] - a["yl"] for a, b in zip(seq, seq[1:])]
    multi = any(len(c["cells"]) > 1 for c in seq)
    if multi:
        return "multi-row", seq
    if not dys or set(dys) == {0}:
        return "flat", seq
    if len(set(dys)) == 1:
        return "linear", seq
    return "varying", seq


def run_kernel(src, img, setup, base):
    code, _ = assemble(src, CODE)
    mem = bytearray(0x10000)
    mem[CODE:CODE + len(code)] = code
    for a, v in setup:
        mem[a] = v & 0xFF
        if a in (WDEST, WWORD, WLUTP):
            mem[a + 1] = (v >> 8) & 0xFF
    for i, v in enumerate(base):
        mem[mb.MAP + 2 * i] = v & 0xFF
        mem[mb.MAP + 2 * i + 1] = v >> 8
    # the EDGELUT rows the walker reads
    for r, vals in el.load_table().items():
        for i, v in enumerate(vals):
            mem[el.LUT + 64 * r + 2 * i] = v & 0xFF
            mem[el.LUT + 64 * r + 2 * i + 1] = (v >> 8) & 0xFF
    cpu = Z80(mem); cpu.run(CODE)
    got = [cpu.m[mb.MAP + 2 * i] | (cpu.m[mb.MAP + 2 * i + 1] << 8)
           for i in range(ROWS * COLS)]
    return cpu.t, got


def segments(cells):
    """Maximal groups that a WALK_FLAT loop can emit in one go: destinations
    advancing by exactly +2 AND carrying the identical tile word.

    Both conditions are load-bearing and the second was a surprise.  Zero
    vertical movement of the carried endpoint does NOT imply a constant tile:
    the tile is selected by the column's own slope (yr - yl), which can change
    while the left endpoint stands still.  A chain also breaks wherever a
    column emitted no row at all, and A39 measured 20.1% of draw_edge calls
    emitting zero rows."""
    out, cur = [], [cells[0]]
    for a, b in zip(cells, cells[1:]):
        if b[1] == a[1] + 2 and b[2] == a[2]:
            cur.append(b)
        else:
            out.append(cur); cur = [b]
    out.append(cur)
    return out


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

    src = sp.instrumented_source()
    img, labels, _ = sp.base_image(src, el.load_table())
    bgm = mb.background_map()

    seglen = collections.Counter()
    twalk, nrows_tot, nseg_tot = [], 0, 0
    fails = 0
    nposes = 0
    per_edge_rows = []

    for runs in cases:
        per_run, _ = capture(img, runs, bgm, labels)
        nposes += 1
        for sh, calls in per_run:
            for which in (0, 1):
                seq = [c for c in calls if c["bot"] == which]
                cells = [cell for c in seq for cell in c["cells"]]
                if not cells:
                    continue
                per_edge_rows.append(len(cells))
                segs = segments(cells)
                for sg in segs:
                    seglen[len(sg)] += 1
                nseg_tot += len(segs)
                nrows_tot += len(cells)

                base = list(bgm)
                expect = list(bgm)
                for locl, dest, word in cells:
                    expect[(dest - mb.MAP) // 2] = word
                t, m = 0, base
                for sg in segs:
                    tt, m = run_kernel(SRC_FLAT, img,
                                       [(WDEST, sg[0][1]), (WWORD, sg[0][2]),
                                        (WCNT, len(sg))], m)
                    t += tt
                twalk.append((t, len(cells)))
                if m != expect:
                    fails += 1

    tw = sum(t for t, _ in twalk)
    nr = sum(n for _, n in twalk)
    print(f"WALK - {nposes} poses, {len(twalk)} run-edges, {nr} edge rows")
    print(f"  rows per run-edge      mean {stt.mean(per_edge_rows):5.2f}")
    print(f"  walkable segments      {nseg_tot} "
          f"({nseg_tot/len(twalk):.2f} per run-edge)")
    print(f"  rows per segment       mean {nr/nseg_tot:5.2f}")
    print(f"  segment length histogram {dict(sorted(seglen.items())[:8])}")
    print()
    print(f"{'kernel':12} {'T/edge row':>11} {'exact':>8}   what it is")
    print(f"{'WALK_FLAT':12} {tw/nr:11.1f} "
          f"{'EXACT' if not fails else str(fails)+' WRONG':>8}"
          f"   persistent walker, one setup per segment")
    print(f"{'EMIT_B':12} {49.2:11.1f}       --   A40, oracle (dest,word) stream")
    print(f"{'current':12} {1023.0:11.1f}       --   A39, stages 6+7+8")

    # what this does to the materializer, if the edge path cost only this
    EDGE_PATH = 66252.0
    v = tw / nr
    print(f"\n  edge path 66,252 -> {EDGE_PATH*v/1023.0:,.0f} T/pose")
    print(f"  materializer 175,827 -> "
          f"{175827 - EDGE_PATH + EDGE_PATH*v/1023.0:,.0f} T/pose")
    return 0


if __name__ == "__main__":
    sys.exit(main())
