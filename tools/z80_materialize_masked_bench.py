#!/usr/bin/env python3
"""MASKED_E: near->far traversal with a per-column coverage mask.

WHY A POSE-SCOPED BENCH
-----------------------
Every earlier materializer bench verified ONE run against a per-run oracle.
Coverage cannot be checked that way: the entire mechanism is state carried
across runs, so the unit of verification has to be a whole pose - all of its
runs, in near->far order, against the final 20x18 name table.

`build/coverage_pose_oracle.txt` is that oracle, dumped by
`coverage_potential_probe.c`, whose near->far masked pass is already proven
bit-identical to the far->near host image on 29,824/29,824 poses.

THE A/B
-------
Both variants use the SAME kernel (FILLLOOP_D, imported from the run bench so
it cannot drift), the same helpers and the same map addressing:

  FAR_NEAR_D   runs replayed far->near, no mask       - what ships today
  MASKED_E     runs replayed near->far, coverage mask - the candidate

so the difference is the traversal order and the mask, and nothing else.

WHAT THE HOST MEASUREMENT SAID TO EXPECT (A25)
----------------------------------------------
Per update: 3.16 columns fully occluded (10.9%), 22.96 with nothing owned
(79.5%), 2.78 partially overlapping (9.6%). Only that last 9.6% has to pay
for per-cell masking, which is why this kernel classifies the column once and
then draws in one of three ways rather than gating every store.

    make masked-bench
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80                      # noqa: E402
import z80_materialize_run_bench as rb                 # noqa: E402

CODE = 0x0000
IQ, STEP, PROFILE = rb.IQ, rb.STEP, rb.PROFILE
C0, C1, LREAL, RREAL, SHADE = rb.C0, rb.C1, rb.LREAL, rb.RREAL, rb.SHADE
MAP, ROWS, COLS = rb.MAP, rb.ROWS, rb.COLS

COV, STASH, FLAGS = 0xC600, 0xC700, 0xC740
LOWTAB, HIGHTAB = 0xC800, 0xC880

COV_ASM = open(ROOT / "tools" / "materialize_coverage.asm").read()
COV_FAST = open(ROOT / "tools" / "materialize_coverage_fast.asm").read()

# Everything from the mask tables down is shared; only the classifier's front
# half - how lo and hi are obtained - differs between MASKED_E and MASKED_F.
_SPLIT = "\n; RANGE = LOWTAB[lo] & HIGHTAB[hi]"
assert _SPLIT in COV_ASM, "coverage asm split marker moved"
COV_ASM_FAST = COV_FAST + _SPLIT + COV_ASM.split(_SPLIT, 1)[1]

SRC_FAR = rb.SRC_FILLLOOP

# The three-way column classifier wraps the draws; the skip path still has to
# run the endpoint carry, so it lands on the tail rather than the loop top.
_DRAW_HEAD = """        ld hl,(0xc010)
        ld (0xc018),hl
        ld hl,(0xc012)
        ld (0xc01a),hl
        xor a
        ld (0xc01c),a
        call draw_edge"""
assert _DRAW_HEAD in SRC_FAR, "draw head anchor moved"
SRC_MASKED = SRC_FAR.replace(_DRAW_HEAD, """        call cov_prep
        ld a,(0xc048)
        or a
        jp z,cov_skip_col
        cp 2
        jp nz,cov_do_draw
        call cov_stash
cov_do_draw:
""" + _DRAW_HEAD, 1)

_DRAW_TAIL = """        call draw_full

; ---- THE CARRY"""
assert _DRAW_TAIL in SRC_MASKED, "draw tail anchor moved"
SRC_MASKED = SRC_MASKED.replace(_DRAW_TAIL, """        call draw_full
        ld a,(0xc048)
        cp 2
        jp nz,cov_no_restore
        call cov_restore
cov_no_restore:
        call cov_mark
cov_skip_col:

; ---- THE CARRY""", 1)
SRC_MASKED += COV_ASM

# PREP_ONLY: run the classifier, then throw its verdict away and draw every
# column unmasked. It cannot skip or stash, so it measures the CLASSIFIER's
# cost alone - the difference between it and FAR_NEAR_D is what every column
# pays before coverage can save anything.
#
# It is replayed FAR->NEAR, so it still has to reproduce the name table
# exactly. Running an unmasked kernel near->far would produce a wrong image
# by construction (the nearest run gets overwritten) and could not be
# verified, which would make its timing worth nothing.
SRC_MASKED_F = SRC_MASKED[:-len(COV_ASM)] + COV_ASM_FAST

SRC_PREPONLY = SRC_MASKED.replace("""        call cov_prep
        ld a,(0xc048)""", """        call cov_prep
        ld a,1
        ld (0xc048),a
        ld a,(0xc048)""", 1)


def tables():
    """LOWTAB[r] = bits r..17 set, HIGHTAB[r] = bits 0..r set, stride 4 so the
    index is two `add hl,hl` rather than a multiply."""
    low, high = bytearray(18 * 4), bytearray(18 * 4)
    for r in range(18):
        lo = sum(1 << b for b in range(r, 18))
        hi = sum(1 << b for b in range(0, r + 1))
        for i in range(3):
            low[r * 4 + i] = (lo >> (8 * i)) & 0xFF
            high[r * 4 + i] = (hi >> (8 * i)) & 0xFF
    return low, high


def background_map():
    return rb.background_map()


def run_pose(code_img, runs, near_first, bgm):
    """Replay one pose. Memory persists across runs - the coverage mask lives
    there, which is the whole point."""
    mem = bytearray(code_img)
    for i, v in enumerate(bgm):
        mem[MAP + 2 * i] = v & 0xFF
        mem[MAP + 2 * i + 1] = v >> 8
    for i in range(COLS * 3):
        mem[COV + i] = 0
    order = runs if near_first else list(reversed(runs))
    total = 0
    for iq, stp, c0, c1, prof, lr, rr, sh in order:
        mem[IQ] = iq & 0xFF; mem[IQ + 1] = (iq >> 8) & 0xFF
        mem[STEP] = stp & 0xFF; mem[STEP + 1] = (stp >> 8) & 0xFF
        mem[PROFILE], mem[C0], mem[C1] = prof, c0, c1
        mem[LREAL], mem[RREAL], mem[SHADE] = lr, rr, sh
        cpu = Z80(mem)
        cpu.run(CODE)
        total += cpu.t
        mem = cpu.m
    got = [mem[MAP + 2 * i] | (mem[MAP + 2 * i + 1] << 8)
           for i in range(ROWS * COLS)]
    return total, got


def main():
    dump = ROOT / "build" / "coverage_pose_oracle.txt"
    if not dump.exists():
        raise SystemExit(f"missing {dump} - run `make coverage-potential` first")
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 400
    lines = [l.split() for l in dump.read_text().splitlines() if l.strip()]
    stepn = max(1, len(lines) // limit)
    cases = []
    for f in lines[::stepn]:
        n = int(f[0])
        runs = []
        for k in range(n):
            runs.append(tuple(int(v) for v in f[1 + 8 * k: 9 + 8 * k]))
        want = [int(v) for v in f[1 + 8 * n:]]
        assert len(want) == ROWS * COLS, len(want)
        cases.append((runs, want))

    low, high = tables()
    bgm = background_map()
    results = {}
    print(f"pose oracle: {len(cases)} poses strided across {len(lines)}")
    print(f"runs/pose: {stt.mean(len(c[0]) for c in cases):.2f}\n")

    for name, src, near in (("FAR_NEAR_D", SRC_FAR, False),
                            ("PREP_ONLY", SRC_PREPONLY, False),
                            ("MASKED_E", SRC_MASKED, True),
                            ("MASKED_F", SRC_MASKED_F, True)):
        code, _ = assemble(src, CODE)
        img = bytearray(0x10000)
        img[CODE:CODE + len(code)] = code
        img[LOWTAB:LOWTAB + len(low)] = low
        img[HIGHTAB:HIGHTAB + len(high)] = high
        fails = 0
        ts, cols = [], 0
        for runs, want in cases:
            t, got = run_pose(img, runs, near, bgm)
            ts.append(t)
            cols += sum(r[3] - r[2] + 1 for r in runs)
            if got != want:
                fails += 1
                if fails <= 2:
                    bad = [i for i in range(ROWS * COLS) if got[i] != want[i]]
                    print(f"  MISMATCH cells {len(bad)} first "
                          f"r{bad[0]//COLS} c{bad[0]%COLS} "
                          f"want {want[bad[0]]} got {got[bad[0]]}")
        if fails:
            raise SystemExit(f"FAIL: {name}: {fails}/{len(cases)} poses wrong "
                             f"- {len(code)} bytes")
        per_pose = stt.mean(ts)
        results[name] = (per_pose, cols / len(cases), len(code))
        print(f"{name:11s} VERIFIED {len(cases)}/{len(cases)} poses exact   "
              f"{per_pose:9,.0f} T/update  ({len(code)} bytes)")

    fn, mk = results["FAR_NEAR_D"][0], results["MASKED_E"][0]
    pp = results["PREP_ONLY"][0]
    print(f"\n=== where the coverage cost goes ===")
    print(f"  classifier alone (PREP_ONLY)  {pp-fn:+9,.0f} T/update  "
          f"{(pp/fn - 1):+.1%}")
    print(f"  skipping + stash/restore      {mk-pp:+9,.0f} T/update  "
          f"{(mk/pp - 1):+.1%}")
    mf = results["MASKED_F"][0]
    print(f"\n=== A/B: does the coverage mask pay? ===")
    print(f"  MASKED_F     {mf:9,.0f} T/update  {(mf/fn - 1):+.1%}  "
          f"(cheap classifier)")
    print(f"  FAR_NEAR_D   {fn:9,.0f} T/update  (ships today)")
    print(f"  MASKED_E     {mk:9,.0f} T/update  {(mk/fn - 1):+.1%}")
    print(f"\ncode size {results['FAR_NEAR_D'][2]} -> {results['MASKED_E'][2]} "
          f"bytes (+{results['MASKED_E'][2]-results['FAR_NEAR_D'][2]})")
    other = 5833.0 + 11036.0 + 2339.0 + 26820.0 + 1314.0
    for nm, (t, _, _) in results.items():
        print(f"{nm:11s} whole update {t+other:11,.0f} T   "
              f"{59736.0/(t+other):.2f} updates/frame")


if __name__ == "__main__":
    main()
