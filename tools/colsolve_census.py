"""COLSOLVE_CENSUS_A: split the column-solve stage into measured sub-costs.

A14 measured the complete chain - `wall_d_q4 -> inv_for_dq4 -> inv_at_invd x2`,
plus `angle_x` and the iq/step derivation - as one cycle-exact 880-byte kernel
at 6,243 T/span, 26,845 T/update.  That is now the largest single stage outside
the materializer and nothing has ever split it.

Method is the same label attribution A39 used on the materializer: run the
EXISTING verified kernel unmodified, attribute every executed T to the nearest
preceding label, then aggregate labels into components.  The kernel is not
touched, so the number being split is the number A14 verified, and there is no
instrumentation overhead to subtract - the profiler counts the interpreter's
own T, not wall time.

Rows are weighted by their class's share of the real population, exactly as
the bench does, so a coverage-balanced sample does not skew the split.
"""
from __future__ import annotations
import bisect
import collections
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80                       # noqa: E402
import z80_column_solve_bench as cs                     # noqa: E402

SPANS_PER_UPDATE = 4.30          # span_decode_workload.py, per A14's note

# ---- label -> component.  Built by reading the kernel's own section comments.
# Order matters: the PRIMITIVES are matched first, because the first pass put
# `um_dok` (the multiply's own inner loop) and every shift helper into
# "other / control", which then read as 58% and hid what the stage is actually
# doing.  Helpers are attributed to the primitive that owns them.
COMPONENT = [
    ("multiply primitive", ("umul", "um_", "mul", "qsq", "sq_", "sm_", "smul")),
    ("shift primitives",   ("shr", "shl", "swu", "sw_", "srl")),
    ("wall_d_q4",          ("wd_", "wall_d")),
    ("inv_for_dq4",        ("ifd", "if_", "inv_for", "invz", "iv_")),
    ("inv_at_invd",        ("iai", "ia_", "inv_at")),
    ("angle_x / clip",     ("ax_", "angle_x", "clip", "cl_")),
    ("iq / step ramp",     ("cs_", "iq_", "recip", "ramp")),
    ("bearing / trig",     ("sin", "cos", "brg", "bear")),
]


def classify(name):
    for comp, pfx in COMPONENT:
        for p in pfx:
            if name.startswith(p):
                return comp
    return "other / control"


def main():
    budget = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
    T = cs.load_tables()
    code, labels = assemble(cs.SRC, cs.CODE)
    dump = ROOT / "build" / "column_solve_oracle.txt"
    if not dump.exists():
        raise SystemExit("missing build/column_solve_oracle.txt")
    allrows = [l.split() for l in dump.read_text().splitlines() if l.strip()]
    rows = cs.stratify(allrows, T, budget)

    marks = sorted((v, k) for k, v in labels.items())
    addrs = [a for a, _ in marks]
    names = [n for _, n in marks]
    base = cs.build_mem(T)

    cost = collections.Counter()
    calls = collections.Counter()
    tot_w = 0.0
    ts = []

    class P(Z80):
        def _step(self):
            pc, t0 = self.pc, self.t
            Z80._step(self)
            i = bisect.bisect_right(addrs, pc) - 1
            nm = names[i] if i >= 0 else "<top>"
            self.acc[nm] = self.acc.get(nm, 0) + (self.t - t0)
            if pc in self.entry:
                self.hit[nm] = self.hit.get(nm, 0) + 1

    entry = set(addrs)
    fails = 0
    for r, wgt in rows:
        (px, py, yaw, sid, lo, hi, x0, x1, invd, inv0, inv1,
         c0, c1, n, iq, step) = (int(v) for v in r)
        mem = bytearray(base)
        mem[cs.CODE:cs.CODE + len(code)] = code
        mem[cs.SID] = sid
        cs.w16(mem, cs.XQ4, px); cs.w16(mem, cs.YQ4, py)
        cs.w16(mem, cs.YAWQ, (yaw << 4) & 0xFFFF)
        cs.w16(mem, cs.LO, lo); cs.w16(mem, cs.HI, hi)
        cpu = P(mem); cpu.acc = {}; cpu.hit = {}; cpu.entry = entry
        cpu.run(cs.CODE)
        got = (cpu.m[cs.INVD], cpu.m[cs.INV0], cpu.m[cs.INV1])
        if got != (invd, inv0, inv1):
            fails += 1
        ts.append((cpu.t, wgt))
        tot_w += wgt
        for nm, t in cpu.acc.items():
            cost[nm] += t * wgt
        for nm, c in cpu.hit.items():
            calls[nm] += c * wgt

    if fails:
        raise SystemExit(f"kernel disagreed with the oracle on {fails} rows")

    mean_t = sum(t * w for t, w in ts) / tot_w
    line = mean_t * SPANS_PER_UPDATE
    print(f"=== COLSOLVE_CENSUS_A ===")
    print(f"{len(rows)} rows stratified from {len(allrows)} C-verified spans, "
          f"{len(code)} bytes, {fails} mismatches")
    print(f"population-weighted mean {mean_t:,.1f} T/span "
          f"-> {line:,.0f} T/update at {SPANS_PER_UPDATE} spans\n")

    comp_t = collections.Counter()
    comp_c = collections.Counter()
    for nm, t in cost.items():
        comp_t[classify(nm)] += t / tot_w
    for nm, c in calls.items():
        comp_c[classify(nm)] += c / tot_w

    WHOLE = 223266.0
    print(f"{'component':22} {'calls/span':>11} {'T/span':>10} {'T/update':>11}"
          f" {'% colsolve':>11} {'% update':>10}")
    for comp, t in comp_t.most_common():
        tu = t * SPANS_PER_UPDATE
        print(f"{comp:22} {comp_c[comp]:11.2f} {t:10,.1f} {tu:11,.0f}"
              f" {100*t/mean_t:10.1f}% {100*tu/WHOLE:9.2f}%")
    print(f"{'TOTAL':22} {'':>11} {mean_t:10,.1f} {line:11,.0f}"
          f" {100.0:10.1f}% {100*line/WHOLE:9.2f}%")

    print(f"\ntop labels")
    for nm, t in sorted(cost.items(), key=lambda kv: -kv[1])[:20]:
        print(f"  {nm:18s} {t/tot_w:9,.1f} T/span  {100*t/tot_w/mean_t:5.1f}%"
              f"   calls {calls.get(nm,0)/tot_w:6.2f}   [{classify(nm)}]")

    out = ROOT / "build" / "colsolve_census.txt"
    out.write_text(
        "\n".join(f"{c}\t{comp_c[c]:.4f}\t{t:.2f}\t{t*SPANS_PER_UPDATE:.1f}"
                  for c, t in comp_t.most_common()) + "\n")
    print(f"\nwritten to {out}")


if __name__ == "__main__":
    main()
