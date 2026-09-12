#!/usr/bin/env python3
"""TEMP_BOUNDARY_A: the complete exact temporal update, measured end to end.

The decisive question. Union + executor + retained-state maintenance as ONE
number, against the verified full-render sequence baseline of 153,450 T/update,
with the whole 20x18 name table required to match exactly.

WHAT IS MEASURED
----------------
Starting from the PREVIOUS frame's name table:
  1. UNION_E builds the dirty mask from the retained span state
  2. the executor collapses that to dirty columns, resets them to background,
     and replays every run over its dirty sub-ranges through the VERIFIED
     DDA_G materializer
  3. the result must equal the reference renderer's name table for the new pose

Both stages run in one Z80 image and their T-states are summed. Nothing is
inferred from cell counts - A29 already showed why that misleads.

    make temporal-exec
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80                       # noqa: E402
import z80_materialize_masked_bench as mb               # noqa: E402
import z80_materialize_dda_bench as dda                 # noqa: E402
import z80_union_bench as ub                            # noqa: E402

ROWS, COLS = 18, 20
BASEWORD = 0xE080
RUNCNT, RUNARR = 0xDC00, 0xDC10
BASELINE = 153450.0


def build_image():
    """One image holding: entry stub, union, executor, DDA_G as a subroutine."""
    dsrc = dda.SRC_DDA
    assert dsrc.count("        halt\n") == 1, "DDA_G's single halt is its return"
    dsrc = dsrc.replace("        halt\n", "        ret\n")

    union = ub.VARIANTS_BY_NAME["UNION_E"]
    union = union.replace("        call union_build\n        halt\n", "")

    ex = (ROOT / "tools" / "temporal_exec.asm").read_text()
    ex = ex.replace("        jp MATERIALIZE_ENTRY", "        jp materialize")

    src = ("        call union_build\n"
           "        call exec_entry\n"
           "        halt\n"
           + union + "\n" + ex + "\nmaterialize:\n" + dsrc)
    code, labels = assemble(src, 0)
    return code, labels


def base_word(r):
    if r < 9:
        return 0          # TSP_TILE_CEILING
    if r == 9:
        return 2          # TSP_TILE_HORIZON
    return 1              # TSP_TILE_FLOOR


def parse(path, limit):
    """cur spans, prev spans, dirty mask, runs, prev map, cur map."""
    out = []
    for line in path.read_text().splitlines():
        f = [int(v) for v in line.split()]
        p = 0

        def take_spans():
            nonlocal p
            n = f[p]; p += 1
            spans = []
            for _ in range(n):
                keyid, c0, c1, prof, sid, i0, i1, fl = f[p:p + 8]; p += 8
                cols = {}
                for c in range(c0, c1 + 1):
                    cols[c] = tuple(f[p:p + 3]); p += 3
                spans.append((keyid, c0, c1, prof, cols, (sid, i0, i1, c0, c1, fl)))
            return spans

        cur, prev = take_spans(), take_spans()
        p += 3 * ROWS                      # the host mask; the kernel rebuilds it
        nr = f[p]; p += 1
        runs = []
        for _ in range(nr):
            runs.append(tuple(f[p:p + 8])); p += 8
        pmap = f[p:p + ROWS * COLS]; p += ROWS * COLS
        cmap = f[p:p + ROWS * COLS]; p += ROWS * COLS
        out.append((cur, prev, runs, pmap, cmap))
    step = max(1, len(out) // limit)
    return out[::step]


def main():
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    code, labels = build_image()
    low, high = mb.tables()

    img = bytearray(0x10000)
    img[0:len(code)] = code
    img[mb.LOWTAB:mb.LOWTAB + len(low)] = low
    img[mb.HIGHTAB:mb.HIGHTAB + len(high)] = high
    ub.tables(img)
    for r in range(ROWS):
        w = base_word(r)
        img[BASEWORD + 2 * r] = w & 0xFF
        img[BASEWORD + 2 * r + 1] = w >> 8

    print(f"TEMP_BOUNDARY_A: {len(code)} bytes "
          f"(union + executor + DDA_G in one image)\n")

    for label, name in (("U=1 all regimes", "temporal_exec_all.txt"),
                        ("U=1 rotation", "temporal_exec_rot.txt")):
        dump = ROOT / "build" / name
        if not dump.exists():
            raise SystemExit(f"missing {dump} - run `make temporal-exec`")
        cases = parse(dump, limit)
        ts, bad_cells, bad_updates = [], 0, 0
        for cur, prev, runs, pmap, cmap in cases:
            mem = bytearray(img)
            mem[ub.NNEW], mem[ub.NOLD] = len(cur), len(prev)
            ub.lay(mem, ub.NEWBASE, cur)
            ub.lay(mem, ub.OLDBASE, prev)
            ub.summaries(mem, ub.SUMNEW, cur)
            ub.summaries(mem, ub.SUMOLD, prev)
            mem[RUNCNT] = len(runs)
            for i, (iq, stp, c0, c1, prof, lr, rr, sh) in enumerate(runs):
                o = RUNARR + 10 * i
                mem[o] = iq & 0xFF; mem[o + 1] = (iq >> 8) & 0xFF
                mem[o + 2] = stp & 0xFF; mem[o + 3] = (stp >> 8) & 0xFF
                mem[o + 4], mem[o + 5] = c0, c1
                mem[o + 6], mem[o + 7], mem[o + 8], mem[o + 9] = prof, lr, rr, sh
            for i, v in enumerate(pmap):
                mem[mb.MAP + 2 * i] = v & 0xFF
                mem[mb.MAP + 2 * i + 1] = v >> 8
            cpu = Z80(mem)
            cpu.run(0)
            ts.append(cpu.t)
            got = [cpu.m[mb.MAP + 2 * i] | (cpu.m[mb.MAP + 2 * i + 1] << 8)
                   for i in range(ROWS * COLS)]
            d = sum(1 for a, b in zip(got, cmap) if a != b)
            if d:
                bad_cells += d
                bad_updates += 1
        ts.sort()
        n = len(ts)
        mean, med = stt.mean(ts), ts[n // 2]
        p95, mx = ts[int(0.95 * n)], ts[-1]
        over = sum(1 for t in ts if t > BASELINE)
        tag = "EXACT" if not bad_cells else f"*** {bad_cells} WRONG CELLS ***"
        print(f"{label}   {n} updates   {tag}")
        print(f"  mean {mean:9.0f} T  ({100 * mean / BASELINE:5.1f}% of a render,"
              f" {BASELINE / mean:4.2f}x)")
        print(f"  med  {med:9d} T  ({100 * med / BASELINE:5.1f}%)")
        print(f"  p95  {p95:9d} T  ({100 * p95 / BASELINE:5.1f}%)")
        print(f"  max  {mx:9d} T  ({100 * mx / BASELINE:5.1f}%)")
        print(f"  updates costing MORE than a full render: {over}/{n}"
              f"  = {100.0 * over / n:.1f}%")
        if bad_updates:
            print(f"  updates with any wrong cell: {bad_updates}/{n}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
