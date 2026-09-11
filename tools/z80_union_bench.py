#!/usr/bin/env python3
"""UNION_A: price the cross-span dirty union in Z80. The first go/no-go.

A31 proved the union is mandatory - a purely local per-span skip is wrong on
76.7% of rotating pose pairs. A26 proved a classifier costing more than it
saves kills the idea. A32 counted the operations on the host and suggested it
is nothing like A26's shape, because the row extent comes from two retained
height bytes instead of being re-derived. This measures it.

VERIFICATION
------------
`temporal_boundary_probe.c` dumps, per pose pair, exactly what the kernel reads
- the retained 3-byte-per-column state of both poses - and exactly what the
host classifier produces from it, as an 18x20-bit mask. The kernel must produce
a SUPERSET of that mask: it is allowed to be conservative, never to miss a
cell. A missed cell is a wrong image, so that check hard-fails. The inflation
is reported rather than assumed away.

    make union-bench
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80                       # noqa: E402

CODE = 0x0000
NNEW, NOLD = 0xC000, 0xC001
NEWBASE, OLDBASE, DMASK = 0xC100, 0xC800, 0xCD00
STRIDE, MAXSLOT = 64, 20
ROWS, COLS = 18, 20

BASE = open(ROOT / "tools" / "temporal_union.asm").read()

# The twins differ in ONE thing: which routine marks a changed column.
# UNION_C is UNION_B with one change: table-driven marking. The twins differ
# in exactly one mechanism each, as A24/A26/A27 required.
_B = BASE.replace("UB_MARK_HOOK", "mark_span_b")
_C = _B.replace("call mark_col", "call mark_col_t") \
       .replace("jp mark_col", "jp mark_col_t")
_D = _C.replace("        jp ub_pair\n", "        jp ub_pair_d\n")
# The precheck is a no-op in every variant but E, so the twins differ in one
# thing: whether a 6-byte span record is consulted before any column work.
_NOP = "us_precheck_nop"
_E = _C.replace("US_PRECHECK_HOOK", "us_precheck")
# UNION_G is UNION_E plus one thing: the column bit and byte offset are
# computed once per column instead of once per marked range.
_G = _E.replace("        call mark_col\n", "        call mark_col_p\n") \
       .replace("        jp mark_col\n", "        jp mark_col_p\n") \
       .replace("mark_span_a:\n", "mark_span_a:\n        call mark_col_prep\n") \
       .replace("mark_span_b:\n", "mark_span_b:\n        call mark_col_prep\n")
VARIANTS = tuple(
    (n, v.replace("US_PRECHECK_HOOK", _NOP)
        if n not in ("UNION_E", "UNION_G") else v)
    for n, v in (
        ("UNION_A", BASE.replace("UB_MARK_HOOK", "mark_span_a")),
        ("UNION_B", _B),
        ("UNION_C", _C),
        ("UNION_D", _D),
        ("UNION_E", _E),
        ("UNION_G", _G),
    ))

SUMNEW, SUMOLD = 0xE200, 0xE280

COLBIT, COLBYTE, ROWBASE = 0xE000, 0xE014, 0xE028


def summaries(mem, base, spans):
    """The 6-byte span record UNION_E compares: sid, inv0, inv1, c0, c1, flags."""
    for i, sp in enumerate(spans[:MAXSLOT]):
        o = base + i * 6
        for k, v in enumerate(sp[5]):
            mem[o + k] = v


def tables(mem):
    for c in range(COLS):
        mem[COLBIT + c] = 1 << (c & 7)
        mem[COLBYTE + c] = c >> 3
    for r in range(ROWS):
        a = DMASK + 3 * r
        mem[ROWBASE + 2 * r] = a & 0xFF
        mem[ROWBASE + 2 * r + 1] = a >> 8


def parse(path):
    cases = []
    for line in path.read_text().splitlines():
        f = [int(v) for v in line.split()]
        p = 0

        def take_set():
            nonlocal p
            n = f[p]; p += 1
            spans = []
            for _ in range(n):
                keyid, c0, c1, prof, sid, inv0, inv1, fl = f[p:p + 8]; p += 8
                cols = {}
                for c in range(c0, c1 + 1):
                    cols[c] = tuple(f[p:p + 3]); p += 3
                spans.append((keyid, c0, c1, prof, cols,
                              (sid, inv0, inv1, c0, c1, fl)))
            return spans

        cur = take_set()
        prev = take_set()
        mask = []
        for _ in range(ROWS):
            lo, mid, hi = f[p:p + 3]; p += 3
            mask.append(lo | (mid << 8) | (hi << 16))
        cases.append((cur, prev, mask))
    return cases


def lay(mem, base, spans):
    for i, (keyid, c0, c1, prof, cols, _sum) in enumerate(spans[:MAXSLOT]):
        o = base + i * STRIDE
        mem[o] = keyid
        mem[o + 1] = c0
        mem[o + 2] = c1
        mem[o + 3] = prof
        for c, (hl, hr, bd) in cols.items():
            mem[o + 4 + 3 * c] = hl
            mem[o + 4 + 3 * c + 1] = hr
            mem[o + 4 + 3 * c + 2] = bd


def main():
    dump = ROOT / "build" / "temporal_union_oracle.txt"
    if not dump.exists():
        raise SystemExit(f"missing {dump} - run `make union-bench`")
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 600
    cases = parse(dump)
    step = max(1, len(cases) // limit)
    cases = cases[::step]

    results = {}
    for name, src in VARIANTS:
        code, _ = assemble(src, CODE)
        img = bytearray(0x10000)
        img[CODE:CODE + len(code)] = code
        tables(img)

        ts, misses, want_cells, got_cells, over = [], 0, 0, 0, 0
        for cur, prev, want in cases:
            mem = bytearray(img)
            assert len(cur) <= MAXSLOT and len(prev) <= MAXSLOT, "slot overflow"
            mem[NNEW] = len(cur)
            mem[NOLD] = len(prev)
            lay(mem, NEWBASE, cur)
            lay(mem, OLDBASE, prev)
            summaries(mem, SUMNEW, cur)
            summaries(mem, SUMOLD, prev)
            cpu = Z80(mem)
            cpu.run(CODE)
            ts.append(cpu.t)
            for r in range(ROWS):
                b = cpu.m[DMASK + 3 * r: DMASK + 3 * r + 3]
                g = b[0] | (b[1] << 8) | (b[2] << 16)
                w = want[r]
                want_cells += bin(w).count("1")
                got_cells += bin(g).count("1")
                if w & ~g:
                    misses += bin(w & ~g).count("1")
                over += bin(g & ~w).count("1")
        ts.sort()
        n = len(ts)
        results[name] = dict(
            bytes=len(code), mean=stt.mean(ts), p95=ts[int(0.95 * n)],
            mx=ts[-1], mn=ts[0], marks=got_cells / n,
            host=want_cells / n, over=100.0 * over / want_cells,
            misses=misses)
        if misses:
            print(f"{name}: {len(code)} bytes - *** MISSES {misses} dirty"
                  f" cells, NOT CORRECT ***")
        else:
            print(f"{name}: {len(code)} bytes - superset verified,"
                  f" 0 missed cells")

    print(f"\n{len(cases)} pose pairs, U=1 rotation corpus\n")
    print(f"{'variant':10} {'T/update':>10} {'p95':>9} {'max':>9} {'min':>8}"
          f" {'marks':>8} {'over':>8} {'exact':>7}")
    for name in results:
        r = results[name]
        print(f"{name:10} {r['mean']:10.1f} {r['p95']:9d} {r['mx']:9d}"
              f" {r['mn']:8d} {r['marks']:8.2f} {r['over']:7.0f}%"
              f" {'yes' if not r['misses'] else 'NO':>7}")
    print("  An unverified variant's T is not a result - it is what an"
          " incorrect kernel costs.")
    a, b = results["UNION_A"], results["UNION_B"]
    print(f"\n  host classifier's exact dirty set: {a['host']:.2f} cells/update")
    print(f"  UNION_B vs UNION_A: {100.0 * b['mean'] / a['mean'] - 100:+.1f}% T,"
          f" {b['marks'] / a['marks']:.2f}x the marks")
    print(f"\n  against the DDA_G sequence baseline of 153,450 T/update:")
    for name in results:
        if results[name]['misses']:
            continue
        print(f"    {name} costs {100.0 * results[name]['mean'] / 153450.0:5.1f}%"
              f" of a full render before any drawing happens")
    print("  A26's classifier alone was +26.3% of its baseline, and that is the"
          "\n  bar this has to stay under to be worth continuing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
