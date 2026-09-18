#!/usr/bin/env python3
"""Plot the selector ladder against the hand-written DDA.

Reads the profiler's own output rather than re-deriving anything, so the plot
cannot disagree with the numbers the correctness gate released.
"""
import re, sys
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SRC = Path(sys.argv[1] if len(sys.argv) > 1 else "build/race/ladder.txt")
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else "build/selector-ladder.png")

txt = SRC.read_text()
if "RACE_EXACT" not in txt:
    sys.exit("no RACE_EXACT line: the correctness gate withheld these timings")

def rows_after(marker, occurrence=1):
    """The numeric rows of the table that follows the nth occurrence of marker."""
    block = txt.split(marker)[occurrence]
    out = {}
    for line in block.splitlines():
        m = re.match(r"\s+(\d+)\s+((?:[-\d.]+x?\s*){5})$", line)
        if m:
            out[int(m.group(1))] = [float(v.rstrip("x")) for v in m.group(2).split()]
        elif out:
            break
    return out

refrows = rows_after("asm gap")      # DDA C, BAND C, DDA asm, PACK asm, gap
absol   = rows_after("B pak lin", 1) # absolute T-states per span
speed   = rows_after("B pak lin", 2) # speedup against the hand DDA

LEN = sorted(absol)
NAMES = ["A0 oracle", "A1 honest", "B fixed, linear", "B fixed, balanced", "B packed, linear"]
# one hue per series, assigned in fixed order and never cycled
COL = ["#3b5bdb", "#1098ad", "#f76707", "#ae3ec9", "#2f9e44"]

fig, ax = plt.subplots(1, 2, figsize=(13, 5.2))

for i, nm in enumerate(NAMES):
    ax[0].plot(LEN, [absol[n][i] for n in LEN], marker="o", ms=7, lw=2, color=COL[i], label=nm)
ax[0].plot(LEN, [refrows[n][2] for n in LEN], marker="s", ms=8, lw=2.5, color="#212529",
           label="hand DDA (the thing to beat)")
ax[0].plot(LEN, [refrows[n][3] for n in LEN], marker="^", ms=7, lw=2, ls="--", color="#868e96",
           label="packed replay floor (identification free)")
ax[0].set_xlabel("columns in the span"); ax[0].set_ylabel("T-states per span")
ax[0].set_title("Whole path charged: bank select, address formation,\nsearch, body fetch, replay, chunk chaining", fontsize=10)
ax[0].set_xticks(LEN); ax[0].grid(alpha=.25, lw=.7); ax[0].legend(fontsize=8.5)

for i, nm in enumerate(NAMES):
    ax[1].plot(LEN, [speed[n][i] for n in LEN], marker="o", ms=7, lw=2, color=COL[i], label=nm)
    ax[1].annotate(f"{speed[LEN[-1]][i]:.2f}x", (LEN[-1], speed[LEN[-1]][i]),
                   textcoords="offset points", xytext=(6, -3), fontsize=9, color=COL[i])
ax[1].axhline(1.0, color="#212529", lw=2, label="hand DDA")
ax[1].set_xlabel("columns in the span"); ax[1].set_ylabel("speedup vs hand DDA")
ax[1].set_title("Every selector beats deriving it, and the exact-state\noracle bounds what any naming scheme can win", fontsize=10)
ax[1].set_xticks(LEN); ax[1].grid(alpha=.25, lw=.7); ax[1].legend(fontsize=8.5)

fig.suptitle("Selector ladder on real Z80 timing, all eleven kernels byte-identical", fontsize=12)
fig.tight_layout()
OUT.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(OUT, dpi=140)
print(f"wrote {OUT}")
