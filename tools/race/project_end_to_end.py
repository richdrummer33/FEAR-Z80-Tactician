#!/usr/bin/env python3
"""Weight the measured per-length kernel costs by the span lengths the renderer
actually produces.

The race times every span length from one to twenty. The census counts how often
each length occurs across a quarter of a million poses. Multiplying the two gives
a projection of the raster kernel's cost per update that rests on two
measurements and no cost model -- no interpolation between sampled lengths, no
assumed linearity in columns (the selector is a step function in chunks, not
linear), and no guess about how common short spans are.

It is still a PROJECTION, not an end-to-end measurement: it assumes every span
pays its full per-span setup and that nothing above the raster kernel changes.
Integration is what tests those assumptions.
"""
import csv, sys
from pathlib import Path

per = Path(sys.argv[1] if len(sys.argv) > 1 else "build/race/per_length.csv")
wts = Path(sys.argv[2] if len(sys.argv) > 2 else "build/race/span_weights.csv")

cost = {int(r["cols"]): {k: float(v) for k, v in r.items() if k != "cols"}
        for r in csv.DictReader(per.open())}
weight, colwork = {}, {}
for r in csv.DictReader(wts.open()):
    weight[int(r["cols"])] = int(r["spans"])
    colwork[int(r["cols"])] = int(r["columns"])

# Spans longer than the race measured are folded onto the longest measured
# length, which understates their cost for every kernel equally.
top = max(cost)
def c(n, k):
    return cost[min(n, top)][k]

KERNELS = [("dda_asm", "hand DDA"), ("bpl", "B packed, linear"),
           ("bfl", "B fixed, linear"), ("a1", "A1 honest oracle"),
           ("a0", "A0 ideal oracle"), ("pack_asm", "packed replay floor")]

# The per-length table shows the selector LOSING on one- and two-column spans:
# its cost is a step function in chunks, so a one-column span pays a whole
# chunk's setup while the DDA is linear in columns and barely pays anything.
# Even the ideal oracle loses there, so this is not selector inefficiency, it is
# the per-span setup having nothing to amortise against. A length test costs a
# compare and a branch; DISPATCH_T charges it generously.
DISPATCH_T = 25.0
def crossover(k):
    return min((n for n in sorted(cost) if cost[n][k] < cost[n]["dda_asm"]), default=None)

SPANS_PER_POSE = 4.31   # tools/race/span_census.c, 238,592 poses
nspan = sum(weight.values())
ncol = sum(colwork.values())
over = sum(v for n, v in weight.items() if n > top)

print(f"projected raster-kernel cost, weighted by {nspan:,} measured spans")
print(f"  {ncol:,} columns, mean {ncol/nspan:.2f} columns per span")
if over:
    print(f"  {over:,} spans ({100*over/nspan:.2f}%) exceed the longest timed length "
          f"({top}) and are charged as {top}")
print()
print(f"  {'kernel':<22} {'T-states/span':>14} {'per update':>14} {'vs DDA':>9}")
base = None
for key, name in KERNELS:
    tot = sum(weight[n] * c(n, key) for n in weight)
    per_span = tot / nspan
    if base is None:
        base = per_span
    # SPANS_PER_POSE is the census figure; one update draws one pose
    print(f"  {name:<22} {per_span:>14,.0f} {per_span*SPANS_PER_POSE:>14,.0f} "
          f"{base/per_span:>8.2f}x")
print()
print(f"  'per update' is the whole raster kernel for one frame at {SPANS_PER_POSE} spans a pose.")

for key, name in (("bpl", "B packed, linear"), ("a0", "A0 ideal oracle")):
    x = crossover(key)
    tot = sum(weight[n] * (min(c(n, key), c(n, "dda_asm")) + DISPATCH_T) for n in weight)
    ps = tot / nspan
    short = sum(weight[n] for n in weight if c(n, key) >= c(n, "dda_asm"))
    print(f"\n  {name} + DDA below {x} columns, dispatched on span length")
    print(f"    {ps:,.0f} T-states/span, {ps*SPANS_PER_POSE:,.0f} per update, "
          f"{base/ps:.2f}x  (DDA handles {100*short/nspan:.1f}% of spans)")

# where the remaining cost sits, for the packed selector
print("\n  where the packed selector's time goes, by span length")
print(f"  {'cols':>5} {'spans':>9} {'share':>7} {'T/span':>9} {'share of total':>15}")
tot = sum(weight[n] * c(n, "bpl") for n in weight)
for n in sorted(weight):
    t = weight[n] * c(n, "bpl")
    print(f"  {n:>5} {weight[n]:>9,} {100*weight[n]/nspan:>6.2f}% "
          f"{c(n,'bpl'):>9,.0f} {100*t/tot:>14.2f}%")
