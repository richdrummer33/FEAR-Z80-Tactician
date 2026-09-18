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
# The rung 15 "quantised" rate of 0.41 was measured on a descriptor that did NOT
# carry the edge tile word, so it counted columns as identical whose edges had
# changed. It is not a sound skip rate and is not used here. The sound rates are
# the raw-input one above (per invocation) and the g_map column identity below
# (per final screen column), the latter measured from the output itself.
HIT_COL  = {"spin": 0.3924, "cruise": 0.5662, "corners": 0.5874, "stress": 0.6525}
INVOCATIONS, SCREEN_COLS = 44.52, 20
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

def skip_at(hit, n, units):
    """Whole-unit skip. ONE compare skips every emission component, so the cost
    is charged once a unit, not once a component. `units` is what decides the
    answer: a materializer invocation, or a final screen column after ownership
    has been resolved."""
    per_unit = EMISSION / units
    cost = n * CMP_BYTE + (1 - hit) * n * STO_BYTE
    return (hit * per_unit - cost) * units

print("All figures are NET T-STATES SAVED an update. Positive is better.")
print("A 12-byte descriptor throughout; granularity, not width, decides this.\n")
print(f"  {'skip granularity':<46}{'net saved':>11}{'share':>9}")
print(f"  per materializer invocation ({INVOCATIONS} a frame, "
      f"{EMISSION/INVOCATIONS:,.0f} T each)")
for hit in (0.108, 0.244):
    v = skip_at(hit, 12, INVOCATIONS)
    print(f"  {'    sound identity ' + f'{hit:.1%}':<46}{v:>+11,.0f}{100*v/RENDER:>+8.2f}%")
print(f"  per final screen column ({SCREEN_COLS} a frame, "
      f"{EMISSION/SCREEN_COLS:,.0f} T each)")
for t, hit in sorted(HIT_COL.items(), key=lambda x: x[1]):
    v = skip_at(hit, 12, SCREEN_COLS)
    print(f"  {'    ' + t + f', measured identity {hit:.1%}':<46}{v:>+11,.0f}{100*v/RENDER:>+8.2f}%")
print()
for r in REPR:
    for c in (CELL, 300.0):
        v = delta(r, c)
        print(f"  delta emission, repr {r:.0%}, {c:3.0f} T a cell{'':<12}{v:>+11,.0f}{100*v/RENDER:>+8.2f}%")
