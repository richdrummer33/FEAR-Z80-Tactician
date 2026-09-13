# Renderer state — current-state dashboard

**Read this first, then `docs/PERFORMANCE_GROUND_TRUTH.md` for the full ledger
with provenance on every figure.**

Last updated: **2026-09-13**, after GUARDBAND — clipping solved at zero cycles,
compiled-edge-program coverage 80% -> 95.55%. Branch
`claude/renderer-forensic-reconstruction-azzocg`.

Provenance labels used throughout: **TARGET-MEASURED / Z80-SIM-MEASURED /
FUNCTIONALLY-VERIFIED / CORPUS-MEASURED / COMPOSED / HOST-MODELLED / ESTIMATED**.
Defined in `PERFORMANCE_GROUND_TRUTH.md` §2. Never collapse them into "modelled".

---

## Current architecture

A **span/sector renderer**, not a raycaster. One camera pose becomes a 20x18
name table of 8px tiles, which is then uploaded to VDP VRAM.

Six stages: **bearing lookup -> decode/clip -> GATE selector -> column-solve ->
depth sort -> column materializer**, then **VRAM upload**.

Each of the six exists as a cycle-exact Z80 kernel verified against its own
oracle. **They have never been run together.** There is no end-to-end Z80 update,
and the block walker that would drive them is not written in Z80 at all.

---

## Current measured/composed performance

| item | value | provenance |
| --- | ---: | --- |
| column materializer (BORDERHOIST) | **169,548 T/update** | Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED |
| the other five stages, summed | 41,760 T/update | COMPOSED from Z80-SIM-MEASURED kernels |
| **whole update, six stages** | **211,308 T** | **COMPOSED** |
| VRAM upload, representative | 6,000-8,000 T | HOST-MODELLED + CORPUS-MEASURED |
| **whole update incl. upload** | **~219,300 T** | **COMPOSED** |
| implied rate | **16.3 updates/s** | **IMPLIED FROM COMPOSED COST — NOT MEASURED FPS** |

**No renderer frame rate has ever been measured on target.** See "Invalidated".

---

## Experimental architecture: the compiled edge-program materializer (A46)

**The proposal.** Replace the materializer's per-column derivation of where wall
edges fall and which tile to draw with **precompiled ROM-resident programs** —
short sequences of (name-table word, destination delta) pairs — selected by a
lookup called the **rank dispatcher** from the wall's per-column step and
starting phase. The runtime would then only play programs back.

| | materializer | provenance |
| --- | ---: | --- |
| **guard band + fallback for the 4.45% it cannot bake** | **83,627 T** | **COMPOSED from Z80-SIM-MEASURED per-unit costs + CORPUS-MEASURED frequencies** |
| earlier, excluding off-screen edges | 96,848 T | superseded by the guard band |
| composed claim (SUPERSEDED) | ~~65,785 T~~ | see Invalidated |

Whole update, with BORDERHOIST and a representative upload: **129,673 T,
27.6 updates/s**.

> IMPLIED FROM COMPOSED COST — NOT MEASURED FPS.

**This CLEARS the 20 Hz solid target (178,977 T) with 28% margin**, while the
current implemented pipeline does not clear it at all. The 30 Hz aspiration is
now missed by only 8.7% — and dispatch, 59.9% of the compiled path, is still
untuned.

### How much of A46 is actually built — after PROGJOIN

| | status |
| --- | --- |
| endpoint-family/phase model vs the real renderer | **VERIFIED EXACT**, 1,742,796 real columns |
| program + dispatch tables emitted as real bytes | **YES** — `tools/edge_progjoin_bake.c` |
| dispatch selects the right program on corpus inputs | **YES** — 24,587 dispatches, output verified by the cells produced |
| dispatch joined to playback | **YES** — `ld sp,hl` implemented, tested, used |
| playback fed real generated programs | **YES** — no longer captured renderer output |
| edge cells vs the renderer's own `draw_edge` | **EXACT** — 0 wrong, 0 stray, over 19,912 run-edges |
| viewport clipping | **SOLVED at zero cycles** by a 7-row guard band; coverage 80.13% -> **95.55%** |
| edge-path cycle count | **Z80-SIM-MEASURED**, 22,240 T/pose |
| complete materializer (column walk, interior fill, borders, addressing) | **NOT re-integrated** |
| whole 20x18 name table vs oracle | **NOT done** — edge cells only |
| in a ROM, emulator, or hardware | **NO** |

**On the 95.55% of run-edges it covers the architecture is a real 5.1x win**:
the stages it replaces cost 114,440 T/pose for that subset, the compiled path
costs 22,240 T/pose. What is left is dispatch cost, which is untuned.

| measured per-unit cost | value | prior figure |
| --- | ---: | --- |
| rank dispatch | **1,041.7 T** | 381 T claimed — **2.73x**, and untuned |
| playback | **68.2 T/cell played** | 71.1 T composed — sound |
| chunk advance | 243.7 T/chunk | **absent from every composed figure** |
| per-run-edge setup | 167.0 T/run-edge | **absent from every composed figure** |

## Current bottlenecks, ranked by actual cycle mass

| rank | stage | T/update | share of 211,308 |
| ---: | --- | ---: | ---: |
| 1 | **column materializer** | 169,548 | 80.2% |
| 2 | column-solve (depth-plane) | 21,238 | 10.1% |
| 3 | decode-clip | 11,036 | 5.2% |
| 4 | VRAM upload | ~7,995 | (3.6% of 219k) |
| 5 | bearing lookup | 5,833 | 2.8% |
| 6 | GATE selector | 2,339 | 1.1% |
| 7 | depth sort | 1,314 | 0.6% |

Within the materializer, the five stages A46 would delete are **119,773 T, 68%
of it**: edge setup 41,424, endpoint geometry 28,163, row extents 25,357, edge
row walk 13,279, edge tile select 11,550.

---

## Open correctness gaps

- **C1 — the bearing cache is assumed, not built.** The 5,833 T line needs a
  de-duplicating cache that has never been written. Without it the line is
  ~12,888 T (+3.3% of the pipeline). Functional gap, not efficiency.
- **C4 — seven corners have no accurate baked leaf**; the `0xff` escape marker is
  unhandled at runtime.
- **C5 — Polar vs TileSector oracle divergence**, traced upstream.
- **Viewport clipping: SOLVED.** It was **near-wall frustum clipping, not
  occlusion** (occlusion is the depth sort plus near-to-far ownership): close
  walls whose top leaves the top of the screen or bottom the bottom, at
  half-heights 95-111 against a ~72 threshold. Since the clipped output is
  exactly the unclipped cells with out-of-range rows dropped (39,570/39,570
  columns) and rows span only [-7, 24], a **7-row guard band above and below
  the name table** absorbs them at **zero cycles**. Coverage 80.13% -> 95.55%.
- **The inverse-depth clamp (4.45%) is a real boundary.** Very close walls
  saturate `a>>6` at 255, which pins the heights and breaks the linear model the
  dispatch key assumes. Verified: including them produces a genuine dispatch
  conflict. They need the existing edge path as a fallback.
- **The published dispatch key was wrong twice**, both found only by executing
  it: it does not distinguish a full chunk from a short final one, and it needs
  C+1 thresholds rather than C, because the self-chaining destination delta
  depends on C+2 heights. Both fixed in `tools/edge_progjoin_bake.c`.

---

## Open integration gaps

- **No end-to-end Z80 update exists.** Six kernels, six memory maps, six
  harnesses, nothing that composes them.
- **The block walker is not written in Z80.**
- **BORDERHOIST and HOIST_A are bench variants**, exact and measured, but not
  shipped into `src/`.
- **A46's edge path is executed and exact, but not integrated**: the column
  walk, interior fill, border path and row addressing are untouched, and no
  whole 20x18 name table has been compared.

---

## ROM / WRAM status

| | value |
| --- | --- |
| cartridge configured | `-Wm-yo4` = **64 KiB** — an artefact of the 2D demo, NOT a constraint |
| platform ceiling | **4 MiB**; 512 KiB and 1 MiB are ordinary GG sizes |
| project position | up to 4 MiB available, **<= 1 MiB preferable**, but not at the cost of fidelity, scale or update rate |
| committed ROMs | 2 x 65,536 B — **GOAP tactical-AI demo only, not the renderer** |
| generated ROM tables in `src/generated` | 8,884 B across 24 arrays |
| Game Gear persistent WRAM | 381 B |
| guard-banded name table (A46) | 1,280 B, up from 720 B — **+560 B** |
| host-oracle-only state (not in cartridge) | 768 B |
| A46 ROM in `src/` | **0 bytes, 0 files** |
| A46 ROM if built, at C=6 | **~2.91 MB — affordable inside 4 MiB; above the 1 MiB comfort point, so worth shrinking** |
| A46 bank-switching cost | **UNMODELLED** |

The 64 KiB setting carries over from the 2D GOAP demo. Raising it is a build
change, not a platform problem.

---

## Current target

| | cycle gate | status |
| --- | ---: | --- |
| **60 Hz — lofty dream** | ~59.7k T | not a plan |
| **30 Hz — aspiration / fallback** | ~119.3k T | nice to reach, not required |
| **20 Hz — SOLID TARGET, clearing it is success** | ~179.0k T | — |
| current implemented pipeline | 219,300 T | **misses 20 Hz by 23%** |
| **A46, guard band + 4.45% fallback** | **129,673 T** | **CLEARS 20 Hz, 28% margin; misses 30 Hz by 8.7%** |

Neither gate includes game logic, input, audio, VBlank service, inter-stage
glue, VDP wait states, or banking.

---

## Last completed experiment

**GUARDBAND** — `make progjoin`. Closed the compiled-edge-program
architecture's clipping gap at zero cycles.

**What it does.** Compiled programs are position-independent, so they cannot
carry the row clamp `draw_edge` applies at the viewport edges. Rather than test
bounds per cell, the 20x18 name table now sits inside a **32-row buffer with 7
guard rows above and below**, and off-screen cells land in scratch rows nobody
reads. Three corpus measurements made this sound: the clipped output is exactly
the unclipped cells with out-of-range rows dropped (39,570/39,570 columns),
rows span exactly [-7, 24], and cell content is a pure function of the row.

**Result: EXACT**, and coverage rises **80.13% -> 95.55%** of run-edges.
19,912 run-edges, 31,806 dispatches, 160,717 cells played (20.1% absorbed
off-screen), **0 wrong cells, 0 stray writes** — including a check that nothing
escapes the buffer at all.

**Cost:** zero cycles, zero table growth, zero dispatch change. **+560 bytes of
WRAM** (1,280-byte buffer instead of 720).

**What we learned:**
1. The materializer drops **96,848 -> 83,627 T/pose**, and the whole update
   **142,894 -> 129,673 T (25.1 -> 27.6 updates/s)**.
2. The remaining **4.45% is a genuine boundary**, not laziness: including the
   inverse-depth-clamped run-edges produces a real dispatch conflict, because
   saturation pins the heights and breaks the linear model the key assumes.
   They need the existing edge path as a fallback.
3. A bug the checks caught: the baker used a negative sentinel for "first
   destination not yet set", which the guard band made a legitimate value.
   Every chunk then re-set the cursor. It showed up as a uniform +30-byte
   offset in the written cells.

## Next experiment — ONE rung only

**Tune the rank dispatcher.** It is now the dominant cost and the only large
lever left: **59.9% of the compiled path** at 1,041.7 T per dispatch, entirely
untuned. Two concrete wins are already visible in the kernel:

1. The descriptor lookup repeats per chunk although **family is constant per
   run-edge**, and the played-column count is C for every chunk but the last.
   Hoist it to per-run-edge setup.
2. The chunk advance (245.1 T) computes `iq += want*step` with a `djnz` add
   loop. For the common case `want == C` that is a constant multiple.

Halving dispatch would put the whole update near 123k T (~29 /s), within
touching distance of the 30 Hz aspiration. Nothing depends on it — the
architecture already clears the solid target.

Then, in order: price the 4.45% inverse-depth fallback, and integrate a
complete materializer (column walk, interior fill, borders) with a whole 20x18
name-table comparison, which PROGJOIN still has not done.

## Invalidated / superseded — must not return

| number | why void |
| ---: | --- |
| **48,600 T / 58,693 T** whole-update budgets | priced a 228,403 T stage at 21,756 T |
| **~61 Hz / ~73 updates/s / "fits in one NTSC frame"** | implied rates from those void budgets |
| **69,098 T / 0.86 updates/frame** | same `emit` placeholder defect |
| **26,820 / 26,891 T column-solve** | priced the fallback path the target takes 0.90% of the time |
| **~13,300 T column-solve** | A49 host model; the built kernel measured 21,238 T — **37% optimistic** |
| **223,266 T** current pipeline | wrong column-solve + pre-BORDERHOIST materializer |
| **113,127 T** A46 pipeline | wrong column-solve |
| **1.07 MB** A46 ROM | corrected to 3.00 MB at L=4 / 2.91 MB at C=6 |
| **65,785 T** A46 materializer | superseded by PROGJOIN: **96,848 T** measured+fallback, **77,269 T** clipping-free bound |
| **381 T** rank dispatch | measured at **1,038.8 T** for a dispatch that returns a playable program |
| **~32 updates/s** for A46 | superseded twice: 25.1 /s excluding off-screen edges, then **27.6 /s** with the guard band. Clears the 20 Hz solid target with 28% margin. |
| **"A46 covers only 80% of run-edges"** | superseded by the guard band: **95.55%** |
| **"37.9"** | video frames per logical **AI** tick, GOAP demo. Real rate 1.58 ticks/s |
| **"60 fps"** | ffmpeg capture rate / NTSC display cadence |

**There is no renderer FPS result of 20, 30, 37, or 40 in this project's
history.** Every renderer update rate ever quoted is arithmetic from a cycle
budget.

---

## Maintenance rule

Update this file whenever an experiment materially changes project state.
A new session should be able to open with:

> *Read `docs/RENDERER_STATE.md` and `docs/PERFORMANCE_GROUND_TRUTH.md`.*

and continue accurately.
