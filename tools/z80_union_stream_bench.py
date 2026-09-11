#!/usr/bin/env python3
"""UNION_S: the dirty union over a SPAN STREAM, against A33's column form.

A33 measured the retained-column union at 35-55% of a full render and put the
cost in per-column iteration and presence testing rather than in any real work.
The span stream removes both: a span's columns are contiguous, so there is no
presence test, and one 6-byte record compare proves every column of that span
matches.

VERIFICATION, and it is stricter than A33's
-------------------------------------------
A33 checked its mask against the host classifier's own conservative mask, which
measures agreement with one particular classifier. This checks against the set
of cells that ACTUALLY changed between the two rendered frames. The kernel's
mask must be a superset of that: conservative is allowed, missing a cell is a
wrong image.

    make union-stream
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
NEWBASE, OLDBASE, DMASK = 0xC100, 0xC200, 0xCD00
STRIDE, MAXSLOT = 8, 20
ROWS, COLS = 18, 20
COLBIT, COLBYTE, ROWBASE, RECIP = 0xE000, 0xE014, 0xE028, 0xE100

# The marking routines live in the column kernel; take them verbatim so the two
# benches cannot drift apart on how a row range becomes mask bits.
_COL = (ROOT / "tools" / "temporal_union.asm").read_text()
SHARED = _COL[_COL.index("; ---- UNION_A's marking"):]
SRC = (ROOT / "tools" / "temporal_union_stream.asm").read_text() + "\n" + SHARED
SRC = SRC.replace("UB_MARK_HOOK", "mark_span_a")

# Two twins, differing in one thing: how a changed column's rows are marked.
VARIANTS = (
    ("UNION_S_A", SRC.replace("US_MARK_HOOK", "mark_span_a")),
    ("UNION_S_B", SRC.replace("US_MARK_HOOK", "mark_span_b")),
)

# k_col_recip_q8, read out of the generated data so the walk matches draw_run.
def recip_table():
    import re
    txt = (ROOT / "src" / "tilesector_polar_renderer.c").read_text()
    i = txt.index("k_col_recip_q8[21]")
    j = txt.index("}", i)
    return [int(v) for v in re.findall(r"-?\d+", txt[txt.index("{", i) + 1:j])]


def parse(path):
    cases = []
    for line in path.read_text().splitlines():
        f = [int(v) for v in line.split()]
        p = 0

        def take():
            nonlocal p
            n = f[p]; p += 1
            out = []
            for _ in range(n):
                out.append(tuple(f[p:p + 8])); p += 8
            return out

        cur, prev = take(), take()
        mask = []
        for _ in range(ROWS):
            lo, mid, hi = f[p:p + 3]; p += 3
            mask.append(lo | (mid << 8) | (hi << 16))
        cases.append((cur, prev, mask))
    return cases


def lay(mem, base, spans):
    for i, rec in enumerate(spans[:MAXSLOT]):
        o = base + i * STRIDE
        for k, v in enumerate(rec):
            mem[o + k] = v


def main():
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    recip = recip_table()

    base = bytearray(0x10000)
    for c in range(COLS):
        base[COLBIT + c] = 1 << (c & 7)
        base[COLBYTE + c] = c >> 3
    for r in range(ROWS):
        a = DMASK + 3 * r
        base[ROWBASE + 2 * r] = a & 0xFF
        base[ROWBASE + 2 * r + 1] = a >> 8
    for i, v in enumerate(recip[:32]):
        base[RECIP + i] = v & 0xFF

    corpora = (("U=1 rotation", "span_stream_rot.txt"),
               ("U=1 all regimes", "span_stream_all.txt"))
    a33 = {"U=1 rotation": 84307.0, "U=1 all regimes": 53282.0}
    rows = []
    for vname, vsrc in VARIANTS:
        code, _ = assemble(vsrc, CODE)
        img = bytearray(base)
        img[CODE:CODE + len(code)] = code
        for label, name in corpora:
            dump = ROOT / "build" / name
            if not dump.exists():
                raise SystemExit(f"missing {dump} - run `make union-stream`")
            cases = parse(dump)
            cases = cases[::max(1, len(cases) // limit)]
            ts, misses, want, got = [], 0, 0, 0
            for cur, prev, mask in cases:
                mem = bytearray(img)
                assert len(cur) <= MAXSLOT and len(prev) <= MAXSLOT
                mem[NNEW], mem[NOLD] = len(cur), len(prev)
                lay(mem, NEWBASE, cur)
                lay(mem, OLDBASE, prev)
                cpu = Z80(mem)
                cpu.run(CODE)
                ts.append(cpu.t)
                for r in range(ROWS):
                    b = cpu.m[DMASK + 3 * r: DMASK + 3 * r + 3]
                    g = b[0] | (b[1] << 8) | (b[2] << 16)
                    w = mask[r]
                    want += bin(w).count("1")
                    got += bin(g).count("1")
                    if w & ~g:
                        misses += bin(w & ~g).count("1")
            ts.sort()
            n = len(ts)
            rows.append((vname, label, len(code), stt.mean(ts),
                         ts[int(0.95 * n)], ts[-1], got / n, want / n,
                         misses, n))

    print(f"{'variant':10} {'corpus':18} {'T/update':>10} {'p95':>9} {'max':>9}"
          f" {'marks':>7} {'changed':>8} {'exact':>6}")
    for v, lab, nb, m, p95, mx, marks, want, miss, n in rows:
        print(f"{v:10} {lab:18} {m:10.1f} {p95:9d} {mx:9d} {marks:7.2f}"
              f" {want:8.2f} {'yes' if not miss else 'NO':>6}")
    print("  changed = cells that actually differ between the two frames."
          " marks must cover them.")

    print(f"\n  against DDA_G's 153,450 T/update and A33's column union:")
    for v, lab, nb, m, p95, mx, marks, want, miss, n in rows:
        if miss:
            continue
        print(f"    {v} {lab:18} {100.0 * m / 153450.0:5.1f}% of a render"
              f"   vs UNION_C {100.0 * a33[lab] / 153450.0:5.1f}%"
              f"   = {a33[lab] / m:.2f}x")
        print(f"    {'':10} {'':18} p95 {100.0 * p95 / 153450.0:5.1f}%")
    return 1 if all(r[8] for r in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
