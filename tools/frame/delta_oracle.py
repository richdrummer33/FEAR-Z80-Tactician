#!/usr/bin/env python3
"""Can a compact retained descriptor predict the true dirty set?

The name-table census proved the output changes sparsely and in one or two short
runs per column. It did not prove that a SMALL retained descriptor can say WHICH
cells changed without rediscovering them by scanning, and that is the load-bearing
claim under a delta materializer. If the descriptor has to be the eighteen words
themselves, comparing it is as expensive as the scan we are trying to avoid.

So this fits each column, each frame, to a seven-field descriptor:

    top edge     (row, word)          -- the cell(s) above the solid interior
    fill         (first, last, word)  -- the solid interior
    bottom edge  (row, word)          -- the cell(s) below it

then, from consecutive descriptors ALONE, predicts which rows need
reconsideration, and checks that prediction against the real
g_map(N-1) XOR g_map(N).

Two things can go wrong and both are measured rather than assumed:

  UNREPRESENTABLE  the column is not describable by the seven fields at all
                   (three edge cells, two separate fill runs, and so on).
  UNSOUND          the descriptors compare equal, or predict a smaller set,
                   than the cells that actually changed. A delta materializer
                   built on an unsound descriptor renders the wrong picture.

Over-prediction is fine and is reported as the efficiency cost.
"""
import sys
from pathlib import Path

ROWS, COLS, STRIDE = 18, 20, 40
MAX_EDGE = int(__import__('os').environ.get('MAX_EDGE', '1'))
EDGE_HIST = [0] * 10
RAGGED = [0]

def frames(path):
    b = Path(path).read_bytes()
    n = len(b) // (ROWS * STRIDE)
    for f in range(n):
        base = f * ROWS * STRIDE
        yield [[b[base + r * STRIDE + c * 2] | (b[base + r * STRIDE + c * 2 + 1] << 8)
                for c in range(COLS)] for r in range(ROWS)]

def column(fr, c):
    return [fr[r][c] for r in range(ROWS)]

def fit(col, base):
    """Fit the seven-field descriptor. Returns (desc, exact).

    The background is whatever the base word for that row is; everything else is
    surface. The fill is the longest run of one repeated surface word; anything
    surface above it is the top edge and below it the bottom edge. Exact means
    at most one cell of edge on each side, which is what the descriptor can hold.
    """
    surf = [r for r in range(ROWS) if col[r] != base[r]]
    if not surf:
        return (None, True)
    lo, hi = surf[0], surf[-1]
    if hi - lo + 1 != len(surf):
        return (("ragged",), False)          # holes: not a single occupied range
    best_s = best_e = lo; best_n = 1
    r = lo
    while r <= hi:
        s = r
        while r + 1 <= hi and col[r + 1] == col[s]:
            r += 1
        if r - s + 1 > best_n:
            best_n, best_s, best_e = r - s + 1, s, r
        r += 1
    top = [(r, col[r]) for r in range(lo, best_s)]
    bot = [(r, col[r]) for r in range(best_e + 1, hi + 1)]
    exact = len(top) <= MAX_EDGE and len(bot) <= MAX_EDGE
    EDGE_HIST[min(len(top), 9)] += 1
    EDGE_HIST[min(len(bot), 9)] += 1
    d = (tuple(top), (best_s, best_e, col[best_s]), tuple(bot))
    return (d, exact)

def predict(a, b):
    """Rows needing reconsideration, from the two descriptors alone."""
    dirty = set()
    def cells(d):
        if d is None:
            return {}
        out = {r: w for r, w in d[0]}
        f0, f1, fw = d[1]
        for r in range(f0, f1 + 1):
            out[r] = fw
        out.update({r: w for r, w in d[2]})
        return out
    ca, cb = cells(a), cells(b)
    for r in set(ca) | set(cb):
        if ca.get(r) != cb.get(r):
            dirty.add(r)
    return dirty

def run(path, label, base):
    fs = list(frames(path))
    if len(fs) < 2:
        print(f"[{label}] too few frames"); return
    n_cols = n_exact = 0
    n_actual = n_pred = 0
    unsound = unsound_cells = 0
    unrep = 0
    prev = fs[0]
    prev_desc = [fit(column(prev, c), base) for c in range(COLS)]
    for f in fs[1:]:
        desc = [fit(column(f, c), base) for c in range(COLS)]
        for c in range(COLS):
            n_cols += 1
            da, ea = prev_desc[c]
            db, eb = desc[c]
            if ea and eb:
                n_exact += 1
            else:
                unrep += 1
            actual = {r for r in range(ROWS) if prev[r][c] != f[r][c]}
            n_actual += len(actual)
            if not (ea and eb):
                # an unrepresentable column has to fall back to a full rewrite
                n_pred += ROWS if actual else 0
                continue
            pred = predict(da, db)
            n_pred += len(pred)
            missed = actual - pred
            if missed:
                unsound += 1
                unsound_cells += len(missed)
        prev, prev_desc = f, desc
    print(f"[{label}] {len(fs)} frames, {n_cols} column transitions")
    print(f"    representable by the seven-field descriptor: {100*n_exact/n_cols:.2f}%")
    print(f"    UNSOUND transitions (predicted set missed a real change): {unsound} "
          f"({100*unsound/n_cols:.3f}%), {unsound_cells} cells")
    print(f"    cells actually changed {n_actual}, predicted {n_pred} "
          f"(over-prediction {100*(n_pred-n_actual)/max(n_actual,1):+.1f}%)")
    tot = sum(EDGE_HIST) or 1
    print("    edge cells on one side of the fill:",
          " ".join(f"{i}:{100*EDGE_HIST[i]/tot:.1f}%" for i in range(10) if EDGE_HIST[i]),
          f"| ragged columns {RAGGED[0]}")
    EDGE_HIST[:] = [0] * 10
    RAGGED[0] = 0

# base_word(row), taken from the renderer rather than inferred. An earlier
# version took the most common word per row, which inverted the analysis on
# wall-heavy traces: 0x000F is the mid-shade full-wall tile and is the modal
# word in rows a wall usually covers, so the background and the wall swapped
# places and nothing was representable.
BASE = [0x0000] * 9 + [0x0002] + [0x0001] * 8

for t in sys.argv[1:]:
    run(f"build/frame/maps/{t}.bin", t, BASE)
