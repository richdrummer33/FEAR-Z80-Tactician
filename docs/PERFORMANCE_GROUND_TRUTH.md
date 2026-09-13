# Performance ground truth

**Purpose.** The authoritative performance ledger for the 3D span renderer.
Every figure carries a noun (what physical work it represents) and one or more
provenance labels. **If a number is not in this file with a label, do not quote
it.**

Rebuilt from evidence on **2026-09-13** at commit `8991f14`, branch
`claude/renderer-forensic-reconstruction-azzocg`. Every figure marked RE-RUN
below was re-executed during that rebuild, not copied from prior prose.

**Updated 2026-09-13 with the PROGJOIN result (§8a):** the compiled
edge-program chain has now been executed end to end on real corpus inputs and
is oracle-EXACT, and its measured cost is materially higher than the composed
65,785 T figure. See §8a before quoting any compiled-edge-program number.

---

## 1. Clock and budget reference

Sega Game Gear Z80 at **3,579,545 Hz**. The NTSC display runs at 59.9 Hz; one
display frame is **59,736 T**, of which VBlank alone is 15,960 T.

| target update rate | cycles available per update |
| ---: | ---: |
| **60 Hz** | **~59.7k T** (59,659) |
| **40 Hz** | **~89.5k T** (89,489) |
| **30 Hz** | **~119.3k T** (119,318) |
| **20 Hz** | **~179k T** (178,977) |

Project gates (`docs/PROJECT_MEMORY.md`), and how to read them:

| rate | cycles/update | standing |
| --- | ---: | --- |
| 60 Hz | ~59.7k | **lofty dream goal.** Not a plan. |
| 30 Hz | ~119.3k | **aspiration / fallback target.** Nice to reach, not required. |
| **20 Hz** | **~179.0k** | **the solid target. Clearing this is SUCCESS.** |

**Anything at or above ~20 updates/s is a success.** Do not describe missing
30 Hz as a failure: 30 Hz was never the spec.

---

## 2. Provenance vocabulary — these are never interchangeable

| label | meaning |
| --- | --- |
| **TARGET-MEASURED** | Executed as integrated Game Gear code in a ROM, in an emulator, or on hardware. |
| **Z80-SIM-MEASURED** | Real Z80 machine code executed and cycle-counted by `tools/z80core.py`. Real cycles for the code that ran — but one stage in isolation, no VBlank, no VDP, no glue. |
| **FUNCTIONALLY-VERIFIED** | The implementation consumed representative or real inputs and its output was compared against an independent oracle. |
| **CORPUS-MEASURED** | A statistic obtained by running the real pose/span corpus (frequencies, counts, success rates). |
| **COMPOSED** | Arithmetic over separately obtained quantities. No integrated execution. |
| **HOST-MODELLED** | A host-side model or census standing in for a proposed target architecture. Carries no target cycle measurement. |
| **ESTIMATED** | Not implemented or measured well enough to earn any label above. |

A result usually needs several labels. Two rules that this project has broken
before:

- **Never collapse all non-ROM work into "modelled."** A cycle-exact Z80 kernel
  verified against an oracle and a spreadsheet projection are not the same
  thing.
- **Never call a composed number "measured" because its ingredients were
  measured.** Composing measured component costs across a path change
  mispredicted the depth-plane column-solve kernel by **37%** (§7).

---

## 3. What has actually run on a Game Gear: nothing from the renderer

**The 3D span renderer has never been assembled into a Game Gear ROM, never run
in an emulator, and never run on hardware.** Verified three ways at `8991f14`:

- `make state-census` reports: *A46 tables in `src/`: 0 files, 0 bytes; A46
  kernel composed into `src/`: NO; A46 ROM in the cartridge: 0 bytes.*
- `grep -rli` for any compiled-edge-program artifact in `src/` returns nothing.
- The GBDK/SDCC toolchain is not even installed in the working environment, so
  no `.gg` can currently be produced from this tree at all.

The only ROMs that exist are the **GOAP tactical-AI demo** — a 2D top-down
squad simulation, a completely separate program from the span renderer:

| artifact | what it is | provenance |
| --- | --- | --- |
| `roms/FEAR-Z80-Tactician-v0.5.0-seed2.gg` (65,536 B) | GOAP AI demo, 11 agents | TARGET-MEASURED |
| `roms/FEAR-Z80-Tactician-v0.5.0-seed42.gg` (65,536 B) | same, seed 42, 10 agents | TARGET-MEASURED |
| `docs/releases/v0.5.0-runtime-verify.txt` | Gearsystem 3.9.16 run of the above | TARGET-MEASURED |
| `docs/history/STAGE4_OFFICELOOP_RUNTIME_VERIFY.txt` | earlier GOAP office-loop run | TARGET-MEASURED |

---

## 4. The FPS record, settled

Searched `docs/`, `README.md`, `CHANGELOG.md` and the complete git log.

### Has the 3D renderer ever...

| question | answer |
| --- | --- |
| run as a Game Gear ROM? | **NO** |
| run in a Game Gear emulator? | **NO** |
| run on hardware? | **NO** |
| produced a measured renderer update counter? | **NO** — the harness is specified in `docs/TODO_DEFERRED.md` §B and deliberately not started |
| demonstrated 20+ renderer updates/s? | **NO** |
| demonstrated 30+? | **NO** |
| demonstrated 40+? | **NO** |

**There is no target-measured renderer frame rate of any value.** Every
update-rate figure this project has ever produced for the renderer is
arithmetic: cycles-per-update divided into the clock.

### What the historical numbers actually were

| figure | what it really measures | where |
| --- | --- | --- |
| **60 fps** | the ffmpeg capture rate of the emulator's display output, i.e. the Game Gear's own NTSC cadence | `v0.5.0-runtime-verify.txt` line 130 |
| **37.9** | `2237 / 59` = **video frames per logical AI tick**, for the GOAP demo. The reciprocal of a rate, and for the AI, not the renderer. The same file states the real rate: **about 1.58 eleven-agent world ticks per second.** | ibid. lines 116-117 |
| **3.2 / 2.4 ticks/s** | GOAP office-loop world-tick rates | `STAGE4_OFFICELOOP_RUNTIME_VERIFY.txt` |

### Correction: where a "renderer runs near frame rate" impression came from

It was never stated as "40 FPS" or "30 FPS". It entered as **implied update
rate from a budget that was missing the renderer's most expensive stage**:

> *"The span interpreter fits inside one NTSC frame for the first time — 58,693 T
> against 59,736 T"* — `TODO_DEFERRED.md` A12
>
> *"TOTAL 48,600 T, 1.23 updates/frame ... about 61.0 Hz of update rate"* — A13

Both budgets carried a line called **"emit," 21,756 T**, a kernel that rebuilt a
name table *from finished words*. The work of **producing** those words did not
exist yet. When the column materializer was actually built it measured
**228,403 T** and the record states plainly: *"This REPLACES the emit line, it
does not add to it."* The whole update went from 48,600 T to **275,745 T** in one
step.

**Those budgets understated the renderer's dominant stage by a factor of ten.
Every update-rate figure above ~17/s in this project's history traces to them.
They are VOID.** See §9.

`TODO_DEFERRED.md` §B carries the standing rule: *"Gearsystem's ~59.9 Hz is the
Game Gear display/VBlank cadence, not the renderer's FPS."*

---

## 5. CURRENT IMPLEMENTED PIPELINE

The six stages that turn one camera pose into a finished 20x18 name table in
RAM. Every line is a real Z80 kernel executed on `tools/z80core.py` and checked
against an independent oracle. **They have never been executed together.**

| # | stage | what it physically does | T/update | provenance | oracle status |
| --- | --- | --- | ---: | --- | --- |
| 1 | bearing lookup | turns a wall-corner coordinate into a bearing, via the A12 quarter-square byte-plane multiply | **5,833** | COMPOSED: Z80-SIM-MEASURED 639.6 T/lookup x CORPUS-MEASURED 9.12 distinct corners | 53,112/53,112 exact; **but see caveat C1** |
| 2 | decode-clip | decodes a span record and clips it to the view frustum | **11,036** | COMPOSED: Z80-SIM-MEASURED 919.8 T/span x CORPUS-MEASURED 12.00 spans/update | 6,000/6,000 exact |
| 3 | GATE selector | evaluates a doorway/aperture visibility gate | **2,339** | COMPOSED: Z80-SIM-MEASURED 863.1 T/test x CORPUS-MEASURED 2.71 tests/update | 6,000/6,000 exact |
| 4 | column-solve (depth-plane, normal path) | turns a wall segment + pose into the starting inverse-depth and per-column step the materializer walks | **21,238** | COMPOSED: Z80-SIM-MEASURED 4,939.0 T/span x CORPUS-MEASURED 4.3 spans/update | 6,039/6,039 exact |
| 5 | depth sort | orders runs back-to-front | **1,314** | Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED | verified |
| 6 | column materializer (BORDERHOIST) | walks the columns and writes every name-table word: edges, interior fill, borders | **169,548** | Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED, 1,216 bytes | EXACT vs pose oracle |
| | **SUM** | | **211,308** | **COMPOSED** | — |

*(RE-RUN 2026-09-13: materializer 169,548.4 EXACT; DPSOLVE 4,939.0 T/span,
6,039/6,039 exact; decode-clip 11,036; GATE 2,339; depth sort 1,314. All
reproduced.)*

### VRAM / name-table upload

The shipped uploader `src/tilesector_polar_ntupload_raw_gg.s` walks 18 rows and,
for each row whose cells changed since the previous update, sets a VDP address
and streams the changed interval with `otir`. Cost depends on camera motion, not
on a single pose.

| camera motion | changed cells | dirty rows | bytes | **upload cost** |
| --- | ---: | ---: | ---: | ---: |
| pure yaw, 1 unit/update | 29.4 of 360 | 9.3 of 18 | 85.5 | **7,995 T** |
| forward walk, 4 q4/update | 19.2 of 360 | 6.2 of 18 | 52.9 | **5,892 T** |
| worst observed | — | 18 | — | 23,587 T |
| full 720-byte table | — | 18 | 720 | 25,183 T |
| floor, nothing changed | — | 0 | 0 | 2,035 T |

**Provenance: HOST-MODELLED + CORPUS-MEASURED.** This is *not* a Z80-sim
measurement. `tools/vram_upload_census.c` counts real dirty cells/rows over the
real pose corpus, then prices the shipped instruction sequence with documented
Z80 timings. It is straight-line code with measured iteration counts, so the
model is tight — but no Z80 executed it. *(RE-RUN 2026-09-13: reproduced
exactly.)*

**About 6-8k T/update, 3-4% of the pipeline.** The gap was flagged as
potentially decisive and it is not.

### The fallback column-solve, priced probabilistically

There are two column-solve implementations and the shipped C picks between them
at runtime. The **depth-plane solve** is tried first. The **endpoint solve**
(projecting both wall endpoints through a trigonometric chain) runs only when
the depth-plane solve reports the wall plane crossing zero inside the visible
run.

**CORPUS-MEASURED over 29,824 poses / 127,958 visible runs on the Game Gear code
path: the depth-plane solve succeeds on 126,809 runs = 99.10%. The fallback runs
on 0.90%.** *(RE-RUN 2026-09-13: reproduced exactly.)*

Rather than pretend the fallback is always or never executed:

| | T/span |
| --- | ---: |
| depth-plane path (DPSOLVE, Z80-SIM-MEASURED) | 4,939.0 |
| endpoint path (A14 kernel, Z80-SIM-MEASURED) | 6,253.7 |
| blended, fallback *replaces* (lower bound) | 4,950.8 |
| blended, fallback *adds after a failed attempt* (upper bound) | 4,995.2 |

**Blended column-solve: 21,288-21,479 T/update** against the 21,238 T unblended
line. The conservative upper bound moves the whole pipeline from 211,308 to
**211,549 T — a 0.1% change.** The ledger keeps 21,238 as the headline and
records this as immaterial.

### Implied update rate

**IMPLIED FROM COMPOSED COST — NOT MEASURED FPS.**

| | T/update | implied updates/s |
| --- | ---: | ---: |
| six measured kernels | 211,308 | 16.94 |
| **plus the pure-yaw upload** | **219,303** | **16.32** |
| plus the worst observed upload | 234,895 | 15.24 |

Against the gates: **23% over the 20 Hz solid target and 84% over the 30 Hz
aspiration**, before a single cycle of game logic.

### What is NOT in any of these numbers

- **Game logic, input handling, audio, VBlank service.**
- **Inter-stage glue.** No harness composes the six kernels; each has its own
  memory map. Historically the glue is where this project's surprises live
  (emit's hand-count missed by 94%, decode-clip's by 190%).
- **The block walker.** Something must read the per-cell span program, dispatch
  SPAN/SPANC/GATE/END and feed the other kernels. **No version has been written
  in Z80.**
- **The bearing cache** (caveat C1 below).
- **VDP wait states.** `otir` at 21 T/byte is faster than the VDP accepts
  outside VBlank; a real machine either runs the upload inside VBlank or pays
  more.
- **Bank-switching overhead.**

---

## 6. EXPERIMENTAL BUT EXECUTED COMPONENTS

Real Z80 code, really executed and cycle-counted, **not integrated into `src/`**.

| component | what it physically does | T | provenance |
| --- | --- | ---: | --- |
| EDGELUT3 materializer | the shipped-equivalent materializer with a hoisted edge-tile table | **175,924.4** T/pose over 300 poses; **175,827** T/pose over 125 poses | Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED (125/125 exact vs pose oracle), 1,225 bytes |
| HOIST_A | tests "does this edge draw anything" before computing its slope | **173,257.1** T/pose | Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED, −1.5% |
| BORDERHOIST | computes the wall-border tile pointer once per run instead of per column | **169,548.4** T/pose | Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED, −2.1%, 1,216 bytes |
| WALK_PROG playback loop | 20 bytes: `pop de / ld (hl),e / inc hl / ld (hl),d / pop bc / add hl,bc / dec a / jp nz`. Applies a stream of (name-table word, destination delta) pairs. | **71.7 T per edge row** (61 poses, 3,832 edge rows) | Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED **against captured old-renderer output, not against generated program data** — see §8 |
| DPSOLVE | the depth-plane column-solve, built as a twin of the A14 kernel with only the solve stage swapped | **4,939.0 T/span** | Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED, 6,039/6,039, 1,345 bytes |

**The two baseline figures 175,924 and 175,827 are the same kernel** measured
over different pose samples (300 vs 125). They differ by 0.06%. The A46
subtraction chain uses 175,827 because the stage attribution comes from the same
125-pose run.

### Stage attribution of the 175,827 T baseline

*(RE-RUN 2026-09-13 via `tools/z80_semantic_profile.py`: "VERIFIED 125/125 poses
exact against the pose oracle". Z80-SIM-MEASURED, label-attributed.)*

| stage | T/pose | share | replaced by the compiled-program architecture? |
| --- | ---: | ---: | --- |
| 6 edge setup | 41,424 | 23.6% | **yes** |
| 4 endpoint geometry | 28,163 | 16.0% | **yes** |
| 5 row extents | 25,357 | 14.4% | **yes** |
| 3 column walk | 19,085 | 10.9% | no |
| 7 edge row walk | 13,279 | 7.6% | **yes** |
| 11 row addressing | 11,715 | 6.7% | no |
| 8 edge tile select | 11,550 | 6.6% | **yes** |
| 10 full interior fill | 10,947 | 6.2% | no |
| 9 full setup | 6,469 | 3.7% | no |
| 2 endpoint decode | 5,926 | 3.4% | no |
| 1 per-run setup | 1,892 | 1.1% | no |
| **TOTAL** | **175,827** | 100% | **removed sub-total: 119,773** |

---

## 7. PROPOSED / COMPOSED FUTURE PIPELINE — the compiled edge-program architecture (A46)

**None of this exists in `src/`. It has 0 bytes in the cartridge.**

### What it proposes

Today the materializer derives, per column, where a wall's top and bottom edges
fall and which tile to draw. The proposal replaces that derivation with
**precompiled programs**: short ROM-resident sequences of (name-table word,
destination delta) pairs, selected by a lookup — the **rank dispatcher** — from
the wall's per-column step and starting phase. The runtime then does nothing but
play the program back.

### The 65,785 T figure, fully derived

```
  65,785  =  175,827      Z80-SIM-MEASURED baseline materializer, whole
            − 119,773     Z80-SIM-MEASURED label-attributed stages it removes
            +   4,609     COMPOSED playback
            +   5,121     ESTIMATED dispatch
```

| term | value | provenance |
| --- | ---: | --- |
| baseline | 175,827 | **Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED.** The 1,225-byte EDGELUT3 materializer run over 125 poses, re-verified 125/125 against the pose oracle in the same pass. |
| stages removed | 119,773 | **Z80-SIM-MEASURED.** Sum of five label-attributed stages of that same execution (table above). |
| playback | 4,609 | **COMPOSED**: CORPUS-MEASURED 64.8 edge cells/pose x 71.125 T/cell, where the per-cell figure is itself composed from a Z80-SIM-MEASURED 65.0 T loop body plus a Z80-SIM-MEASURED 43.0 T per-chain setup amortised over a corpus-derived 7.02 cells per chain. |
| dispatch | 5,121 | **ESTIMATED**: CORPUS-MEASURED 13.44 dispatches/pose x **381 T — a figure with no committed source anywhere in this repository's history** (§8). |

**Correct classification, to be used verbatim:**

> **A46 compiled edge-program materializer at C=6 (6 columns per program),
> 65,785 T/update: COMPOSED** from a Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED
> baseline, Z80-SIM-MEASURED removed-stage costs, CORPUS-MEASURED frequencies, a
> FUNCTIONALLY-VERIFIED playback kernel that was never fed generated program
> data, and an ESTIMATED dispatch cost that cannot be reproduced. **The complete
> materializer has NEVER been executed, is NOT FUNCTIONALLY-VERIFIED, and is NOT
> TARGET-MEASURED.**

Never describe 65,785 T as "measured". Never describe it as merely "modelled"
either — two of its four terms are real cycle-counted executions of real Z80
code verified against an oracle.

### The progression

| step | materializer T/update | provenance |
| --- | ---: | --- |
| current/old materializer (EDGELUT3) | **175,827** | Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED |
| C=6 compiled-program materializer | **65,785** | COMPOSED (above) |
| + BORDERHOIST | **62,076** | COMPOSED. BORDERHOIST's −3,709 T lands in the column walk, which the program architecture leaves untouched, so it composes. (HOIST_A's −2,667 T does **not** compose: its saving is inside the edge path the architecture deletes outright.) |

### Best honest future-pipeline model

Using the corrected depth-plane column-solve and a representative upload:

| line | T/update | provenance |
| --- | ---: | --- |
| bearing lookup | 5,833 | COMPOSED (caveat C1) |
| decode-clip | 11,036 | COMPOSED from Z80-SIM-MEASURED |
| GATE | 2,339 | COMPOSED from Z80-SIM-MEASURED |
| column-solve, depth-plane | 21,238 | COMPOSED from Z80-SIM-MEASURED |
| depth sort | 1,314 | Z80-SIM-MEASURED |
| A46 + BORDERHOIST materializer | 62,076 | COMPOSED, largely unexecuted |
| **sub-total** | **103,836** | |
| + pure-yaw VRAM upload | 7,995 | HOST-MODELLED + CORPUS-MEASURED |
| **TOTAL** | **111,831** | **COMPOSED** |

The arithmetic supports **roughly 110-115k T/update** (109,728 with the
forward-walk upload, 111,831 with the pure-yaw upload, 112,072 if the
column-solve fallback is priced in at its conservative bound).

**111,831 T/update implies 32.0 updates/s.**

> **IMPLIED FROM COMPOSED COST — NOT MEASURED FPS.**

That would clear the 30 Hz aspiration (119,318 T) with ~6% margin — *if* the
architecture works as designed, *if* integration adds no cost, and *if* the
cartridge problem below is solved. None of those is established.

### The ROM problem, which is decisive

| C (columns per program) | table entries | dispatch ROM | thresholds | bodies | **total** |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 1,337,756 | 2.55 MB | 80 KB | 64.3 KB | **2.69 MB** |
| **6** | **1,350,306** | **2.58 MB** | 120 KB | 226.9 KB | **2.91 MB** |

*(RE-RUN 2026-09-13: reproduced exactly.)*

**The cartridge is currently configured `-Wm-yo4` = 64 KiB**, and the committed
ROMs are 65,536 bytes — but that setting is an artefact of the 2D GOAP demo, not
a platform limit or a project constraint.

Real Game Gear cartridges are routinely **512 KiB and 1 MiB**, and the mapper
reaches **4 MiB**. The project's own position: **up to 4 MiB is available,
<= 1 MiB is preferable, and ROM size should not be bought at the cost of
fidelity, ability to scale, or update rate.**

So **2.91 MB is affordable** — it is inside the 4 MiB ceiling. It is above the
1 MiB comfort point, which makes shrinking it *desirable*, not a blocker. What
it does still require is a **banking scheme, whose cycle cost is unmodelled**
(caveat C6).

**Bank-switching cost is not in any A46 figure.** A multi-megabyte table on a
machine with a 64 KiB address space means dispatches will cross banks. That cost
is **unmodelled** in every A46 number here, and it is the real open question
about table size — not the byte count itself.

---

## 8. Forensic answer: what has the compiled-edge-program experiment (A46) actually demonstrated?

Twelve questions, each answered against evidence re-run at `8991f14`.

| # | question | verdict |
| ---: | --- | --- |
| 1 | Was the proposed program vocabulary generated correctly? | **PARTIAL** |
| 2 | Was its coverage over the pose corpus verified? | **PARTIAL** |
| 3 | Was selecting the correct program for a real span/pose verified? | **NO** |
| 4 | Was the rank/dispatch algorithm functionally verified? | **NO** |
| 5 | Was the playback loop functionally verified? | **YES** (scoped) |
| 6 | Was playback verified using actual generated A46 program data? | **NO** |
| 7 | Was dispatch joined to playback? | **NO** |
| 8 | Was the complete A46 materializer ever executed in `z80core.py`? | **NO** |
| 9 | Was its output compared against the renderer/name-table oracle? | **NO** |
| 10 | Was its complete cycle count directly measured? | **NO** |
| 11 | Was anything assembled into a real Game Gear ROM? | **NO** |
| 12 | Was anything run in a Game Gear emulator or on hardware? | **NO** |

### Evidence

**1. Vocabulary generated correctly — PARTIAL.**
`tools/edge_family_verify.c` is a genuine check: it walks real camera poses,
runs the **real renderer's** projection and run-insertion, and for each real run
and each real column recomputes the expected tile *with the renderer's own
per-column code* (`draw_run`, profile branches included), then compares it to
the family/phase model. Result, re-run today:

| endpoint family | columns | exact | period |
| --- | ---: | ---: | ---: |
| `71-h` FULL top | 351,901 | 351,901 | 1024 EXACT |
| `72-h` LINTEL/RAISED top | 449,372 | 449,372 | 1024 EXACT |
| `72+h` FULL bot, RISER bot | 422,026 | 422,026 | 1024 EXACT |
| `72-(h>>1)` LINTEL bot | 138,061 | 138,061 | 2048 EXACT |
| `72+h-(h>>2)` RAISED bot, RISER top | 381,436 | 381,436 | 4096 EXACT |
| **total** | **1,742,796** | **1,742,796** | |

So the **generating formula** is FUNCTIONALLY-VERIFIED against the renderer over
1.74 million real columns. That is a real result and it is the strongest thing
A46 has. It is PARTIAL and not YES because **the programs themselves were never
produced as data** — see the "never generated" section below.

**2. Coverage over the pose corpus — PARTIAL.** Two named holes:
- `edge_family_verify` reports **1,686 inverse-depth-clamped columns** routed to
  a "separate path" the model does not cover.
- `edge_dispatch_verify` **skips clamped samples entirely** — 1,398,102 skipped
  per small family at C=6.
- `target_solve_census` reports **13,530 distinct (step, phase) A46 keys** present
  in the corpus, but **nothing has ever checked those corpus keys against a
  baked program set**, because no baked program set exists.

**3. Selecting the correct program for a real span/pose — NO.** What was proven
is a *mathematical identity*: that `program = T[family][step][H mod M][rank(u)]`
is conflict-free. That sweep is **exhaustive and synthetic** — `step` over its
entire `[-2048, 2047]` range and `u` over `[0,128)` — not corpus-driven. Re-run
today: **0 conflicts, 1,350,306 table entries at C=6; 0 conflicts, 1,337,756 at
C=4.** No corpus pose has ever been pushed through a dispatch to retrieve a
program, and there is no table to retrieve from.

> *Documentation correction:* `TODO_DEFERRED.md` records "ZERO conflicts over
> **34,538,688** observations". The C=4 per-family counts it lists sum to
> **34,738,688**. The zero-conflict result reproduces exactly; the total is a
> transcription error of 200,000.

**4. Rank/dispatch functionally verified — NO, and worse than previously
recorded.** The prior write-up said the 381 T kernel "ran in `z80core.py`" but
its output was never read back and its inputs were random. Searching the
**entire git history** (`git log --all --name-only`) for any dispatch or rank
artifact returns exactly one file: `tools/edge_dispatch_verify.c`, a host C
program. `grep` for `381` across `tools/` returns nothing.

> **Correction: we previously believed the 381 T rank-dispatch figure was an
> unverified measurement. The evidence now shows it is an *unreproducible* one —
> no Z80 rank-dispatch kernel exists in the repository or in any commit. This
> changes the dispatch term's label from "COMPOSED from an unverified kernel" to
> ESTIMATED, and means the 5,121 T dispatch line cannot currently be checked by
> anyone.**

**5. Playback loop functionally verified — YES, within its scope.** The 20-byte
WALK_PROG kernel executed on `z80core.py`; the resulting 20x18 name table was
compared cell-by-cell against expectation and was **EXACT on every run-edge
tested** (61 poses, 3,832 edge rows, re-run today at 71.7 T/edge row). What this
proves is that the loop correctly applies a (word, delta) stream — nothing about
where the stream comes from.

**6. Playback fed real generated program data — NO.** `capture()` in
`tools/z80_edge_prog_bench.py` instruments the **shipped EDGELUT3 kernel** and
records its own stores; `build_program()` then re-encodes exactly those captured
cells into a stream. The bench's own docstring is explicit:

> *"the words are the renderer's own, so playback is exact by construction and
> the comparison below is a real check of the KERNEL, not of the baker."*

So playback replayed the old renderer's output. It has never consumed a program
produced by the program generator.

**7. Dispatch joined to playback — NO.** The playback kernel sets its program
address with a compile-time constant (`ld sp,0x8000`). The real architecture must
take that address from the dispatcher, which needs **`ld sp,hl` (opcode 0xF9)**.
Verified today: **`0xF9` does not appear anywhere in `tools/z80core.py`.** The
interpreter would raise `unimplemented opcode` on it. The hand-off has never been
assembled, let alone measured.

**8, 9, 10. Complete materializer executed / oracle-compared / cycle-counted —
NO, NO, NO.** The only A46 code ever executed on a Z80 is the 20-byte playback
loop. There is no column walk, no border path, no interior fill, no dispatch, no
integration. 65,785 T is arithmetic.

**11, 12. ROM / emulator / hardware — NO, NO.** `make state-census`: *A46 tables
in `src/`: 0 files -> 0 bytes. A46 kernel composed into `src/`: NO. A46 ROM in
the cartridge: 0 bytes.*

### What "never generated" means, precisely

**The 2.91 MB is a count multiplied by a bytes-per-entry constant, not a
measurement of emitted bytes.** Verified today by reading the sources:

- `tools/edge_program_bake.c` computes each program into a small stack array
  (`uint16_t cur[MAXL]`), hashes it into a set, and **discards it**. It contains
  **no `fopen`, no `fwrite`, no `FILE *`** — confirmed by grep. The reported
  body figure is `distinct programs x L x bytes-per-entry`.
- `tools/edge_dispatch_verify.c` does the same for dispatch: it counts reachable
  `(family, step, base, rank)` entries and never materialises them. 2.58 MB is
  `1,350,306 x 2`.
- Neither emits a `.inc`, `.bin` or `.s`. Nothing was assembled. Nothing was
  linked.

So the programs were generated **transiently, one at a time, purely to be
counted**, then thrown away — and because the playback measurement fed on
captured renderer output instead, **no generated A46 program has ever been
consumed by any kernel, timed or otherwise.**

### Summary: what A46 *has* earned

1. The endpoint-family/phase model reproduces the renderer's per-column tile
   **exactly over 1,742,796 real columns** from real poses. FUNCTIONALLY-VERIFIED.
2. The dispatch key decomposition is **conflict-free over an exhaustive synthetic
   sweep** of its entire input domain. HOST-MODELLED, exhaustive.
3. A 20-byte Z80 playback loop applies a (word, delta) stream correctly at
   **71.7 T/edge row against the shipped path's 1,023 T/edge row** — a 14x
   reduction on that specific work. Z80-SIM-MEASURED + FUNCTIONALLY-VERIFIED.
4. The ROM cost is **~2.91 MB at C=6**, from counts. HOST-MODELLED.

That is a well-founded *architecture*. It is not a materializer.

---

## 8a. PROGJOIN — the compiled edge-program chain, executed

**Experiment:** PROGJOIN, `make progjoin`. **What it does:** takes real
run-edges from the pose corpus, runs a real Z80 **rank dispatcher** (the lookup
that selects which precompiled edge program to execute), points the stack at the
selected program with `ld sp,hl`, plays the program back into the name table,
and compares the cells against the renderer's own `draw_edge`.

This is the first time any generated compiled-edge-program data has been
consumed by any kernel.

### What was built

| piece | what it is |
| --- | --- |
| `tools/z80core.py` | **`ld sp,hl` (0xF9, 6 T) implemented**, with `tests/test_z80_ld_sp_hl.py` checking encoding, timing, HL→SP copy, flag preservation, `pop` usability and full-range round trip. |
| `tools/edge_progjoin_bake.c` | **Writes actual bytes** — step→slot map, descriptors, threshold lists, dispatch blocks and program bodies — unlike `edge_program_bake.c`, which contains no `fopen`. Programs are built from the MODEL's formulas, never from renderer output. |
| `tools/z80_progjoin_bench.py` | 293-byte Z80 kernel: dispatch → `ld sp,hl` → playback, looping the chunks of a run-edge with the destination cursor carried across them. |

### Result: the chain works and is EXACT

| | |
| --- | ---: |
| poses | 2,486 |
| run-edges in corpus | 20,840 |
| run-edges executed | **16,700 (80.13%)** |
| dispatches executed | 24,587 |
| cells played | 115,089 |
| **wrong cells** | **0** |
| **stray writes** | **0** |
| **verdict** | **EXACT** vs the renderer's own `draw_edge` |

### Result: the cost is much higher than composed

Cycles attributed by program counter, not fitted:

| stage | measured | prior figure | ratio |
| --- | ---: | ---: | ---: |
| **rank dispatch** | **1,038.8 T** per dispatch | 381 T claimed | **2.73x** |
| **playback** | **68.4 T** per cell | 71.1 T composed | 0.96x — **the playback composition was sound** |
| chunk advance | 243.7 T per chunk | **absent from every composed figure** | — |
| per-run-edge setup | 167.0 T per run-edge | **absent from every composed figure** | — |

The 381 T figure priced a **fragment**: its description covers masking `u`,
extracting the base, indexing a threshold list and four compares. It does not
include the family/length dimension, the body-pointer indirection, the
cell-count lookup, or forming the program address — all of which a dispatch must
do to hand playback something to execute. The kernel measured here is also
**untuned**; the descriptor lookup is re-done per chunk although family is
constant per run-edge, so there is real headroom. Neither fact closes a 2.7x gap.

### Three defects the published verification could not have caught

`edge_dispatch_verify.c`'s "zero conflicts over 34.7M observations" hashed only
each program's C-column tile sequence — **with no successor column, no
partial-chunk case, and multi-row columns truncated at four rows**
(`if (n > 4) n = 4;`). Running the chain found:

1. **The key does not distinguish chunk length.** A full C-column chunk and a
   short final chunk of a run are different programs; `T[fam][step][base][rank]`
   maps them to one entry. First hit at family 0, step 0, base 6, rank 6.
2. **The key needs C+1 thresholds, not C.** A C-column program's drawn cells
   need heights h_0..h_C, which C thresholds cover. But the self-chaining
   destination delta also needs where the NEXT chunk starts, and that depends on
   h_{C+1} — the lookahead column's right endpoint. So the selector reads **C+2
   heights**. First hit at family 0, step 5, base 7, rank 0, with accumulators
   3008 and 2974 agreeing on every drawn cell and disagreeing only on the final
   chaining delta. Fixing this costs one more rank row.
3. **Programs carry no screen clipping, and 19.2% of run-edges need it.**
   `draw_edge` clamps the drawn row range to the 18-row viewport; a
   position-independent program cannot carry that clamp. A further 0.65% hit the
   255 inverse-depth clamp. **Only 80.13% of run-edges are bakeable as things
   stand**; the rest need a fallback that exists in no A46 budget.

### What that clipping actually is, and why it is cheap to fix

It is **near-wall view-frustum clipping**, top and bottom — *not* occlusion.
Occlusion is a separate mechanism entirely: the depth sort plus near-to-far
ownership, where the first writer of a cell wins.

The viewport is 144 scanlines centred on y = 71.5, so a wall whose half-height
`h` exceeds about 72 has its top edge above the screen or its bottom edge below
it. Measured over the corpus, the split is perfectly clean:

| family | excluded off TOP | off BOTTOM | mean h |
| --- | ---: | ---: | ---: |
| `71-h` FULL top | 958 | 0 | 95.7 |
| `72-h` LINTEL/RAISED top | 1,190 | 0 | 96.9 |
| `72+h` FULL/RISER bottom | 0 | 1,190 | 96.6 |
| `72+h-(h>>2)` RAISED bot/RISER top | 0 | 666 | 111.2 |
| `72-(h>>1)` LINTEL bottom | 0 | 0 | — |

Top edges only ever leave the top, bottom edges only ever the bottom, at wall
half-heights of 95-111 against a ~72 threshold. These are **close walls**. The
LINTEL bottom family never clips, because its `h>>1` halves the excursion.

**And the fix is cheap, which was not previously known.** Tested over the whole
corpus: the renderer's clipped output is *exactly* the unclipped program's cells
with out-of-range rows dropped — **39,570 of 39,570 columns, no exceptions**.
The cell CONTENT is unchanged; only which cells get written changes. Each
column's tile is a pure function of its row, so clamping the row range simply
drops leading or trailing cells (1.01 dropped of 1.50 per clipped column).

**So the "clipping fallback" is not a second renderer.** It is the same baked
program, played with leading/trailing cells skipped — a skip count and a
shortened play count, computable at dispatch from the absolute row the cursor
already holds. That makes the 19.87% recoverable rather than an architectural
dead end.

### What this does to the 65,785 T figure

On the 80.13% of run-edges it can handle, the architecture is a large real win:
the stages it replaces cost **95,979 T/pose** for that subset and the measured
compiled-program path costs **17,001 T/pose** — **5.6x cheaper**. The idea works.

The whole-materializer arithmetic, however, moves against it:

| | materializer T/pose | provenance |
| --- | ---: | --- |
| composed claim | 65,785 | COMPOSED, largely unexecuted |
| **measured path + old-path fallback for the 19.87%** | **96,848** | **COMPOSED from Z80-SIM-MEASURED per-unit costs + CORPUS-MEASURED frequencies** |
| if clipping were free and every edge baked | 77,269 | COMPOSED, optimistic bound |

Whole update, adding the five corrected front-end stages (41,760 T), BORDERHOIST
(−3,709 T) and a representative pure-yaw upload (7,995 T):

| | T/update | implied rate |
| --- | ---: | ---: |
| composed claim (superseded) | 111,831 | 32.0 /s |
| **best case, clipping free** | **123,315** | **29.0 /s** |
| **measured + fallback** | **142,894** | **25.1 /s** |

> **IMPLIED FROM COMPOSED COST — NOT MEASURED FPS.**

**Against the actual gates, this is a success.** The solid target is 20 Hz /
178,977 T, and the measured architecture clears it with **25% margin even in the
pessimistic measured+fallback case** — while the current implemented pipeline
(219,300 T, 16.3 /s) does not clear it at all. The 30 Hz aspiration is missed at
123,315-142,894 T; that is a stretch goal, not the spec.

*(Correction: an earlier write-up of this result called 30 Hz "the target it was
pursued for" and treated missing it as a failure. That was wrong — 30 Hz is
aspirational, 20 Hz is the solid target, and this clears it.)*

### What PROGJOIN still does not close

The forensic answers in §8 change as follows, and only these:

| # | question | was | now |
| ---: | --- | --- | --- |
| 3 | correct program selected for a real span/pose? | NO | **YES** — 24,587 dispatches on corpus inputs, output verified by the cells it produced |
| 4 | rank/dispatch functionally verified? | NO | **YES** — real Z80 kernel, output read back and checked |
| 6 | playback fed real generated program data? | NO | **YES** — programs emitted to disk and consumed |
| 7 | dispatch joined to playback? | NO | **YES** — `ld sp,hl` implemented, tested, and used |
| 1 | vocabulary generated correctly? | PARTIAL | **YES** for the bakeable 80.13%; the excluded 19.87% has no program |
| 2 | coverage over the pose corpus verified? | PARTIAL | **PARTIAL** — now quantified: 80.13% covered, 19.21% needs clipping, 0.65% clamped |
| 8 | complete materializer executed? | NO | **PARTIAL** — the edge path is executed; column walk, interior fill, borders and row addressing are untouched and not re-integrated |
| 9 | output compared against the oracle? | NO | **PARTIAL** — edge cells vs the renderer's `draw_edge`, not a whole 20x18 name table |
| 10 | complete cycle count measured? | NO | **PARTIAL** — the edge path is measured; the whole materializer is still composed |
| 5, 11, 12 | playback verified / ROM / emulator | YES, NO, NO | unchanged |

---

## 9. INVALIDATED / SUPERSEDED — numbers that must not return

| number | what it was | why it is void |
| ---: | --- | --- |
| **48,600 T / 58,693 T** | "whole-update budget", A12/A13 era | Carried "emit 21,756 T" in place of a stage that measured **228,403 T**. Understated the dominant cost ~10x. |
| **~61 Hz / ~73 updates/s / "1.02-1.23 updates per frame"** | implied rates from the above | Void with their budget. **This is the origin of any impression that the renderer ever ran near frame rate.** |
| **69,098 T / 0.86 updates/frame** | depth-sort-era budget | Same defect: `emit` placeholder + wrong column-solve path. |
| **26,820 / 26,891 T column-solve** | budget line in every rung up to A49 | Priced the **endpoint/fallback** solve, which the target takes on **0.90%** of runs. Superseded by 21,238 T. |
| **~13,300 T column-solve** | A49 host model of the depth-plane path | The built kernel measured 21,238 T. **The model was 37% too optimistic.** This is the project's standing evidence that composing measured component costs across a path change does not predict the result. |
| **223,266 T** | previous "current pipeline" headline | Used the wrong column-solve and pre-BORDERHOIST materializer. Superseded by 211,308. |
| **113,127 T** | previous A46 whole-update | Used the wrong (fallback) column-solve. Superseded by 103,836 / 111,831. |
| **1.07 MB A46 ROM** | A44 | Corrected by A45 to 3.00 MB at L=4; 2.91 MB at C=6 under the dispatch model. |
| **65,785 T A46 materializer** | composed compiled-edge-program cost | Superseded by PROGJOIN (§8a): dispatch measured at 2.73x the assumed cost, two cost terms were missing entirely, and 19.87% of run-edges cannot be baked at all. Measured recomposition **96,848 T**; clipping-free bound **77,269 T**. |
| **381 T rank dispatch** | assumed dispatch cost | Measured at **1,038.8 T** for a dispatch that actually returns a playable program (§8a). |
| **A46 reaches ~32 updates/s** | composed whole update | Superseded: **25.1 /s** measured+fallback, **29.0 /s** clipping-free bound. Does not reach the 30 Hz gate. |
| **"37.9"** | GOAP AI demo | Video frames per logical AI tick, not a rate and not the renderer. Real rate: 1.58 world ticks/s. |
| **"60 fps"** | GOAP runtime verification | ffmpeg capture rate / NTSC display cadence. |

---

## 10. Open caveats

**C1 — the bearing cache is assumed, not built.** The 5,833 T bearing line is
`9.12 distinct corners x 639.6 T`. The 9.12 figure is CORPUS-MEASURED and the
639.6 T is Z80-SIM-MEASURED — but **9.12 is a de-duplicated count that requires a
bearing cache, and that cache has never been written.** The naive count is 20.15
evaluations per update. Without the cache the line is **~12,888 T, a +7,055 T
swing (+3.3% of the whole pipeline).** This is a functional gap, not an
efficiency one, and the current ledger total depends on it.

**C2 — no end-to-end update exists.** Six verified kernels, each proving its own
stage against its own oracle, and nothing that composes them. Separate memory
maps, separate harnesses. Until one update runs start to finish and matches the
host oracle's 360 words, the architecture is proven in pieces and unproven as a
whole.

**C3 — the block walker does not exist in Z80.** See §5.

**C4 — seven corners have no accurate baked leaf**; the `0xff` escape marker is
unhandled at runtime. Open.

**C5 — Polar vs TileSector oracle divergence**, traced upstream. Open.

**C6 — A46 bank switching is entirely unmodelled.** The 2.91 MB table is inside
the 4 MiB cartridge ceiling, so size is affordable; the open cost is how often a
dispatch crosses a bank and what that costs.

**C7 — VDP wait states are not modelled** in the upload figure.

---

## 11. Standing rules

1. The **normal** column-solve path is the depth-plane solve (99.10%); the
   **fallback** is the endpoint solve (0.90%). Never price the fallback as the
   stage.
2. A composed budget is not a measurement. Composing measured component costs
   across a path change mispredicted a real kernel by **37%**.
3. Display cadence is not renderer frame rate. Video frames per logical tick is
   not a frame rate at all.
4. **No renderer update rate may be called "measured"** until a WRAM update
   counter is read from a real ROM.
5. Any A46 figure must state that the complete materializer has never executed.
6. Quote no number from this project without its provenance label.
