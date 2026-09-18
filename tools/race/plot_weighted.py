#!/usr/bin/env python3
"""Per-length kernel cost against the span lengths the renderer actually makes.

Two measurements on one pair of axes, because neither means much alone: the race
says what a span of each length costs, and the census says how often each length
happens. The shape that matters is the sawtooth -- the selector's cost is a step
function in six-column chunks while the DDA is linear in columns, so the selector
is at its best just before a chunk boundary and at its worst just after one.
"""
import csv, sys
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

per = Path(sys.argv[1] if len(sys.argv) > 1 else "build/race/per_length.csv")
wts = Path(sys.argv[2] if len(sys.argv) > 2 else "build/race/span_weights.csv")
out = Path(sys.argv[3] if len(sys.argv) > 3 else "build/selector-weighted.png")

cost = {int(r["cols"]): {k: float(v) for k, v in r.items() if k != "cols"}
        for r in csv.DictReader(per.open())}
weight = {int(r["cols"]): int(r["spans"]) for r in csv.DictReader(wts.open())}
N = sorted(cost)
tot = sum(weight.values())

fig, ax = plt.subplots(1, 2, figsize=(13.5, 5.4))

# left: cost per span, with the population underneath
bg = ax[0].twinx()
bg.bar(N, [100 * weight.get(n, 0) / tot for n in N], color="#dee2e6", width=.8, zorder=0)
bg.set_ylabel("share of spans (%)", color="#868e96")
bg.tick_params(axis="y", colors="#868e96")
bg.set_ylim(0, 40)
for key, name, col in (("dda_asm", "hand DDA", "#212529"),
                       ("bpl", "B packed, linear", "#2f9e44"),
                       ("a0", "A0 ideal oracle", "#3b5bdb"),
                       ("pack_asm", "packed replay floor", "#868e96")):
    ax[0].plot(N, [cost[n][key] for n in N], marker="o", ms=5, lw=2, color=col,
               ls="--" if key == "pack_asm" else "-", label=name, zorder=3)
ax[0].set_zorder(bg.get_zorder() + 1); ax[0].patch.set_visible(False)
ax[0].set_xlabel("columns in the span"); ax[0].set_ylabel("T-states per span")
ax[0].set_title("Cost per span, over the span lengths that actually occur\n"
                "(grey bars: how often each length happens)", fontsize=10)
ax[0].set_xticks(N[::2]); ax[0].grid(alpha=.25, lw=.7, zorder=1); ax[0].legend(fontsize=8.5, loc="upper left")

# right: the speedup sawtooth
sp = [cost[n]["dda_asm"] / cost[n]["bpl"] for n in N]
ax[1].plot(N, sp, marker="o", ms=6, lw=2.2, color="#2f9e44", label="B packed, linear")
ax[1].plot(N, [cost[n]["dda_asm"] / cost[n]["a0"] for n in N], marker="o", ms=5, lw=1.8,
           color="#3b5bdb", label="A0 ideal oracle")
ax[1].axhline(1.0, color="#212529", lw=2, label="hand DDA")
for b in (6.5, 12.5, 18.5):
    ax[1].axvline(b, color="#f76707", lw=1.2, ls=":", zorder=0)
ax[1].annotate("chunk boundaries", (6.6, 0.55), color="#f76707", fontsize=8.5)
wavg = sum(weight[n] * cost[n]["dda_asm"] for n in N) / sum(weight[n] * cost[n]["bpl"] for n in N)
ax[1].axhline(wavg, color="#2f9e44", lw=1.2, ls="--", alpha=.55)
ax[1].annotate(f"length-weighted projection: {wavg:.2f}x", (1.2, wavg + .07),
               color="#2f9e44", fontsize=8.5, alpha=.8)
# The projection above still samples (step, phase) from the race's cases. MEASURED
# is the same two kernels replaying the renderer's own tuples, which is the number
# to quote: at matched lengths the race's cases had steeper geometry, so they
# inflated the DDA's per-move loop more than the selector's LDIR.
MEASURED = 1.445
ax[1].axhline(MEASURED, color="#c2255c", lw=2.0, ls="-")
ax[1].annotate(f"measured on the renderer's real tuples: {MEASURED:.3f}x",
               (1.2, MEASURED - .18), color="#c2255c", fontsize=9, fontweight="bold")
ax[1].set_xlabel("columns in the span"); ax[1].set_ylabel("speedup vs hand DDA")
ax[1].set_title("The selector pays per chunk and the DDA pays per column,\n"
                "so the win sawtooths and is negative below three columns", fontsize=10)
ax[1].set_xticks(N[::2]); ax[1].grid(alpha=.25, lw=.7); ax[1].legend(fontsize=8.5, loc="lower right")

fig.suptitle("What the selector is worth once the renderer's real workload is accounted for", fontsize=12)
fig.tight_layout()
out.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(out, dpi=140)
print(f"wrote {out}")
