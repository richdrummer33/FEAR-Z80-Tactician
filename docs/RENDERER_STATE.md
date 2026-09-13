# Renderer state — current-state dashboard

**Read this first, then `docs/PERFORMANCE_GROUND_TRUTH.md` for the full ledger
with provenance on every figure.**

Last updated: **2026-09-13**, ground-truth commit `8991f14`
(forensic reconstruction on branch `claude/renderer-forensic-reconstruction-azzocg`).

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

| | value | provenance |
| --- | ---: | --- |
| A46 materializer at C=6 (6 columns/program) | 65,785 T | **COMPOSED** |
| A46 + BORDERHOIST | 62,076 T | **COMPOSED** |
| A46 whole pipeline, corrected column-solve | 103,836 T | COMPOSED |
| A46 whole pipeline + representative upload | **~110-115k T** | **COMPOSED** |
| implied rate | **~32 updates/s** | **IMPLIED FROM COMPOSED COST — NOT MEASURED FPS** |

### How much of A46 is actually built

| | status |
| --- | --- |
| endpoint-family/phase model vs the real renderer | **VERIFIED EXACT**, 1,742,796 real columns |
| dispatch key decomposition is conflict-free | **VERIFIED**, 0 conflicts, exhaustive **synthetic** sweep |
| 20-byte Z80 playback loop | **Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED**, 71.7 T/edge row |
| ...but fed **captured old-renderer output**, not generated programs | **NOT VERIFIED** |
| program/dispatch tables ever emitted as data | **NO** — generated transiently, hashed, discarded |
| rank dispatcher as Z80 code | **DOES NOT EXIST** in any commit; its 381 T is unreproducible |
| dispatch joined to playback | **NO** — blocked on `ld sp,hl`, absent from `z80core.py` |
| complete materializer executed / oracle-checked / cycle-counted | **NO / NO / NO** |
| in a ROM, emulator, or hardware | **NO** |

**65,785 T is COMPOSED**: a Z80-SIM-MEASURED baseline (175,827) minus
Z80-SIM-MEASURED removed stages (119,773) plus COMPOSED playback (4,609) plus an
ESTIMATED dispatch (5,121). Never call it measured; never call it merely
modelled.

---

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
- **A46 clamped path** — 1,686 inverse-depth-clamped columns are routed to a
  "separate path" the program model does not cover, and the dispatch verifier
  skips clamped samples entirely.

---

## Open integration gaps

- **No end-to-end Z80 update exists.** Six kernels, six memory maps, six
  harnesses, nothing that composes them.
- **The block walker is not written in Z80.**
- **BORDERHOIST and HOIST_A are bench variants**, exact and measured, but not
  shipped into `src/`.
- **All of A46.**

---

## ROM / WRAM status

| | value |
| --- | --- |
| cartridge configured | `-Wm-yo4` = **64 KiB** |
| committed ROMs | 2 x 65,536 B — **GOAP tactical-AI demo only, not the renderer** |
| generated ROM tables in `src/generated` | 8,884 B across 24 arrays |
| Game Gear persistent WRAM | 381 B |
| host-oracle-only state (not in cartridge) | 768 B |
| A46 ROM in `src/` | **0 bytes, 0 files** |
| A46 ROM if built, at C=6 | **~2.91 MB — 45x the configured cartridge** |
| A46 bank-switching cost | **UNMODELLED** |

A 4 MB cartridge is an assumption this repository **does not currently make**.
Taking it is an explicit decision.

---

## Current target

| | cycle gate | status |
| --- | ---: | --- |
| **20 Hz — solid target** | ~179k T/update | **missed: 219k, 23% over** |
| **30 Hz — aspiration** | ~119k T/update | missed: 84% over |
| A46 future pipeline vs 30 Hz | ~119k T | would clear at ~110-115k **if it worked and integrated free** |

Neither gate includes game logic, input, audio, VBlank service, inter-stage
glue, VDP wait states, or banking.

---

## Last completed experiment

**Forensic reconstruction (this commit).** Re-ran every headline benchmark from
source rather than trusting recorded prose.

**What reproduced exactly:** the three materializer variants (175,924 / 173,257 /
169,548, all oracle-EXACT); the 175,827 T baseline with 125/125 pose
verification; the five-stage attribution summing to 119,773; decode-clip 11,036;
GATE 2,339; depth sort 1,314; DPSOLVE 4,939.0 T/span at 6,039/6,039 exact; the
99.10% depth-plane success rate; the VRAM upload census; the A46 dispatch
decomposition at 0 conflicts and 2.91 MB.

**What we learned that was not previously recorded:**
1. The **381 T rank-dispatch figure is unreproducible** — no Z80 dispatch kernel
   exists in any commit. It was previously described as merely unverified.
2. The **bearing cache is not built**, and the 5,833 T ledger line silently
   depends on it.
3. The historical "renderer near frame rate" impression traces to the
   **48,600 T / 58,693 T budgets** that priced a 228,403 T stage at 21,756 T.
4. `TODO_DEFERRED.md`'s "34,538,688 observations" should be **34,738,688**.
5. `tools/target_solve_census.c` could not build at all — the Makefile creates
   `build/gbdk/gbdk` but never populates the GBDK shim header it needs. Fixed.

---

## Next experiment — ONE rung only

**PROGJOIN: execute a real compiled edge program end to end.**

Close the single largest A46 gap: no generated program has ever been consumed by
any kernel. In order:

1. Implement **`ld sp,hl` (0xF9, 6 T)** in `tools/z80core.py` with a unit test.
   This is a genuine interpreter gap, not something to design around.
2. **Emit a real, bounded program + dispatch table** — only the keys reachable
   from a pose subset. `target_solve_census` already reports **13,530 distinct
   (step, phase) keys** across the whole corpus, so a representative subset is
   tens of KB, not 2.91 MB.
3. Assemble a Z80 kernel that **joins dispatch to playback**: rank compare ->
   program address -> `ld sp,hl` -> playback.
4. Feed it **corpus-derived run records** from the pose oracle.
5. **Verify dispatch returns the correct program** by reading its output back —
   the defect that made 381 T meaningless.
6. Compare the resulting **20x18 name table against the pose oracle**, the same
   oracle EDGELUT3 passes 125/125.
7. **Cycle-count the complete path** and report the delta against the 65,785 T
   composition.

Success criterion: a **Z80-SIM-MEASURED, FUNCTIONALLY-VERIFIED** complete-stage
cost to set beside the composed one. Explicitly out of scope: banking cost, the
full 2.91 MB corpus, and any ROM build.

---

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
