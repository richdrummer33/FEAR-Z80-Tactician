#!/usr/bin/env python3
"""The four-rung race, settled by arithmetic rather than by four Z80 kernels.

The Z80 has no cache, no pipeline and no speculative execution, so an
instruction sequence's cost is not an empirical quantity -- it is the sum of
published per-instruction timings. Where every other input is already measured,
building kernels to confirm a number that can be counted adds risk, not
confidence. What a race would genuinely settle is anything data-dependent, and
that is exactly what is measured here rather than assumed: hit rates, call
counts, per-call costs, changed-cell counts and representability all come from
the census and the repaired profile.

The one parameter that cannot be counted is the delta emitter's cost per changed
cell, because that code does not exist yet. It is swept.

Run: python3 tools/frame/race_arith.py
"""
# --- exact Z80 timings, cruise, after both mask-first conversions -------------
LD_A_NN, CP_HL, JR_NT, INC_HL, LD_HL_A = 13, 7, 7, 6, 7
CMP_BYTE = LD_A_NN + CP_HL + JR_NT + INC_HL     # 33
STO_BYTE = LD_A_NN + LD_HL_A + INC_HL           # 26
NBYTE = 16          # top_l/r, bot_l/r, profile, shade, border, 3 mask bytes, pad

# --- measured ----------------------------------------------------------------
RENDER   = 466_172      # T-states an update (frame timeline)
COLUMNS  = 44.52        # materializer invocations an update (profile)
EMISSION = 107_595      # T an update in emission (repaired profile, rung 18/19)
CHANGED  = 39.51        # name-table cells that change a frame (rung 15 ground truth)
CELL     = 142.6        # measured p_fill_open cost an interior row (rung 18)
HIT_IN   = 0.17         # descriptor identity on raw pixel inputs (rung 15)
HIT_Q    = 0.41         # descriptor identity on the quantised result (rung 15)
REPR     = (0.83, 0.90) # two-edge descriptor representability (rung 16)

per_col = EMISSION / COLUMNS
cmp_c, sto_c = NBYTE * CMP_BYTE, NBYTE * STO_BYTE

def skip(hit):
    """Rung 2: compare the whole descriptor, skip emission on a hit."""
    cost = hit * cmp_c + (1 - hit) * (cmp_c + sto_c)
    return (hit * per_col - cost) * COLUMNS

def delta(repr_, cell):
    """Rung 3/4: emit only the changed cells; unrepresentable columns fall back."""
    return EMISSION - (cmp_c * COLUMNS + sto_c * COLUMNS
                       + CHANGED * repr_ * cell
                       + (1 - repr_) * COLUMNS * per_col)

def skip_n(hit, n):
    """Rung 2 at a chosen descriptor width. ONE compare skips every emission
    component, so the cost is charged once a column, not once a component."""
    cost = n * CMP_BYTE + (1 - hit) * n * STO_BYTE
    return (hit * per_col - cost) * COLUMNS

print(f"emission {per_col:.0f} T a column; one compare skips all of it\n")
print("All figures are NET T-STATES SAVED an update. Positive is better.\n")
print(f"  {'rung':<44}{'net saved':>11}{'share':>9}")
print(f"  {'1  current materializer':<44}{'baseline':>11}{'':>9}")
for n, what in ((12, "quantised: rows, edge words, fill word, mask"),
                (18, "raw inputs: rows, pixel endpoints, mask")):
    hit = HIT_Q if n == 12 else HIT_IN
    v = skip_n(hit, n)
    print(f"  2  skip whole column, {n}B {what[:22]:<22}{v:>+11,.0f}{100*v/RENDER:>+8.2f}%")
print(f"  {'   break-even descriptor width':<44}{HIT_Q*per_col/(CMP_BYTE+(1-HIT_Q)*STO_BYTE):>10.1f}B")
for r in REPR:
    for c in (CELL, 300.0):
        v = delta(r, c)
        print(f"  3  delta emission, repr {r:.0%}, {c:3.0f} T a cell{'':<10}{v:>+11,.0f}{100*v/RENDER:>+8.2f}%")
