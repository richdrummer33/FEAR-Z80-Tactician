# Parity implementation log — 2026-09-14

Branch: `validation/parity-enforcement-20260914`

Implemented in this rung:

1. Added a human-readable parity matrix separating ROM-equivalent, ROM-experimental, partial, and Z80-only work.
2. Added a machine-readable parity status file.
3. Added a deterministic parity-materializer builder that applies the current proven ROM integration rungs in one explicit order and restores the canonical source afterward.
4. Added a generated-source checker that fails when documented parity markers are absent or the original per-column two-endpoint run loop survives intact.
5. Added CI that preserves baseline assembly, generated assembly, and their diff as artifacts.
6. Added manifest validation CI so an integrated status cannot silently point at a missing implementation file.
7. Documented the convergence order: lock current exact-ROM rungs, collapse patch scripts into one source of truth, profile residual old-skeleton cost, then introduce a restricted finite-program ROM executor before attempting PROGJOIN/dispatcher descendants.

This rung intentionally does **not** claim finite edge programs or PROGJOIN are integrated. They remain the largest known architectural parity gap.

## PROGJOIN ROM bridge progress

The first real banked-ROM semantic probe proved case 0 exactly but stopped with `done=0 cases=1 fail=0 lookup_miss=0 bounds_fail=0`. The same result survived the Frame-2 vector-lifetime fix, which ruled out the suspected live-ROM-pointer fault for that symptom. The host runner was then found to have a fixed 120-frame watchdog while each semantic case deliberately performs a complete 1280-byte 32-bit FNV hash on the real Z80. The watchdog has therefore been increased to 30000 frames so the full 192-case corpus can actually finish before being classified as a hang.

The banked selector and body player have also been extracted from the probe into the reusable Game Gear runtime pair:

- `src/tilesector_polar_progjoin_runtime.h`
- `src/tilesector_polar_progjoin_runtime.c`

The standalone semantic ROM probe now links and exercises that shared runtime rather than carrying a private copy of dispatch/playback. This is an intentional convergence step: the eventual playable-renderer splice must consume the exact same bank-switching and sparse-dispatch implementation that is proven by the real-ROM semantic harness.

The next graduation gate remains unchanged: the reusable runtime must pass the full real-ROM corpus before any successful finite-program integration claim is made for the renderer itself.

## Playable live-rung failure: diagnosis and fix

The `GG PROGJOIN playable FULL live rung` workflow built the experimental 1 MiB
playable ROM green and then failed in the new exactness step with:

```
only 0/48 loops, instructions=250000000
```

The failure was **not** in the ROM. It was in how the step named the symbol
file. `makebin` emits two symbol files for the same link:

- `.noi` — NoICE/no$gmb: `DEF _g_ts_prof_phase 0xC3C3`
- `.sym` — makebin: `00:C3C3 _g_ts_prof_phase`

The live rung passed the `.noi` file to `polar_ab_profile`, whose `find_symbol`
only understood the `.sym` shapes. Its fallback pattern is `"%x %255s"`, and
`DEF` is itself a valid hexadecimal literal, so every line of a `.noi` file
parses as address `0x0DEF` with the symbol name in the second field. All four
required globals therefore resolved to `0x0DEF` — a constant byte in fixed ROM
bank 0 — so the profile phase never appeared to change, no logical update was
ever recorded, and the run burned its whole 250,000,000-instruction budget
before reporting zero loops.

This was reproduced locally on a bit-identical ROM (CRC `493F7C38`, matching the
CI log) and then cleared by passing the `.sym` file to the same binary:

```
AB_PROFILE ... scenario=roomA-turn    loops=48 warmup=6
AB_PROFILE ... scenario=roomA-forward loops=96 warmup=6
LIVE_ROM_PASS roomA-turn    updates=48 mean=336005.3T hz=10.65
LIVE_ROM_PASS roomA-forward updates=96 mean=584334.2T hz=6.13
```

Three changes were made:

1. The live rung now preserves and profiles the `.sym` file.
2. `find_symbol` in all nine symbol-reading tools parses `DEF <symbol> <value>`
   explicitly, ahead of the hex-first forms, so a `.noi` file can no longer be
   silently misread as a file of `0x0DEF` symbols. Verified: the patched
   profiler produces byte-identical CSV output from the `.noi` and `.sym` files
   of the same link, and that output is byte-identical to the pre-patch `.sym`
   run.
3. `polar_ab_profile` now rejects any resolution that lands outside Game Gear
   work RAM (`0xC000`-`0xDFFF`) up front, naming each address, instead of
   discovering the problem as an empty trace 250M instructions later.

The live-rung smoke assertion was also tightened. It previously accepted any
non-empty CSV. It now requires the expected update count, an actual change in
player state for the scenario, and more than one distinct name-table hash across
the trace, so a frozen or degenerate trace cannot read as a pass.

### What this does and does not establish

It establishes that the PROGJOIN-spliced playable ROM **runs**, advances player
state, and mutates the name table under deterministic input, and that its
per-update cycle costs are now measurable.

It does **not** establish playable exactness. There is still no like-for-like
baseline ROM, no name-table hash A/B against one, and no counters for PROGJOIN
attempts, successful compiled runs and fallback reasons. Until those exist a
matching trace could still mean every run-edge fell back to the old edge path.
The finite-program/PROGJOIN status therefore stays `ROM-EXPERIMENT`, and
`playable_renderer_integrated` in `docs/PARITY_STATUS.json` is deliberately left
unchanged. Items 3-6 of the convergence sequence are the next rung.

## Playable A/B rung: exact, but not yet a win

With the harness fixed, convergence items 3-6 were run for the first time. The
live rung now builds three like-for-like 1 MiB ROMs from one parity
materializer and one live splice, identical in ROM size, bank count, appearance
mode and linked PROGJOIN assets, differing only in the compiled path:

| variant | build | purpose |
| --- | --- | --- |
| `baseline` | `TSPF_PROGJOIN_FULL=0` + stats | legacy edge solver; counters prove zero compiled playback |
| `progjoin` | `TSPF_PROGJOIN_FULL=1` | compiled edges, uninstrumented, for the cycle number |
| `progjoin-stats` | `TSPF_PROGJOIN_FULL=1` + stats | compiled edges with accounting |

`tools/build_progjoin_live_rom.sh` builds a variant, `tools/progjoin_live_ab.py`
adjudicates the traces. Measured on `roomA-turn` (48 updates) and
`roomA-forward` (96 updates):

```
                       baseline      progjoin     delta    compiled run-edges
roomA-turn            324,804.8T    336,005.3T    +3.45%     3/143   ( 2.1%)
roomA-forward         513,397.8T    584,334.2T   +13.82%   142/415   (34.2%)
```

### Exactness: pass

Player state and the complete 20x18 name-table hash agree on **every** measured
logical update, in both scenarios, for both the plain and the instrumented
PROGJOIN ROM. `progjoin` and `progjoin-stats` also agree with each other, so the
counters are proven not to perturb what they measure. `fb_play_top` and
`fb_play_bot` are both zero, so the partial-write hazard below never fired on
this corpus and the atomic-replacement model holds in practice.

### Coverage and cycles: the real result

This is exactly the case the parity contract exists to catch. The name-table
hashes match perfectly — and on their own they would have supported a parity
claim. The counters say otherwise: the compiled path ran for **2.1%** of
attempted run-edges while turning and **34.2%** while moving forward. The rest
fell back to the legacy edge solver, which is why the output agrees.

The compiled path is currently a net **regression**: +3.45% and +13.82% cycles
per update. Every attempt pays step-map lookup, threshold walk, descriptor read
and record walk across bank switches; at these hit rates the majority of that
work buys nothing, and the successes do not recover it.

Attributing the misses at the point of refusal (the `fb_sel_*` counters cannot
do this themselves — the top family is preflighted first and short-circuits, so
it absorbs every uncovered run-edge regardless of family):

```
                  miss_step   miss_desc   miss_rank      total
roomA-turn              103          29           8        140
roomA-forward           103         148          22        273
```

`miss_step` means the run-edge's `step` is absent from the compiled corpus's
step vocabulary entirely. `miss_desc` means the step is present but no
descriptor exists for that `(slot, family, want)` — that chunk width is not
compiled for that step. The totals reconcile exactly with `fb_sel_top`.

So the first-order problem is **corpus coverage, not dispatcher maturity**. The
corpus is censused from `coverage_pose_oracle.txt` at `--window 40`; the live
renderer is producing run-edges outside what that bakes.

### Consequence for the convergence order

The handoff sequences the Z80 run-edge invariant hoists (items 24-26) and
direct-rank dispatch (item 27) after playable measurement. Those measurements
now exist, and they reorder the work: hoisting dispatch would optimise a path
taken 2-34% of the time, against a cost dominated by attempts that miss. The
Z80 audit's 3.42% dispatch saving cannot close a 13.82% whole-update
regression.

Coverage should come first. Until it does, no hoist result measured on this ROM
would mean anything, and PROGJOIN stays `ROM-EXPERIMENT`;
`playable_renderer_integrated` remains unchanged.

### Known hazard, currently latent

`tsp_progjoin_play_plan_gated` validates each destination motion *after*
storing the cell, so a refusal returns 0 having already written part of the
edge, and the legacy path then redraws over it. The two `fb_play_*` counters
watch for this and the A/B fails if either becomes non-zero. It is zero today,
but the ordering should be fixed rather than left to the corpus to avoid.

## Why the corpus misses: the decimation is load-bearing

Root-causing the 2.1% / 34.2% coverage. Two things had to be established: what
the corpus is built from, and whether building it from more would help.

### The oracle samples 0.52% of poses, and none of the ones gameplay visits

`build/coverage_pose_oracle.txt` comes from `coverage_potential_probe`, invoked
as `16 <out> 12`. It decimates on three independent axes:

| axis | sampled | of |
| --- | --- | --- |
| heading | 16 (`yaw_step=16`) | 256 |
| sub-cell offset | 4 fixed `{0,0},{7,3},{3,7},{11,5}` | 64x64 |
| pose dump stride | 1 in 12 | enumerated poses |

29,824 poses are enumerated and **2,486 are dumped** — 0.52% of the 477,184
enumerable poses, where "enumerable" has already thrown away 4,092 of 4,096
sub-cell positions.

The decisive check is not the ratio, it is membership. Oracle positions are
`gx*64 + 32 + {0,7,3,11}`. Live positions from the measured traces:

```
roomA-turn      1 distinct position visited,  0 on the oracle grid
roomA-forward  96 distinct positions visited, 0 on the oracle grid
```

**Zero of 97.** The player walks straight through the gaps between sampled
positions, and turning sweeps headings that are sampled 1-in-16.

So the compiled path was never covering the start area. `step` is quantized
(`(inv1-inv0) * recip[n] >> 2`), so unrelated poses can collide on the same
step value, and every compiled hit observed is an incidental collision rather
than designed coverage. That explains the shape of the results exactly:
collisions are common near the spawn geometry (34.2%), rarer further along it
(6.5% measured from update 60), and absent while turning (0/99), where the
geometry has no counterpart in the sample at all.

### Sampling more is not available in this format

The obvious fix is to bake denser. Measured, not assumed — a full census and
pack run over an oracle sampled across all 256 headings (`yaw_step=1`,
`stride=17`, 28,070 poses, chosen to decorrelate stride from the yaw period):

| oracle | poses | steps | semantic entries | unique bodies | census min payload |
| --- | --- | --- | --- | --- | --- |
| shipped `y=16 s=12` | 2,486 | 549 | 4,311 | 2,816 | 355,220 B (22 banks) |
| dense `y=1 s=17` | 28,070 | 2,119 | 62,741 | 18,061 | 2,270,587 B (139 banks) |

At equal pose budget the axis that matters is heading: 29,824 poses at 16 yaws
yields 4,604 distinct `(step, want)` keys, while 28,070 poses spread over all
256 yaws yields 6,182 — 34% more from sampling alone.

The vocabulary does saturate. Over the entire 477,184-pose space there are
812,346 FULL runs but only **2,496 distinct steps** and **7,330 distinct
`(step, want)`**, so the dense sample above already holds ~85% of it. The target
is finite. It is just far larger than what ships.

Running the real sparse pack on the dense corpus does not produce a size. It
fails:

```
record offset overflow
```

`tools/gg_progjoin_sparse_direct.py` refuses at `len(records) > 0xfffe`, because
the descriptor stores each record-list offset as a **uint16** (`0xFFFF` is the
absent sentinel) and `tsp_progjoin_dispatch_body` reads it with `rd16p`. Records
are 4 bytes per semantic entry plus a terminator per descriptor, so:

```
shipped   4,311 entries x 4 + 1,732 terminators =  18,976 B   (29% of cap)
dense    62,741 entries x 4 + 11,351 terminators = 262,315 B   (4.0x over cap)
format cap                                          65,534 B  (~15,240 entries)
```

So the shipped corpus already consumes 28% of every semantic entry the dispatch
format can address, and full pose coverage needs roughly 5x more than the format
can express — before considering that the packed assets would also want ~45 of
the cartridge's 64 banks, alongside the renderer, the game and the projection
tables.

### What this means

The decimation is not an oversight to be turned up. It is what makes the corpus
fit. The Z80 research harness never met this constraint: as
`gg_progjoin_corpus_census.py` says in its own docstring, it "deliberately bakes
small windows because its executable test has a flat 64 KiB address space", and
it only ever evaluates the window it baked. The locked target census (19,912
run-edges, 31,806 chunks, 160,717 cells) and the 52,266,733 T PROGJOIN figure are
measured over that 0.52% sample, so they describe a corpus that covers almost
none of the poses the game actually renders.

That reorders the remaining work more sharply than the previous entry did.
Porting the run-edge invariant hoists (items 24-26) and direct-rank dispatch
(item 27) optimises dispatch on a path that currently succeeds 0-34% of the time
and cannot be made to succeed much more often without a different format. None
of those ports should be attempted before one of these is settled:

1. **Reduce key cardinality.** 7,330 `(step, want)` pairs over the whole pose
   space is the real target. If `step` can be quantized or re-derived so that
   nearby geometry shares a program, the vocabulary shrinks toward something a
   cartridge can hold. This is the only direction that makes the architecture
   work as intended, and it must be proven exact, not approximated.
2. **Widen the dispatch format.** Larger record offsets and more record banks
   are necessary for any denser corpus, but on their own they only move the wall
   from 15,240 entries to a bank budget that still does not fit.
3. **Scope PROGJOIN to a provably covered subset** and make the miss path nearly
   free, so the executor stops charging for run-edges it cannot serve. The
   current miss costs a step-map lookup, a threshold walk, a descriptor read and
   a record walk across bank switches, which is why coverage this low reads as a
   13.82% whole-update regression.

Option 3 is the only one that improves the current ROM without new research.

### Two notes recorded from play testing

Neither affects the measurements above, but both were worth confirming:

- **Startup.** A real cartridge can take 5-10 s before it settles. The profiler
  arms on the first render-phase transition, so it cannot begin measuring before
  the loop runs, and re-running `roomA-forward` with `warmup=60` instead of 6
  produced a clean trace. That control is also what showed coverage falling from
  34.3% to 6.5% once the window starts further from spawn.
- **Motion is per-update, not wall-clock scaled.** The baseline and PROGJOIN
  ROMs have different frame times (324,804.8T vs 336,005.3T mean) yet report
  identical `x_q4`, `y_q4` and `yaw` at every update index. Movement is therefore
  driven per logical update, which is what makes a name-table A/B between two
  ROMs of different speeds meaningful at all; had it been time-scaled the slower
  ROM would drift out of position and the hashes would diverge for reasons
  unrelated to the renderer.

`tools/progjoin_corpus_coverage.py` makes the above reproducible: it reports an
oracle's step and `(step, want)` vocabulary, tests whether a live trace's
positions appear in the oracle's sampled grid, and projects records against the
uint16 cap. With the matching `--entries-per-key` it predicts 18,107 B for the
shipped corpus (actual 18,976) and 258,408 B for the dense one (actual 262,315),
so the cheap check can be trusted before committing to a census run.
