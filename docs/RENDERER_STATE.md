# Renderer state — current-state dashboard

**Read this first, then `docs/PERFORMANCE_GROUND_TRUTH.md` for the full ledger
with provenance on every figure.**

Last updated: **2026-09-13**, after PROGJOIN — the compiled edge-program chain
executed end to end for the first time. Branch
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
| **measured path + fallback for the 19.87% it cannot bake** | **96,848 T** | **COMPOSED from Z80-SIM-MEASURED per-unit costs + CORPUS-MEASURED frequencies** |
| bound if clipping were free and every edge baked | 77,269 T | COMPOSED, optimistic |
| composed claim (SUPERSEDED) | ~~65,785 T~~ | see Invalidated |

Whole update, with BORDERHOIST and a representative upload: **142,894 T
(25.1 updates/s)** measured+fallback, **123,315 T (29.0 /s)** clipping-free bound.

> IMPLIED FROM COMPOSED COST — NOT MEASURED FPS.

**This CLEARS the 20 Hz solid target (178,977 T) with 25% margin**, in the
pessimistic case, while the current implemented pipeline does not clear it at
all. The 30 Hz aspiration is missed — that is a stretch goal, not the spec.

### How much of A46 is actually built — after PROGJOIN

| | status |
| --- | --- |
| endpoint-family/phase model vs the real renderer | **VERIFIED EXACT**, 1,742,796 real columns |
| program + dispatch tables emitted as real bytes | **YES** — `tools/edge_progjoin_bake.c` |
| dispatch selects the right program on corpus inputs | **YES** — 24,587 dispatches, output verified by the cells produced |
| dispatch joined to playback | **YES** — `ld sp,hl` implemented, tested, used |
| playback fed real generated programs | **YES** — no longer captured renderer output |
| edge cells vs the renderer's own `draw_edge` | **EXACT** — 0 wrong, 0 stray, over 16,700 run-edges |
| edge-path cycle count | **Z80-SIM-MEASURED**, 17,001 T/pose |
| complete materializer (column walk, interior fill, borders, addressing) | **NOT re-integrated** |
| whole 20x18 name table vs oracle | **NOT done** — edge cells only |
| in a ROM, emulator, or hardware | **NO** |

**On the 80.13% of run-edges it can handle the architecture is a real 5.6x
win**: the stages it replaces cost 95,979 T/pose for that subset, the compiled
path costs 17,001 T/pose. The shortfall is in dispatch cost and the un-bakeable
remainder, not in the idea.

| measured per-unit cost | value | prior figure |
| --- | ---: | --- |
| rank dispatch | **1,038.8 T** | 381 T claimed — **2.73x** |
| playback | **68.4 T/cell** | 71.1 T composed — sound |
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
- **A46 has no clipping story yet — but it is cheap.** Compiled programs are
  position-independent and carry no screen clipping, while `draw_edge` clamps
  the drawn row range to the 18-row viewport. **19.21% of run-edges need it**;
  a further 0.65% hit the inverse-depth clamp. This is **near-wall frustum
  clipping, not occlusion** (occlusion is the depth sort plus near-to-far
  ownership): close walls whose top leaves the top of the screen or bottom the
  bottom, at half-heights 95-111 against a ~72 threshold.
  **Measured: the clipped output is exactly the unclipped program's cells with
  out-of-range rows dropped — 39,570 of 39,570 columns.** So the fallback is a
  skip count plus a shortened play count over the SAME program, not a second
  renderer.
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
| **A46, measured + clipping fallback** | **142,894 T** | **CLEARS 20 Hz, 25% margin** |
| A46, clipping-free bound | 123,315 T | clears 20 Hz; just misses 30 Hz |

Neither gate includes game logic, input, audio, VBlank service, inter-stage
glue, VDP wait states, or banking.

---

## Last completed experiment

**PROGJOIN** — `make progjoin`. The compiled edge-program chain, executed end to
end on real corpus inputs for the first time:

    corpus run-edge -> rank dispatch -> real generated program
                    -> `ld sp,hl` -> playback -> name-table cells
                    -> compared against the renderer's own draw_edge

**Result: EXACT.** 2,486 poses, 16,700 run-edges, 24,587 dispatches, 115,089
cells, **0 wrong cells, 0 stray writes**.

**What we learned:**
1. **The mechanism works.** Dispatch retrieves the right program and playback
   reproduces the renderer's edge cells exactly.
2. **Dispatch costs 1,038.8 T, not 381 T** — 2.73x. The 381 T priced a fragment
   that did not include the family/length dimension, the body-pointer
   indirection, the cell-count lookup, or forming the program address. The
   kernel measured is also untuned.
3. **The playback composition was sound**: 68.4 T/cell measured vs 71.1 T.
4. **Two cost terms were missing from every A46 budget**: chunk advance
   (243.7 T/chunk) and per-run-edge setup (167.0 T/run-edge).
5. **Two real defects in the published dispatch key**, neither catchable by the
   published verifier — it hashed only tile sequences, with no successor
   column, no partial chunks, and multi-row columns truncated at four rows.
6. **19.87% of run-edges cannot be baked at all** — programs carry no clipping.
7. `ld sp,hl` is implemented and tested. Also recorded: the assembler accepts
   `exx`, `ex af,af'` and `daa`, which the interpreter cannot execute; they
   raise rather than mis-execute, but `z80core.py`'s docstring claims an
   instruction cannot be assembled into a form the interpreter will not run.

## Next experiment — ONE rung only

**Implement clipping in the compiled-program path, and recover the 19.87%.**

This is now the highest-value rung and it is well understood. Clipping is a pure
row-range filter over an unchanged program body (39,570/39,570 columns verified),
so the work is: compute a leading skip count and a shortened play count at
dispatch — from the absolute row the destination cursor already holds — then play
the same baked program. Verify against the renderer's `draw_edge` on the
currently-excluded run-edges, and cycle-count the addition.

Success would take the architecture from 80.13% coverage to full coverage, and
from ~25 updates/s toward the ~29 /s clipping-free bound.

Two follow-ons, in order, neither a gate on the above:

1. **Tune the rank dispatcher.** It measured 1,038.8 T untuned: the descriptor
   lookup repeats per chunk although family is constant per run-edge, and the
   chunk advance uses a `djnz` add loop. This is upside on an already-successful
   result, not a condition for continuing.
2. **Integrate a complete materializer** — column walk, interior fill, borders —
   and compare a whole 20x18 name table, which PROGJOIN did not do.

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
| **~32 updates/s** for A46 | superseded: **25.1 /s** measured+fallback, **29.0 /s** bound. Clears the 20 Hz solid target; misses the 30 Hz aspiration. |
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
