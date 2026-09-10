#!/usr/bin/env python3
"""DDA_G: stop dividing to find rows, and stop recomputing the sub-row offset.

WHAT THE PROFILE SAID
---------------------
After the A24 ladder the edge path was the largest coherent target: `de_loop`
+ `edge_entry` + `ee_hi_ok` + `shr3_u` + `de_yl_min` together were ~27% of the
kernel, with no single routine dominating. Two things account for most of it,
and both are recomputation of something already known:

1. **Row extents.** Each column derived rows SIX times - both edges and the
   interior, each with two signed 16-bit compares and two `rowfloor` calls.
   Every endpoint is a monotonic function of one height byte, so one byte
   compare per column picks the min/max ends and the rows follow by three
   shifts. `col_bounds` does that once per column.

2. **The sub-row offset.** `local_left = YL - (r<<3)` was rebuilt every row
   from the row index: load, three doublings, a 16-bit subtract. It is an
   affine walk - each row is exactly 8 less than the previous - so it is
   carried and decremented instead. That is the DDA proper.

`rowfloor`'s negative branch disappears outright: a negative lower bound
clamps to row 0 and a negative upper bound means nothing to draw, so the
shift only ever sees a non-negative value.

VERIFICATION
------------
Pose scope, against `build/coverage_pose_oracle.txt` - every run of a pose
against the final 20x18 name table. The baseline is imported from the run
bench so the A/B cannot drift, and MASKED_H re-runs the coverage question
from A26 on top of DDA, since A26's verdict was explicitly conditional on the
row extent being expensive.

    make dda-bench
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble                            # noqa: E402
import z80_materialize_run_bench as rb                  # noqa: E402
import z80_materialize_masked_bench as mb               # noqa: E402

CODE = 0x0000
ROWS, COLS = rb.ROWS, rb.COLS

DDA_ASM = open(ROOT / "tools" / "materialize_dda.asm").read()

BASE = rb.SRC_FILLLOOP


def _cut(src, start, end, repl, what):
    """Replace the text from `start` up to (and including) `end`."""
    i = src.index(start)
    j = src.index(end, i) + len(end)
    assert i >= 0 and j > i, what
    return src[:i] + repl + src[j:]


def build_dda(src):
    # 1. classify the column's rows once, before anything is drawn
    # col_bounds must run before ANYTHING reads the extent - including the
    # coverage classifier in MASKED_H, which is emitted ahead of the draws.
    head = "        call cov_prep\n" if "        call cov_prep\n" in src \
        else mb._DRAW_HEAD
    assert head in src
    src = src.replace(head, "        call col_bounds\n" + head, 1)

    # 2. draw_edge takes its rows from col_bounds instead of deriving them
    src = _cut(src, "de_rows2:", "de_r1ok:", """de_rows2:
        ld a,(0xc01c)                ; BOTTOM selects which pair to use
        or a
        jp nz,de_rows_bottom
        ld a,(0xc050)
        ld (0xc01f),a
        ld a,(0xc051)
        ld (0xc020),a
        jp de_rows_have
de_rows_bottom:
        ld a,(0xc052)
        ld (0xc01f),a
        ld a,(0xc053)
        ld (0xc020),a
de_rows_have:
de_r1ok:""", "draw_edge row block")

    # 3. draw_full likewise
    src = _cut(src, "\ndraw_full:", "df_r1ok:", """
draw_full:
        ld a,(0xc054)
        ld (0xc01f),a
        ld a,(0xc055)
        ld (0xc020),a
df_r1ok:""", "draw_full row block")

    # 4. the DDA proper: carry local_left down the rows instead of rebuilding
    #    it. edge_entry OVERWRITES LOCL (7-locl for the bottom edge, then
    #    -= mag), so the carried value lives in LOCC and is copied in.
    old = """        call row_addr
        ld (0xc03a),hl
de_loop:
; local_left = YL - (r<<3)
        ld a,(0xc01f)
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ex de,hl
        ld hl,(0xc018)
        or a
        sbc hl,de
        ld (0xc022),hl
        call edge_entry"""
    assert old in src, "de_loop head moved"
    src = src.replace(old, """        call row_addr
        ld (0xc03a),hl
        ld a,(0xc01f)                ; LOCC = YL - (r0<<3), computed ONCE
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ex de,hl
        ld hl,(0xc018)
        or a
        sbc hl,de
        ld (0xc056),hl
de_loop:
        ld hl,(0xc056)
        ld (0xc022),hl
        call edge_entry""", 1)

    old_tail = """        ld a,(0xc01f)
        ld b,a
        ld a,(0xc020)
        cp b
        ret z
        ld a,b
        inc a
        ld (0xc01f),a
        jp de_loop"""
    assert old_tail in src, "de_loop tail moved"
    src = src.replace(old_tail, """        ld a,(0xc01f)
        ld b,a
        ld a,(0xc020)
        cp b
        ret z
        ld a,b
        inc a
        ld (0xc01f),a
        ld hl,(0xc056)               ; one row down is exactly 8 less
        ld de,8
        or a
        sbc hl,de
        ld (0xc056),hl
        jp de_loop""", 1)
    return src + DDA_ASM


SRC_DDA = build_dda(BASE)

# MASKED_H: DDA plus the A26 coverage machinery, to re-decide coverage now
# that knowing a column's rows is nearly free. col_bounds already produced the
# extent, so the classifier reads it instead of deriving it again.
_COV_H = """cov_prep:
        ld a,(0xc058)                ; MASKLO - col_bounds already knows lo
        ld (0xc040),a
        cp 18
        jp c,ch_lo_ok
        xor a
        ld (0xc048),a
        ret
ch_lo_ok:
        ld a,(0xc053)                ; BOTR1 - and hi
        ld (0xc041),a
        ld b,a
        ld a,(0xc040)
        cp b
        jp c,ch_rows_ok
        jp z,ch_rows_ok
        xor a
        ld (0xc048),a
        ret
ch_rows_ok:
"""
SRC_MASKED_H = build_dda(mb.SRC_MASKED[:-len(mb.COV_ASM)])
SRC_MASKED_H += _COV_H + mb._SPLIT + mb.COV_ASM.split(mb._SPLIT, 1)[1]


def profile(src, cases, bgm, low, high, near, top=14):
    """Attribute T to the nearest preceding label. The user's standing rule
    after A24 was to re-profile after every change rather than assume the
    ranking holds, and it did not: the edge row-derivation that dominated
    before DDA is gone, so what is left has to be measured, not guessed."""
    from z80core import Z80
    code, labels = assemble(src, CODE)
    marks = sorted((v, k) for k, v in labels.items())
    img = bytearray(0x10000)
    img[CODE:CODE + len(code)] = code
    img[mb.LOWTAB:mb.LOWTAB + len(low)] = low
    img[mb.HIGHTAB:mb.HIGHTAB + len(high)] = high

    import bisect
    addrs = [a for a, _ in marks]
    names = [n for _, n in marks]
    cost = {}

    class P(Z80):
        def _step(self):
            pc, t0 = self.pc, self.t
            Z80._step(self)
            i = bisect.bisect_right(addrs, pc) - 1
            nm = names[i] if i >= 0 else "<top>"
            cost[nm] = cost.get(nm, 0) + (self.t - t0)

    total = 0
    for runs, want in cases:
        mem = bytearray(img)
        for i, v in enumerate(bgm):
            mem[mb.MAP + 2 * i] = v & 0xFF
            mem[mb.MAP + 2 * i + 1] = v >> 8
        order = runs if near else list(reversed(runs))
        for iq, stp, c0, c1, prof, lr, rr, sh in order:
            mem[rb.IQ] = iq & 0xFF; mem[rb.IQ + 1] = (iq >> 8) & 0xFF
            mem[rb.STEP] = stp & 0xFF; mem[rb.STEP + 1] = (stp >> 8) & 0xFF
            mem[rb.PROFILE], mem[rb.C0], mem[rb.C1] = prof, c0, c1
            mem[rb.LREAL], mem[rb.RREAL], mem[rb.SHADE] = lr, rr, sh
            cpu = P(mem)
            cpu.run(CODE)
            total += cpu.t
            mem = cpu.m
    print(f"\n=== DDA_G profile ({len(cases)} poses, {total:,} T) ===")
    for nm, t in sorted(cost.items(), key=lambda kv: -kv[1])[:top]:
        print(f"  {nm:16s} {t:12,}  {100.0*t/total:5.1f}%")


def main():
    dump = ROOT / "build" / "coverage_pose_oracle.txt"
    if not dump.exists():
        raise SystemExit(f"missing {dump} - run `make coverage-potential` first")
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    lines = [l.split() for l in dump.read_text().splitlines() if l.strip()]
    stepn = max(1, len(lines) // limit)
    cases = []
    for f in lines[::stepn]:
        n = int(f[0])
        runs = [tuple(int(v) for v in f[1 + 8 * k: 9 + 8 * k]) for k in range(n)]
        want = [int(v) for v in f[1 + 8 * n:]]
        cases.append((runs, want))

    low, high = mb.tables()
    bgm = mb.background_map()
    results = {}
    print(f"pose oracle: {len(cases)} poses strided across {len(lines)}\n")

    for name, src, near in (("FILLLOOP_D", BASE, False),
                            ("DDA_G", SRC_DDA, False),
                            ("MASKED_H", SRC_MASKED_H, True)):
        code, _ = assemble(src, CODE)
        img = bytearray(0x10000)
        img[CODE:CODE + len(code)] = code
        img[mb.LOWTAB:mb.LOWTAB + len(low)] = low
        img[mb.HIGHTAB:mb.HIGHTAB + len(high)] = high
        fails, ts = 0, []
        for runs, want in cases:
            t, got = mb.run_pose(img, runs, near, bgm)
            ts.append(t)
            if got != want:
                fails += 1
                if fails <= 2:
                    bad = [i for i in range(ROWS * COLS) if got[i] != want[i]]
                    print(f"  MISMATCH {name} cells {len(bad)} first "
                          f"r{bad[0]//COLS} c{bad[0]%COLS} "
                          f"want {want[bad[0]]} got {got[bad[0]]}")
        if fails:
            raise SystemExit(f"FAIL: {name}: {fails}/{len(cases)} poses wrong")
        results[name] = (stt.mean(ts), len(code))
        print(f"{name:11s} VERIFIED {len(cases)}/{len(cases)} poses exact   "
              f"{stt.mean(ts):9,.0f} T/update  ({len(code)} bytes)")

    b = results["FILLLOOP_D"][0]
    print(f"\n=== A/B ===")
    for nm in ("DDA_G", "MASKED_H"):
        print(f"  {nm:10s} {results[nm][0]:9,.0f} T/update  "
              f"{(results[nm][0]/b - 1):+.1%} vs FILLLOOP_D")
    profile(SRC_DDA, cases[:40], bgm, low, high, False)

    other = 5833.0 + 11036.0 + 2339.0 + 26820.0 + 1314.0
    for nm, (t, _) in results.items():
        print(f"{nm:11s} whole update {t+other:11,.0f} T   "
              f"{59736.0/(t+other):.2f} updates/frame")


if __name__ == "__main__":
    main()
