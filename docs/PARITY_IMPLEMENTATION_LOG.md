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

## Response to the stocktake and its amendment: measuring the shared assumption

Both documents propose replacing the pose-derived dispatch key with a canonical,
pose-independent one. On the two points where they disagree, the amendment is
right and is followed here: viewport clipping must **not** enter the key (the
guard-band result already handles it at zero cycles), and arbitrary poses are an
oracle and adversarial test set, never the program dictionary. The stocktake's
"bake 80%, test 20%" framing still sources the vocabulary from poses.

But both rest on one assumption that neither measures: that the compiled
**bodies** already generalize and only the **keying** is pose-trained. That
decides whether canonical keying is worth building at all, so it was measured
first, with `tools/progjoin_body_generalization.py`. Poses are split disjointly,
both halves are baked through the same windowed baker the census uses, and every
FULL chunk is resolved to its byte-exact serialized body. Two coverages result:

| split | train poses | train bodies | key coverage | body coverage | gap |
| --- | --- | --- | --- | --- | --- |
| `yaw-shipped` (1/16 headings, as ships) | 1,248 | 3,343 | 15.8% | 44.8% | **+29.0** |
| `yaw-parity` (1/2 headings) | 9,984 | 12,102 | 57.2% | 90.0% | **+32.8** |

`key coverage` is what the shipped dispatcher can reach; it lands squarely in the
2.1-34.2% band the live A/B measured, which cross-validates the harness against
the ROM. `body coverage` is the ceiling a perfect pose-independent key could
reach **without adding one byte of ROM**.

**The assumption holds.** Bodies generalize roughly three times further than
their keys, and the gap is a stable +29 to +33 points across sampling densities.
That is the size of the prize, and it is now a measured quantity rather than a
hope. Canonical keying is the right investment.

It is also, on its own, not enough. 44.8% at shipped density is far from the
>=95% acceptance criterion, and the body vocabulary keeps growing with sampling
(`bodies ~ poses^0.62` across the splits, `^0.77` across the two censuses),
projecting to 132,000-158,000 bodies over the full pose space — roughly 5 MB at
the observed 36.6 B/body, against a 1 MiB cartridge.

### Where the vocabulary explosion actually lives

Decomposing the 12,102 bodies of the `yaw-parity` bake:

```
unique full blobs      12,102     <- what the census counts
unique payloads        12,102     <- tuned format, 7-byte prefix removed: no change
unique DELTA sequences    350     <- the raster geometry alone
unique WORD  sequences 11,892     <- the tile appearance alone
```

The raster-geometry vocabulary is **350**. The explosion is entirely in the tile
words. (Stripping the per-`want` prefix header changes nothing, so the tuned
body format is not the lever here — that was tested, not assumed.)

This sharpens the amendment's anchor considerably. Its "generic rasterized edge
program" exists and is small — 350 shapes — but the current body welds geometry
to appearance in one immutable blob, and appearance is what scales with pose
sampling, because shade varies continuously with depth. Every new depth mints a
"new body" for a raster shape already in ROM.

The appearance dimension is not arbitrary either: 354 distinct tile-word values
and 74 distinct intra-body word deltas, dominated by +-64, 0, +-8, +-16, +-1.
It is not, however, derivable from the movement class alone — conditioned on the
commonest motion the word delta still takes 54 values with the mode covering only
11.4% — so the item-23 trick that eliminated destination metadata does not
transfer directly to words. Appearance needs its own compact representation.

**Recommended revision to the amendment's section 12.** The one-cell
falsification experiment is the right next step, but it should canonicalize onto
the **350-shape geometry vocabulary**, and carry shade as a separate parameter
resolved at playback, rather than onto the body vocabulary. Targeting bodies
inherits the appearance explosion and will reproduce the ROM wall at a different
scale.

### A latent correctness bug the wider pose space exposed

The column-advancing motions form the arithmetic family `2 - 40k`: next column,
up *k* rows. The shipped corpus contains k=0..3 exactly:

```
step_bytes present in SHIPPED corpus: {-118: 1344, -78: 2321, -38: 4069, 2: 53997, 40: 12452}
```

and `gate_advance` in `src/tilesector_polar_progjoin_runtime.c` handled exactly
those five. The wider pose space also produces **k=4 (`step_bytes = -158`)**, 21
times in 248,131 transitions sampled. On that value `gate_advance` returned 0,
which propagates out of `tsp_progjoin_play_plan_gated` as a refusal **after cells
have already been written** — the partial-write hazard recorded in the previous
entry, fired for real.

It is zero today only because the shipped corpus is too narrow to contain the
motion. Every recommendation in both documents widens that corpus, so this would
have fired precisely when coverage started working, and presented as a rendering
corruption rather than a fallback.

`gate_advance` now handles k=4, written to extend the same arithmetic pattern.
The three ROMs were rebuilt and the full A/B re-run: every hash, cycle count and
counter is **bit-identical** to before the change, confirming it cannot affect
the shipped corpus while removing the cliff. The `fb_play_*` counters continue to
guard it, and the A/B fails hard if either becomes non-zero.

### Revised order of work

1. **Fix `gate_advance`.** Done above; prerequisite for any corpus widening.
2. **Run the amendment's one-cell falsification experiment**, canonicalized onto
   the 350-shape geometry vocabulary with shade as a separate playback parameter.
   Reject any representation whose dictionary grows with sampled poses.
3. **Give appearance its own compact representation.** 354 word values and 74
   word deltas is a small alphabet; the question is whether shade can be carried
   as a per-run parameter rather than per-cell payload. This is where the ROM
   scaling is actually decided.
4. **Restore the guard-band destination model** (amendment section 9) in place of
   the gated player's per-write bounds checks, keeping ownership semantics intact.
5. Only then the run-edge invariant hoists (items 24-26) and direct-rank dispatch
   (item 27), which remain premature while the compiled path succeeds 2-34% of
   the time.

## The section-12 harness, retargeted: the raster shape is computed, not sampled

`tools/progjoin_shape_canon.py` implements the amendment's falsification
experiment against the 350-shape geometry vocabulary rather than the body
vocabulary. The result is stronger than the amendment supposed, and changes what
the architecture should be.

**The raster shape does not need a dictionary at all.** It is computable in
closed form from the run-edge's own parameters, using the renderer's existing
projection arithmetic and nothing else:

```
h(c)    = bits 7..13 of (iq + c*step + 32)
y(c)    = 71 - h(c)   (family 0, top edge)
        = 72 + h(c)   (family 2, bottom edge)
span(c) = tile rows between y(c) and y(c+1)
shape   = per column: one "down" move per extra tile row, then one "advance"
          move carrying the row jump into column c+1
```

Every cell carries its outgoing move, including the last, which is what lets a
chunk's cursor continue into the next; the model evaluates one column past the
chunk to emit that exit move. That detail was found by measurement, not
assumed — the first model was short by exactly one trailing delta.

### Falsification result

The model was developed against the `yaw-parity` **train** split and then run
against two splits it had never seen, each resolved through the authoritative
baker:

| held-out split | chunks | exact | corpus defect | geometry-model failures |
| --- | --- | --- | --- | --- |
| `yaw-shipped` test | 100,795 | 100,794 | 1 | **0** |
| `yaw-parity` test | 53,729 | 53,728 | 1 | **0** |
| total | **154,524** | **154,522** | 2 | **0** |

Vocabulary construction never consumed the validation poses — it never consumed
poses at all — which satisfies the amendment's acceptance criterion directly
rather than by sampling argument.

The two disagreements are both the same corpus defect, and the harness isolates
them rather than absorbing them. Each is a chunk whose body spans **one** column
although its descriptor was fetched for `want=6`:

```
step=-500 fam=2 want=6 iq=13320 -> body spans 1 column, truth=(39, 1)
step= 324 fam=2 want=6 iq= 3840 -> body spans 1 column, truth=(39, -39)
```

There is exactly one advance move per column, so a body whose advance count
differs from its `want` is internally inconsistent: the descriptor and the body
it points at disagree. 53,728 of 53,729 bodies in the parity split match their
own `want`. This is worth a look at `edge_progjoin_bake.c` and is **not** a
failure of the closed form; it is recorded here rather than diagnosed, because
it was found in a denser bake than the one that ships and its reachability in
the shipped corpus has not been established.

### What this removes

`progjoin_shape_canon.py cost`. The FULL-geometry dispatch tables exist only to
answer a question the closed form answers arithmetically:

```
step_page_base.bin              32 B
stepmap (gg_pj_step_local)   4,096 B
thresholds.bin               4,392 B
descriptor.bin              13,176 B
records.bin                 18,976 B
TOTAL                       40,672 B
```

All of it goes, and with it the uint16 record-offset cap that bounds the corpus
at ~15,240 semantic entries — there is no record blob left to index. That cap
was the hard wall identified two entries ago; it does not need to be widened,
it needs to stop existing.

The cycle case is **not** yet an argument and is not claimed as one. A per-chunk
instruction-count model puts closed-form derivation at roughly 700-800 T against
the Z80 audit's measured 697-947 T/chunk for dispatch — comparable, not clearly
better, and it must be measured on real hardware before anyone quotes it. The
structural case stands on its own: geometry stops depending on which poses were
sampled, which is the entire point.

### Bound found by enumerating the parameter box

`enumerate` scans the `(step, fam, want, iq)` box directly — 3,145,728 points at
step stride 1, iq stride 256 — with no poses involved. It yields 1,510 distinct
shapes (more than any single bake sees, because the box includes parameters this
map never produces) and every move in that vocabulary is executable by the
now-extended `gate_advance`.

It also finds a real bound: some parameter points require a column advance of
**14 or 15 tile rows**, which no compiled-body move can express at all. Those
are outside the `2 - 40k` family for any small k. They must either be proven
unreachable in gameplay or handled by a fallback; the architecture cannot
represent them. This is the kind of limit that only appears when the vocabulary
is enumerated from parameters instead of collected from poses.

### Consequences for the plan

1. Geometry is solved and needs no ROM. The remaining ROM question is entirely
   **appearance** — 354 tile-word values and 74 word deltas, not derivable from
   the movement class, still the open problem.
2. The guard-band work (amendment section 9) is now on the critical path rather
   than adjacent to it: the closed form describes unclipped geometry, so the
   destination model has to absorb visibility, exactly as the amendment argued.
3. Items 24-27 remain premature, and the dispatcher they optimise is the one the
   closed form deletes.

## Correction: the non-geometry dimension is texture addressing, not shade

Two entries above this one attributed the body-vocabulary explosion to
appearance, stating that "shade varies continuously with depth" and that "every
new depth mints a new body for a raster shape already in ROM", and recommending
that "appearance" be given its own compact representation. That reading was
wrong and the measurement that disproves it is simple.

The corpus case line carries its own `shade` field. Across all 53,747 chunks of
the `yaw-parity` bake it is **constant at 1** — appearance mode 0 bakes exactly
one shade, which is what the live-rung build comment always said. Shade cannot be
the explosion because shade never varies.

Decomposing the name-table words properly (tile index in bits 0..8, then hflip,
vflip, palette, priority):

```
distinct tile indices                            93
distinct flag combos (hflip,vflip,pal,pri)        4   (0,0,0,0) (0,1,1,0) (1,0,0,0) (1,1,1,0)
case 'shade' field                          {1: 53747}
bodies where the TILE INDEX varies within the body   47,385  (88.2%)
bodies where the FLAGS vary within the body           4,980  ( 9.3%)
```

So the varying dimension is **wall texture addressing**: which slice of the wall
graphic lands in each cell, plus a horizontal flip and a coupled
`(vflip, palette)` pair. It is not fog, not shadow, and not per-wall material
shade. The correct description of a compiled body is therefore *geometry welded
to texture addressing*, both of which are geometric quantities, rather than
geometry welded to appearance.

This matters for what comes next, because it makes the remaining dimension the
same kind of problem the shape turned out to be rather than an artistic one.

It is not, however, derivable from the cursor position the way the destination
delta was. Testing the tile index against every positional key available to the
player leaves it heavily ambiguous:

```
tile ~ (fam, relative row)              37 buckets,  33 ambiguous
tile ~ (fam, relative column)           12 buckets,  12 ambiguous
tile ~ (fam, relative row, column)     129 buckets, 116 ambiguous
tile ~ (fam, absolute screen row)       34 buckets,  34 ambiguous
```

That is the expected result: texture addressing depends on where the column
samples the wall *surface*, which is a wall-space coordinate, not a screen-space
one. The shape was derivable from screen-space terms the renderer already
computes; the tile index will need the run's wall-space `u` (and the texture
row for `v`), which the renderer computes elsewhere. The lead is real and is the
same shape of problem, one coordinate system further in.

Deferred deliberately, not abandoned. Nothing above depends on it: geometry is
solved and costs no ROM, and the texture dimension is what still does.

## Shape gate in CI, and what it predicts for coverage

The closed form is analytic and has no training set, so every pose set is held
out from it. That makes the gate cheap enough to run unconditionally: against the
**shipped** corpus oracle it completes in 0.44 s.

```
held-out chunks             14,379
  exact reconstruction      14,379 (100.000%)
  differs, wholly off-table 0
  corpus body/want mismatch 0
  differs, ON the table     0
  outside the move family   0
SHAPE_CANON_EXACT
```

It now runs in `gg-progjoin-live-rung.yml` immediately after the corpus is built,
before anything expensive, and fails the rung if the closed form ever stops
reproducing the authoritative baker.

Two things this settles.

**The corpus body/want defect is not reachable in what ships.** The previous
entry recorded two chunks whose body spanned one column although the descriptor
was fetched for `want=6`, and deliberately did not claim reachability. The
shipped corpus has **zero** of them. The defect appears only in the denser bakes
used for the generalization splits, so it is a latent baker concern rather than a
live one, and the gate will catch it if a denser corpus is ever adopted.

**Predicted coverage under the closed form.** The live A/B measured the compiled
path serving 2.1% of run-edges while turning and 34.2% moving forward, with the
misses attributed as `miss_step`, `miss_desc` and `miss_rank`. All three are
lookups into tables the closed form does not have, so none of them can occur:
there is no step map to miss, no descriptor to be absent, and no record list to
walk off. The only refusal left in the closed form is an advance move outside the
`2 - 40k` family, and across 168,903 chunks tested (14,379 shipped + 100,795 +
53,729 held out) that occurred **zero** times.

So the predicted compiled coverage is 100% of FULL run-edges on every pose set
measured so far, against 2.1-34.2% today. That is a prediction from the host
harness, not a ROM measurement, and it stays a prediction until the closed form
actually drives playback on hardware. The 14-15 row advance found by enumerating
the parameter box remains the one known unrepresentable case, and has not been
shown reachable in gameplay.

## Where the compiled path actually spends its cycles

Performance is the deciding metric, and until now the only performance fact on
record was that the PROGJOIN ROM is slower than baseline. That figure is
confounded: at 2-34% coverage most attempts pay dispatch and then pay the legacy
edge path anyway, so it never said what the compiled path costs *per run-edge*.

The Z80 cannot read its own clock, so the profile-phase byte is used as a marker
the emulator samples per instruction: phase 6 for selector/preflight, phase 7 for
playback, phase 8 for per-chunk setup, restoring phase 2 on every exit. This
decomposes the compiled path the same way the Z80 audit does. `polar_ab_profile`
accumulates per phase and emits `pj_dispatch_T`, `pj_play_T`, `pj_chunk_T` per
logical update, alongside a new `g_pj_stat_cells` counter so playback resolves to
T per cell.

First decomposition, `roomA-forward`, 367 attempts / 126 successes / 1,228 cells:

```
dispatch/preflight   3,179,325 T  (41.5%)  =  8,663 T per attempt
per-chunk setup        219,977 T  ( 2.9%)
cell loop            4,267,103 T  (55.7%)  =  3,475 T per cell
TOTAL                7,666,405 T           = 60,844 T per successful run-edge
```

3,475 T to write one 16-bit word is roughly 300 Z80 instructions for work that
needs about 20. Per-chunk setup, which had been the obvious suspect given short
runs, is 2.9% — that hypothesis was wrong and the measurement said so.

### The cause, and the fix

Generating the runtime's assembly found it immediately: `gate_advance` compiled
to **250 instructions**. It took six stack-passed arguments, five of them
pointers, and SDCC reloads each pointer from the stack frame on every
dereference. At 10-14 T per instruction that is ~2,500-3,500 T, which is
essentially the entire measured per-cell cost.

There was exactly one call site, so the advance was inlined into the cell loop,
keeping the cursor state in the caller's locals. `cov` is still computed from the
old `rowbit` before `rowbit` updates, exactly as the helper did.

Two smaller wastes were removed first, and are worth recording because they were
*not* the problem despite looking like it:

- `wi = row*20u + col` emitted a 16-bit multiply per cell, yet `cursor` already
  carries `row*40 + col*2` as an invariant every advance preserves, so the index
  is `cursor>>1`.
- `1u << rowbit` is a variable shift, which SDCC compiles to a loop; replaced
  with an 8-entry constant table.

Together those two gained 5.6%. The inlining gained the rest.

```
                        before        after    change
T/cell                   3,475        1,639    -52.8%
T/run-edge              60,844       41,975    -31.0%
T/attempt (dispatch)     8,663        8,688     +0.3%

whole update, roomA-forward:  +13.82%  ->  +8.73%   vs baseline
whole update, roomA-turn:      +3.45%  ->  +3.45%   (no successes; playback never runs)
```

All of it exactness-preserving: the full A/B still reports `LIVE_AB_EXACT` on
player state and the complete 20x18 name-table hash across both scenarios, and
`progjoin-stats` still agrees with `progjoin`.

### What this says about the architecture

The compiled path is still a net regression, and it is important not to dress
that up. But the shape of the cost has changed and now points somewhere useful:

**Dispatch is now the dominant term at 60.3%**, 8,688 T per attempt, paid by
every attempt whether or not it succeeds. That is precisely what the closed-form
shape derivation deletes — no step map to walk, no thresholds, no descriptor, no
record list. The structural result and the performance result now point at the
same work.

Two cautions on the remaining gap. First, `roomA-turn` is unchanged because it
has zero successes, so its +3.45% is pure wasted dispatch; that scenario is
entirely a coverage problem, not a playback one. Second, 1,639 T/cell is still
far above what hand-written playback should cost, so the cell loop has more to
give even after inlining — the remaining C-level overhead (banked ROM reads
through a pointer, per-cell bounds checks that the guard band would remove) has
not been separately attributed yet.

## Dispatch: one small win, one measured-and-rejected hypothesis

With playback halved, dispatch became the dominant term at 60.3% and 8,688 T per
attempt — and unlike playback it is paid on *every* attempt (367) rather than only
the successes (126), so reducing it helps regardless of coverage.

Two changes were tried against it.

**Inlining the 2-byte reads worked, slightly.** `rd16p` was an out-of-line call
for what is four instructions, used for the step-page base and the descriptor
entry. Inlined at both sites: 8,688 -> 8,625 T per attempt, -0.7%. Small, free,
kept.

**Hoisting the bank switch out of the record walk did not work, and the
measurement is worth keeping.** `record_byte` switches the ROM bank on every byte
it reads, up to four per matched record, so replacing the walk with a single
switch plus pointer indexing looked like an obvious win. It measured **3.6%
slower**: 8,688 -> 9,005 T per attempt.

The reason is that the walks are short. Most lookups match within the first few
records, so the per-iteration bank check and pointer setup the new form needs
costs more than the switches it removes. The change was reverted and the result
recorded in a comment at the site, so it is not re-attempted on the same
reasoning.

That is the second hypothesis in this area that measurement has rejected: per-chunk
setup looked like the playback cost and was 2.9%, and the bank switch looked like
the dispatch cost and is not. The one that did land — `gate_advance` being 250
instructions — was found by reading generated assembly rather than by reasoning
about the C.

Current state of the compiled path, `roomA-forward`:

```
dispatch    8,625 T per attempt   (paid by all 367 attempts)
cells       1,640 T per cell
total      41,807 T per successful run-edge
whole update  +8.73% vs baseline (was +13.82% before this round's work)
turn          +3.45% vs baseline (unchanged; zero successes, pure wasted dispatch)
```

Exactness holds throughout: `LIVE_AB_EXACT` on player state and the full 20x18
name-table hash, both scenarios, with `progjoin-stats` still agreeing with
`progjoin`.

### A process note worth recording

An intermediate build in this round reported the compiled path as never
attempted, with counters at zero and cycles differing from baseline. The cause
was mine: the ROMs were rebuilt after `git checkout` had restored the canonical
sources, without re-running `build_parity_materializer.py` and
`apply_progjoin_full_live_rung.py`, so the splice was absent and the materializer
was the canonical one. The A/B gate's "the compiled path was never attempted"
check caught it immediately. That check was added to stop a fallback-only run
reading as parity, and it also catches a build that silently lost its splice.

## Correction: the closed form is the back half, not the whole chain

`tools/progjoin_shape_canon.py` was titled "arbitrary pose -> canonical generic
raster-line state" and the entries above described it that way. It does not do
that. The chain is:

```
pose -> [ MISSING: baked vertex baselines + global LUTs ] -> step, iq
step, iq -> [ what was actually built ] -> canonical raster line -> program
```

`step` and `iq` are **host-supplied** in that harness: read from the baker's case
file, and on hardware produced by the existing runtime geometry pipeline, which
is exactly the stage the amendment says to replace. The 168,903 chunks are held
out in the *pose* dimension but are not an end-to-end test of arbitrary pose in,
program out. Any future measurement that takes `step`, `iq`, a projected first
row or similar from the host must say so at the point of measurement.

What survives, and is still worth having: given the run-edge parameters, the
raster shape needs no sampled-pose dictionary. It is computed, and exercised
across the whole `(step, iq)` parameter box rather than over combinations a
corpus happened to contain.

`step` and `iq` do **not** need to disappear. They may be perfectly good internal
canonical variables. What had to go, and has gone, is the assumption that their
valid combinations are learned from sampled poses.

### Vocabulary of retired language

Two phrases from earlier entries are withdrawn because they carry the sampled-
dictionary model:

- "the turn case is a coverage problem" — it is not. A 0% turn hit rate is
  evidence that program selection is architecturally wrong, not a deficit to be
  filled by baking more poses. The target system has no training coverage of
  camera poses; a turn must produce a valid canonical line state by construction.
- "dispatch optimisation helps regardless of coverage" — making failed dictionary
  lookups cheaper helps the broken implementation. Replacing the dictionary with a
  complete canonicalizer means there is no coverage statistic to speak of. Those
  are different achievements and were conflated.

`progjoin_live_ab.py` still reports compiled coverage. It is retained as a
regression detector for the current ROM and is explicitly **not** a target.

## Protocol for the arbitrary-pose rungs

Agreed before building, so results cannot be read as stronger than they are.

**Host-supplied labelling.** Every quantity the Game Gear would have to derive is
charged to the Z80 side. Anything the host hands the model is labelled at the
point of measurement, and while any such quantity remains, the experiment is not
end-to-end.

**Two validations, not one.** A representation certified exhaustively over all
local sub-positions and headings needs no statistical held-out test — the
certificate is the stronger guarantee. It still gets an independent held-out
implementation test afterwards, because the implementation can be wrong even when
the representation is right. A representation whose parameters are *selected* from
data needs the held-out split as well.

**Layered oracle.** The comparison is not one pass/fail. In increasing strength:

1. projected endpoint X, and Y with sub-pixel phase
2. canonical line-state ID
3. exact sequence of edge masks and cursor moves
4. covered screen cells
5. ownership result

A difference in the final name-table word caused by material/texture not being
integrated yet must not be confused with a geometry failure, and this ordering
makes that impossible to confuse.

**Vocabulary is measured after canonicalization, not before.** Counting the
diversity of raw parameters measures the wrong thing; the point of
canonicalization is that many poses collapse onto few states.

**Texture does not gate geometry.** The generic line program should be a
geometric primitive — edge mask, cursor move, edge mask — with material, shade,
texture phase and receiver semantics resolved separately and combined later, as
the texture work already does. Rung one runs on flat walls or canonical edge-mask
IDs.

**Rung two is mandatory and immediate.** Cross-cell transitions follow rung one
directly, before the single-cell representation is polished, so an anchor/state
choice that makes continuity across boundaries ugly is discovered early rather
than late.

**No further hand-optimisation of the C player for now.** 3,475 -> 1,640 T/cell
was salvage. 1,640 remains far above what the eventual primitive should cost,
which tells us it belongs in tight assembly with state in registers once the
architecture is proven, not that the remaining C inefficiencies are worth finding
first.

## The corpus is keyed on a derivation the mode-0 ROM does not use

Looking for the missing arbitrary-pose front end turned up something else, and it
is the real mechanical reason the compiled path hits so rarely. It also corrects
the root cause recorded earlier.

The renderer has **two** derivations of the dispatch key, chosen by
`r->depth_plane`:

```c
if (g_tspf_appearance_mode < 2u && r->depth_plane) {   /* screen_depth_plane */
    iq = r->iq;  step = r->step;
} else {                                                /* the other one */
    iq   = (int16_t)r->inv0 << 6;
    step = shr_signed(((int16_t)r->inv1 - (int16_t)r->inv0) * k_col_recip_q8[n], 2);
}
```

`screen_depth_plane` is guarded `#if defined(__SDCC) && TSPF_SCREEN_DEPTH_PLANE`
at all eight of its sites, so **it does not exist in a host build**. The pose
oracle, and therefore the entire compiled corpus, is produced by a host build and
can only ever take the second branch. The shipped mode-0 ROM takes the first.

`tools/rung1/depth_derivation_check.c` transcribes `screen_depth_plane` onto the
host, using the generated `(class, yaw)` coefficient tables and the loader's
exact indexing (`nf[c] = k_depth_nf_q7[c][yaw]`, verified against the generated
loader), and compares the two derivations over arbitrary poses — every walkable
cell, six sub-cell offsets, all 256 headings:

```
FULL runs examined            1,206,906
  depth-plane path applies    1,191,010 (98.7%)
  of those, step matches host    19,338 (1.62%)
  of those, iq   matches host     7,347 (0.62%)
  of those, BOTH match              660 (0.06%)
```

So in mode 0 the ROM uses the depth-plane derivation for 98.7% of FULL runs, and
the key it computes agrees with the key the corpus was baked under in **0.06%**
of cases.

### What this corrects

An earlier entry attributed the 2.1%/34.2% hit rate primarily to pose
undersampling — the oracle sampling 0.52% of poses and no live position landing
on its grid. That finding stands on its own terms and the grid arithmetic is
unchanged, but it is **not the primary cause**. The primary cause is that the
baker and the ROM compute the dispatch key by different formulas. The measured
hit rates are largely the coincidence rate between two unrelated derivations,
which is consistent with the corpus holding 586 of 4,096 possible step values.

### The trap this creates

There is an obvious "fix": rebake the corpus from the depth-plane derivation. The
hit rate would jump, possibly dramatically, and **that would be the worst
available outcome**. It would make a sampled-pose dictionary look like it works,
which is exactly the failure mode this whole line of work exists to avoid. It is
recorded here as a thing not to do rather than an opportunity.

### The good news for the architecture

`screen_depth_plane` is itself close to what the target front end should be. It
derives `iq` and `step` from inverse depth, the wall's normal class, and yaw,
through global LUTs loaded once per update:

```c
iq   = (invd * k_depth_nf_q7[class][yaw])     >> 1;   /* then marched to c0 */
step = (invd * k_depth_stepfac_q4[class][yaw]) >> 4;
```

No sampled poses anywhere. Combined with the certified local bearing bake
(46,109 B, 7 fallback corners, <1 px) and the existing global `angle_x` and
reciprocal LUTs, most of the pose-independent front end the amendment asks for is
already present in the renderer. What is missing is not the derivation but its
**consistency** (the oracle does not use the ROM's path) and its **persistence**
(`g_corner_bearing_valid` is cleared every update, so nothing is retained across
frames).

## Rung 1: arbitrary pose -> ROM projection -> iq/step -> canonical raster state

The candidate architecture is the one the ROM already has, not a new one. No
sampled dictionary is consulted anywhere in this experiment.

```
baked local corner field          -> corner bearing          project_key
- yaw, global angle_x LUT         -> screen endpoints x0,x1  project_key
wall normal distance              -> inverse depth           wall_d_q4 / inv_for_dq4
(invd, normal class, yaw)         -> iq, step                screen_depth_plane
iq, step, family, length          -> canonical raster shape  closed form
```

`tools/rung1/pose_to_raster_check.c`, over every walkable cell x 6 sub-cell
offsets x all 256 headings: 1,206,906 FULL runs, 3,875,340 chunk-family
instances. The depth-plane path applies to 98.7% of FULL runs.

### Disagreement by layer

Exact equality of `(iq, step)` is the right criterion for explaining dictionary
misses and the wrong one for geometric correctness, because two parameterizations
can quantize to the same columns. So each layer is reported separately, and each
is evaluated independently of the others.

```
L1 endpoint   c0,c1              shared by construction (both from angle_x)
L2 depth      per-column h(c)    3,561,290  (91.896%)
L3 trajectory cursor moves       1,905,008  (49.157%)
L4 ownership  covered cells        780,298  (20.135%)
moves outside the expressible family:  host path 63,109 (1.6%), ROM path 146,198 (3.8%)
```

An earlier version of this harness evaluated L4 only where L3 agreed, which
defeats the point of layering — two different trajectories covering the same
cells is exactly what L4 exists to detect. Fixed; the 20.135% above is measured
independently.

**Quantization absorbs most of the parameter difference but not all of it.**
91.9% -> 49.2% -> 20.1% down the layers. So `screen_depth_plane` is **not** merely
a different parameterization of the same geometry: it produces a different
covered-cell set in roughly a fifth of chunk-family instances.

### What that divergence is, and what it is not

This is a real disagreement between the shipped mode-0 ROM and the host reference
renderer, and it has not been adjudicated. Two readings, and the record cannot
currently distinguish them:

- The host path interpolates inverse depth between the two *actual endpoint*
  inverse depths (`inv0`, `inv1`). The depth plane fits a plane from
  `(invd, normal class, yaw)`. A priori the former is the more faithful and the
  latter is a deliberate cheaper approximation, which its name suggests.
- Or the divergence is unintended and nobody has compared the two, because the
  A/B gate compares ROM against ROM (both use the depth plane) and the
  image-equivalence check compares host against host.

Settling it needs a ground-truth render, not more inference, and it should be
settled before either path is treated as the oracle for exactness claims.
Recorded as open.

### Vocabulary collapse, measured after canonicalization

The figure that tests the finite-program hypothesis is not how many distinct
`step` values exist, but how much canonicalization compresses:

```
chunk-family instances            3,875,340
distinct (iq, step, family, len)    726,652
distinct raster trajectories          1,140
collapse                            3,399 : 1
```

**1,140 canonical raster trajectories cover the entire arbitrary-pose domain.**
That is the finite-program hypothesis holding at the geometry layer, derived
without a dictionary and without sampling poses into a vocabulary.

(An earlier run reported 200,000 distinct states; that was a linear-scan table
saturating at its cap and reporting the cap as a count. Replaced with an
open-addressed set. The trajectory figure was always well under the cap and is
unaffected.)

### Where this leaves the architecture

The two pose-independent halves join, and the join produces a small finite
vocabulary. What sits between them in the shipped ROM — the sample-trained
dictionary — is unnecessary for geometry: 1,140 trajectories are generated, not
looked up. The remaining open items are the depth-plane divergence above, the
3.8% of ROM-path chunks whose moves fall outside the expressible family, and
persistence across updates.

## Rung 1.5: two corrections, then the vocabulary actually saturates

### Correction 1: "the entire arbitrary-pose domain" was wrong

The previous entry swept six sub-cell offsets. A Q4 cell has 64x64 = 4,096 local
translations, so that was 0.15% of the translational space, not the domain. The
correct description of that run is a broad corpus spanning all cells and headings.

### Correction 2: two claims in that entry contradicted each other

"1,140 trajectories cover the entire domain" and "3.8% require moves outside the
expressible family" cannot both be true. The 1,140 figure counted only chunks the
shape model could express; the 3.8% were silently excluded. 1,140 covered ~96.2%.

### The 3.8% is exactly two moves

Histogrammed by required row jump over 3,875,340 chunk-family instances:

```
jump   count     share    family split          if added
-15    103,159   70.6%    top 51,758 bot 51,401  1.111% remain
-14     43,039   29.4%    top 24,793 bot 18,246  0.000% remain
```

Two values, nothing in between, balanced across families. The compiled-body move
family goes from five `{0,-1,-2,-3,-4}` to **seven** `{0,-1,-2,-3,-4,-14,-15}` and
representability reaches 100%. The gap at -5..-13 is informative: 14-15 rows on an
18-row table means these are extreme near-field edges crossing nearly the whole
screen between adjacent columns, a regime with no intermediate cases.

### Exhaustive 64x64 translational sweep, with the growth curve

`tools/rung1/pose_to_raster_check.c <yaw_stride> <cell_stride>` sweeps the full
64x64 local translation space of every Nth walkable cell, at all 256 headings.
Every 64th cell, eight cells, all headings:

```
FULL runs                        14,805,535
chunk-family instances           47,828,940
inexpressible                             0  (0.000%)
distinct raster trajectories          1,643
collapse                           29,111 : 1
```

The growth curve, which is the figure that decides whether the vocabulary is
finite:

```
after cell   1   trajectories 1,028   instances  9.5M
after cell  65   trajectories 1,320   instances 19.1M
after cell 129   trajectories 1,612   instances 28.4M
after cell 193   trajectories 1,612   instances 37.7M
after cell 257   trajectories 1,612   instances 39.5M
after cell 321   trajectories 1,643   instances 44.3M
after cell 385   trajectories 1,643   instances 46.1M
after cell 449   trajectories 1,643   instances 47.8M
```

**It saturates.** The final 3.5M instances added no new trajectories. One cell
swept exhaustively already yields 1,028 of the eventual 1,643, so most of the
vocabulary is intrinsic to local geometry rather than to which cell the player
occupies. The broad six-offset corpus over all cells gave 1,763 on a different
cell sample; both land at the same ~1,600-1,800 scale.

### Deduplicated program bytes

```
unique trajectories               1,643
  used by both families             909   (55%, already identical sequences)
  top family only                   387
  bottom family only                347
total moves                      30,486
mean / max length              18.6 / 30 moves
bytes at 1 byte per move         30,486
bytes at 3 bits per move         11,433   (seven moves fit in three bits)
distinct under reversal     1,643 of 1,643   (no reversal equivalence)
```

Top and bottom already share 55% of trajectories as literally identical move
sequences, so no mirroring transform is needed to capture that. Reversal buys
nothing, which is expected: the move alphabet is asymmetric, one "down" against
six "up" jumps.

For scale, the shipped sparse dispatch carries 40,672 B of tables plus 73,094 B of
bodies, about 114 KB, for 2.1-34.2% compiled coverage. The generic trajectory
vocabulary is **11,433 B for 100%**, with no dictionary and nothing learned from
sampled poses.

### Still open

The depth-plane adjudication. L4 ownership divergence is 22.6% on this exhaustive
sweep (20.1% on the earlier corpus), and it remains unadjudicated. The previous
entry's speculation that the host path is "a priori more faithful" is **withdrawn**:
for a planar wall under pinhole perspective, inverse depth is linear in screen X,
which is exactly the `iq + c*step` form the depth plane uses, whereas the host path
quantizes `inv0` and `inv1` to uint8 and derives an increment between already
quantized values. Neither should be assumed correct. Both must be measured against
a high-precision perspective reference, by error magnitude rather than incidence.

## Correction: the -14/-15 moves were a wraparound artifact, not geometry

The previous entry canonised a seven-move alphabet on the strength of two extra
jumps, and explained them as extreme near-field edges. **That explanation was
wrong**, and inspecting the cases rather than believing the story found it.

Two signals gave it away. First the move histogram was non-monotonic: `up 4`
occurred 395,330 times in 69 trajectories while `up 15` occurred 2,244,321 times
in 449. Genuine near-field geometry falls off with steepness; it does not dip at 4
and spike at 15. Second, the captured cases showed the edge moving *down* by one
or two rows while the model computed a jump of -14 or -15:

```
jump -14: y(c)=59 -> y(c+1)=71   dy=+12px   rows 7 -> 8
jump -15: y(c)=52 -> y(c+1)=64   dy=+12px   rows 6 -> 8
```

The mechanism is the mask in the renderer's own depth term,
`h = (((iq + c*step + 32) >> 6) & 0xFF) >> 1`. Evaluating a column where the
pre-mask value leaves `[0,255]` wraps it, and the wrap appears as a ~127px leap,
which is 15.875 tile rows:

```
jump=-15 col=3  raw>>6 at c,c+1,c+2 = [38, 14,  -9]   valid range 0..255
jump=-14 col=3  raw>>6 at c,c+1,c+2 = [25,  0, -25]
```

`|dy|` across captured cases maxes at exactly 127, the full range of `h`.

### What the corrected model shows

The model now refuses to evaluate outside the domain, and the final chunk of a
run emits a terminator instead of extrapolating into a column that lies past the
run. With that:

```
out-of-family jumps                    0     the FIVE-move alphabet is complete
distinct raster trajectories         432     (was 1,643 with the artifacts)
shared by both families              427     of 432, 98.8%
mean / max length              9.8 / 16     moves
move-stream payload                1,586 B  at 3 bits per move
collapse                       110,715 : 1
```

`-599` now records **zero** occurrences, and the alphabet falls off monotonically
— `up 1` 29.9M, `up 2` 7.6M, `up 3` 4.8M, `up 4` 249K — which is what real
geometry looks like.

So the vocabulary is **smaller** than claimed, not larger: 432 trajectories and
about 1.6 KB of move payload, over a five-move alphabet plus a terminator.

### A real finding hiding underneath the artifact

Respecting the domain exposes something the artifact was masking: **10.46% of
ROM-path chunks have the depth term leave the uint8 range within the run itself**,
against 1.28% on the host path. That is a genuine property of `screen_depth_plane`
marching `iq` by `+-step` until it runs out of byte, and it is not explained by the
trailing-column issue (which accounted for only 12.37% -> 10.46%). It needs
handling in any executor built on this path, and it is further reason not to
assume either derivation is correct before the adjudication.

### Cell coverage, stated unambiguously

The exhaustive sweep processed the full 64x64 local translation space and all 256
headings of **every 64th walkable cell — eight cells**, not all walkable cells.
The checkpoint numbers 1, 65, 129 ... are cell indices, not progress markers. The
translational domain is complete *within* each tested cell; the cell sample is
1.7% of the map. A denser sweep over every 4th cell is running.

### Accounting note

The byte figure is **move-stream payload**, not total generic-program ROM. An
executor also needs per-trajectory start and length; at two bytes of offset plus
one of length for 432 entries that is about 1.3 KB more. Call it under 3 KB all
in, against roughly 114 KB for the sparse pose-trained apparatus.

Whether to store 432 programs at all is now an open implementation choice rather
than a constraint: the closed form generates the sequence from canonical line
state, so procedural generation and packed replay can be raced on CPU-versus-ROM
economics.

`build/rung1-dashboard.png` plots saturation, per-checkpoint discovery, rank
frequency, cumulative share and the move alphabet.

## Rung 1.5, item 2: adjudicating both depth derivations against geometry

The layered check said where `screen_depth_plane` and the host `inv0/inv1` path
disagree. It could not say which is right, and the standing caution was explicit:
the host is the historical reference implementation, not established ground truth.
This settles it against a reference that is neither implementation.

### The reference

`tools/rung1/depth_adjudicate.c` computes, in double precision, the closed
geometry the fixed-point code is approximating. Every constant in it is read off
the source rather than fitted:

| Quantity | Established from | Value |
| --- | --- | --- |
| ray direction for world bearing `b` | the `sy` branch of `bearing_q12` negates | `u(b) = (cos B, -sin B)`, `B = 2*pi*b/4096` |
| screen x for relative angle | `k_tspf_angle_x_pos` matches to <=1 LSB over all 513 entries | `x = 80 + 80 tan(theta)` |
| column-to-pixel mapping | `screen_depth_plane` anchors `iq` at column 10 = pixel 80 | column `c` samples pixel `8c` |
| projection constant | `k_tspf_invz[z] == round(2560/z)` for every entry | `K = 2560`, `inv = K/z` |
| screen model | `draw_run`, `TSPF_HORIZON = 72`, FULL decrements the top | `h = inv/2`, `y = 71-h` / `72+h` |

For a plane of unit normal `n` through `V` at perpendicular distance
`D = n.(V - P)`, the ray at `theta` meets it at `t = D/(n.u(phi+theta))` and
`z = t cos theta`, giving

```
inv(x) = (K/D) * (A + B*(x-80)/80)     A = n.u(phi),  B = -(nx sin phi + ny cos phi)
```

which is **exactly linear in screen x**. The depth-plane model is therefore the
correct model and only its quantization was ever in question. Every FULL wall on
this map has a cardinal normal (`nx,ny` in `{0,+-32}` Q5), so `n` and `D` are
exact and the reference carries no approximation of its own.

### What had to be separated out first

The first run put both paths at a mean error above 3 px with maxima above 100 px.
Dumping the worst cases rather than explaining them showed every one of them at
`D = 4.0` cells: inside `TSPF_NEAR_Z_Q4`, where `inv_for_dq4` saturates at 255 for
**both** derivations. That is a property of the inverse-depth table, not of either
`(iq,step)` derivation, so the reference now carries the same 10-cell near clip and
127-cell far clip, and the clip is reported on its own:

```
columns behind the near clip 11.74%, past the far clip 2.27%
edge displacement the clip alone causes: mean 17.86  p95 54.26  max 123.06 px
```

Both paths are also scored on **identical columns**: a column counts only when both
integer derivations stay inside the uint8 depth domain and the reference is itself
representable in it. Excluded: ROM outside domain 20.8M, host outside 0, reference
above 255 (near field, unrepresentable by either) 22.8M.

### Per-column screen Y, over 337,285,583 scored columns

| Path | exact | within 1 px | within 2 px | worse | mean | p95 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ROM `screen_depth_plane` (top) | 36.73% | 57.99% | 5.22% | **0.06%** | 0.70 | 1.52 | **2.69** |
| ROM `screen_depth_plane` (bottom) | 36.72% | 57.92% | 5.29% | 0.06% | 0.70 | 1.52 | 2.69 |
| host `inv0/inv1` (top) | 34.80% | 39.41% | 9.45% | **16.34%** | 2.01 | 9.38 | **40.34** |
| host `inv0/inv1` (bottom) | 34.80% | 39.40% | 9.44% | 16.36% | 2.01 | 9.38 | 40.34 |

The ROM path's worst column error anywhere in the sweep is 2.69 px. The host
path's is 40.34 px, and it is worse than 2 px on one column in six. **The prior
that the host is the accurate one and the ROM the approximation is withdrawn: it
is the other way round, and by a wide margin.** The host path anchors on `inv0`
and `inv1` evaluated at the run's clipped angular endpoints and then interpolates
with `k_col_recip_q8`; those endpoints do not sit on column boundaries, so the
interpolation is anchored in the wrong place. `screen_depth_plane` evaluates the
exact linear law at the exact column positions.

### Ownership, and the distance classification

Two row intervals' symmetric difference always lies at their ends, so there is no
such thing as a disputed cell strictly inside both. The first classifier reported
one anyway (50% "interior"); it was wrong, and what actually distinguishes the
cases is how far the end is off and how close the true edge sat to the tile line
it was being rounded against.

| Path | disputed cells | boundary-adjacent (<1 px, a coin flip) | 1 row off | 2+ rows off |
| --- | --- | --- | --- | --- |
| ROM | 69,206,640 (11.37% of 608.6M reference cells) | 88.08% | 11.89% | **0.03%** |
| host | 120,789,845 (19.85%) | 59.18% | 37.93% | **2.89%** |

This is the shape the question was asked in. For the ROM path, 2-or-more-rows-off
disagreement is 21,087 cells out of 608.6 million reference cells: **0.0035%**.
Per-run symmetric difference is 0 at the median for both paths.

### Temporal behaviour

26,327 sequences, 1.14M samples, one renderer input unit per step, kept only while
the same wall still covers the same screen column.

| Motion | reference (rounded) | ROM | host |
| --- | --- | --- | --- |
| strafe | 0.00 | **0.00** | 2.29 |
| walk forward | 0.00 | **0.00** | 3.38 |
| turn | 0.97 | 1.82 | 7.92 |
| all | 0.19 | 0.36 | 3.84 |

(reversals per 100 samples). The ROM path is no worse than the reference in 97.2%
of sequences and adds **zero** reversals under translation; the host path in 52.1%.
The continuous reference is itself monotone in 90.0% of sequences — the residue is
real geometry during a turn, not quantization.

Two harness bugs were found and fixed while producing this, both of which had been
flattering the result: `reversals_d` truncated its direction accumulator to `int`,
so a sub-pixel step zeroed it and no reversal could ever be reported (it claimed
100% monotone); and a single `sid` reachable through more than one key contributed
two samples to one motion step.

### Status

Item 2 of Rung 1.5 is closed, and it closes in favour of the architecture already
in the ROM: `screen_depth_plane` is the accurate derivation, accurate enough that
the raster's remaining disagreement with exact geometry is 88% tile-boundary coin
flips and 0.03% visible extent error. `build/rung1-adjudication.png` carries
visuals 5, 6, 7 and 8.

## The reference is now a permanent regression test

This result is consequential enough that it must not become the next piece of
project folklore in the way "the host is the reference, therefore the host is
right" did. The reference has been factored into `tools/rung1/geometry_reference.h`
and is now **shared verbatim** by the adjudicator and by a test that attacks it
from four independent directions. The adjudicator's output is byte-identical
before and after the refactor, so the test guards the thing that produced the
result rather than a copy of it.

`tools/rung1/geometry_reference_test.c` touches no sampled data of any kind.

| Arm | What it rules out | Result |
| --- | --- | --- |
| A conventions | a regenerated table silently invalidating the derivation | `k_tspf_invz == round(2560/z)` for all 117 entries; `k_tspf_angle_x_pos == round(80 + 80 tan)` to **0 LSB** over all 513; `k_tspf_sec_q7 == round(128/cos)` to 1 LSB; horizon 72, clips 10/127 |
| B algebra | an error reducing the geometry to a line | closed form vs explicit ray/plane intersection, written structurally differently: 184,176 samples, worst relative disagreement **3.1e-13** |
| C hand cases | the reference and the implementation sharing a mistaken convention | 7 cases whose expected values are literal arithmetic derived on paper from the geometry, each with its derivation in a comment |
| D shipped code | the projection constant being fitted rather than real | `inv_for_dq4` tracks `2560/D` to within **0.937 LSB** across all 1,871 distances between the clips; both clip endpoints exact |

Arm C is the one that matters most. C3 (yaw +45) and C4 (yaw −45) differ **only in
the sign of B**, so a flipped bearing convention cannot satisfy both. C5 uses a
+y normal rather than +x, pinning the normal convention independently. C7 asserts
that the near and far clip are *reported* rather than folded into the error, which
is what keeps the near-field class quarantined.

The gate runs in CI as **Continuous-geometry reference gate**, before the
closed-form shape gate, and fails the build unless `GEOMETRY_REFERENCE_OK` is
printed.

### The hierarchy this establishes

```
continuous perspective geometry      authoritative mathematical reference
        |
        v
ROM screen_depth_plane               quantized hardware representation of it
        |
        v
raster trajectory / ownership        the actual display representation
```

The historical host `inv0/inv1` path is a **legacy implementation, not an oracle**.
It is worth keeping as a comparison and regression case, and host tooling that is
supposed to predict real renderer output should eventually be made to follow the
ROM projection convention rather than the other way round.

### Near clip: a separate design decision, deliberately quarantined

The 11.74% near-field class with a mean 17.86 px displacement is `inv_for_dq4`
saturation policy, not depth-plane error. Whether walls that close should clip,
saturate, switch representation, or be unreachable through collision is a real
question and an orthogonal one; it does not bear on choosing the depth
representation, and its statistics must not be mixed into the projection-accuracy
numbers.

## Correcting a stale vocabulary figure, and the denser sweep

An earlier every-4th-cell run reported 1,845 trajectories with a mean length of
19.1 and a 13,233 B payload. That run used a **pre-correction binary**, built
before the uint8 domain check and the run terminator were added, so its
vocabulary was inflated by the same wraparound artifacts already retired. It is
withdrawn.

Re-run with the corrected binary, over every 4th walkable cell (117 cells, 25% of
the map), the full 64x64 local translation space of each, and all 256 headings:

```
FULL runs                       206,327,024
chunk-family instances          663,453,732
out-of-family jumps                       0
distinct raster trajectories            432
  top only 0, bottom only 0, BOTH       432
mean / max length                 9.8 / 16 moves
move-stream payload                   1,586 B at 3 bits per move
collapse instances->trajectories  1,535,773 : 1
```

**432 — the same 432 the eight-cell sweep found.** The growth curve discovers 365
in the first cell, reaches 432 by cell 33, and then finds **nothing new across 84
further cells and 610 million further chunk instances**. All 432 are shared by
both families, so no mirroring is needed at all (at eight cells, 5 were still
family-specific).

`build/rung1-adjudication.png` panel 8 gives the move state-transition matrix,
weighted by chunk instances. It shows real structure: `up 2`, `up 3` and `up 4`
each have **out-degree 1** — a multi-row jump is always followed by a descent.
Out-degrees are `down 1`: 7, `next col`: 4, `up 1`: 4, `up 2/3/4`: 1, `END`: 0.

Also updated with the corrected binary: ROM-path chunks whose depth term leaves
the uint8 domain within the run are 8.699% (host 1.107%), and the ROM-vs-host
layer disagreements are L2 92.022%, L3 42.278%, L4 17.880%. Those remain
*disagreement* figures between two implementations; the adjudication above is what
says which of them is right.

## The exhaustive certificate: 432, over the whole map

The every-4th-cell result is now proof rather than extrapolation. Every walkable
cell, the complete 64x64 local translation space of each, and all 256 headings:

```
FULL runs                       831,745,536
chunk-family instances        2,675,675,264
out-of-family jumps                       0
distinct raster trajectories            432
  top only 0, bottom only 0, BOTH       432
mean / max length                 9.8 / 16 moves
move-stream payload                   1,586 B at 3 bits per move
collapse instances->trajectories  6,193,693 : 1
```

All 432 are found by the **tenth** walkable cell. The remaining 456 cells and
2.58 billion further chunk instances discover nothing new. "Extremely convincing
saturation" is now exhaustive coverage of this map and this domain.

## Is the vocabulary a list or a grammar?

`tools/rung1/grammar_census.py`. Three measurements, no representation chosen.

**Short context does not determinise it.** The hypothesis that order-2 or order-3
context would make almost every transition deterministic is **not supported**:

| context | contexts | H unweighted | determined | H weighted | determined |
| --- | --- | --- | --- | --- | --- |
| order 0 | 1 | 2.1354 | 0.00% | 2.0154 | 0.00% |
| last 1 | 7 | 1.7349 | 12.65% | 1.7390 | 4.43% |
| last 2 | 19 | 1.2010 | 17.01% | 1.3537 | 9.94% |
| last 3 | 41 | 1.0857 | 20.08% | 1.2884 | 10.75% |
| last 4 | 71 | 0.9896 | 23.04% | 1.2391 | 11.48% |

Entropy falls by half but determinism only reaches 23%, and 11% by instance
weight. A short-context Markov model is not the structure here.

**The minimal automaton is 220 states.** A trie over the 432 sequences accepts
exactly that language; Moore partition refinement minimises it to 220 states and
433 transitions over the 7-symbol alphabet, a 4.1x reduction from the 896-state
trie. Verified by construction: the minimised machine accepts exactly 432
distinct strings, counted as paths through the DAG.

Panel 8's observation in numbers: `up 2`, `up 3` and `up 4` each have
**out-degree 1** and are always followed by `down 1`. They are excursions with a
prescribed recovery, not free symbols. `down 1` has out-degree 7, `next col` and
`up 1` have 4.

**But the real state is a DDA accumulator.** The move sequence is a function of
`(a mod 1024, step, length, family)` where `a = iq + 32`. That is provable, not
observed: `h(c) = a(c) >> 7` and `row(c) = y(c) >> 3`, so adding 1024 to `a`
shifts `h` by exactly 8 and every row by exactly 1, leaving the row *differences*
— which is what the moves are — unchanged. Checked over 2,265,184 in-domain
comparisons across step, length, family, terminator and accumulator phase, with
no map data and no sampled poses: **no mismatch**.

So the raster walker's entire state is a 10-bit accumulator phase and a step.
This is Bresenham/DDA at exactly the precision the renderer permits, which is why
the closed-form walker needs no table at all.

| candidate | cost |
| --- | --- |
| 432 packed trajectories | 1,586 B moves + 1,296 B index = 2,882 B |
| minimal automaton | 866 B at 2 B per transition, 220 states |
| closed-form DDA walker | 0 B of table |

None is chosen here; that is a Z80 cycle question, not a ROM-size one.

## A correction: the reference had the bearing convention backwards

The geometry reference stated `u(b) = (cos B, -sin B)`, derived by reading the
`sy` branch of `bearing_q12`. **That was wrong.** `bearing_q12` is a plain
atan2: `u(b) = (cos B, +sin B)`.

What caught it was not the regression test. It was Rung 2's independent
transcription of the baked projection field, whose self-check refused to
validate. Arms B and C could not catch it **because they were written from the
same mistaken reading** — arm B's ray/plane intersection used the same `u`, and
arm C's hand cases were derived on paper from the same premise. That is exactly
the shared-premise failure the hand cases were meant to rule out, and they did
not rule it out.

The reference now pins the convention **empirically**, in a new arm E, against
`bearing_q12` itself over 17,822 vectors: the correct handedness agrees to 2.82
Q12 units, the opposite to 2041.74, a 725x margin. A new hand case C8 uses a
**non-cardinal** normal, where the two conventions genuinely disagree, so the
hand cases can now separate them too. Any future claim about the sign of the y
term belongs in arm E, not in a comment.

**The adjudication is unaffected, verified rather than asserted.** For a cardinal
normal the two conventions give identical `|inv|`: with `ny = 0` the expressions
for A and B are literally the same, and with `nx = 0` both flip sign together, so
the magnitude is unchanged. Every FULL wall on this map is axis aligned. Re-running
the 337-million-column adjudication after the fix produces a **byte-identical**
report. It would have mattered the moment a diagonal FULL wall existed.

## Renderer bug: ratio_q8_exact returns 0 for equal arguments

`ratio_q8_exact(n, d)` computes `round(n * 65536/d / 256)` as a `uint8_t`. For
`n == d` the true value is 256, which does not fit, and it returns **0**. Every
one of the 255 equal-argument cases is affected.

`bearing_q12` scales `|dx|, |dy|` down until both fit in a byte and then takes
that ratio, so the defect fires whenever the *scaled* magnitudes are equal — not
only on exact diagonals but on near-diagonals at distance. When it fires,
`atan_q12[0] = 0` is used and the bearing reports an **axis direction up to 45.2
degrees away**.

Measured over every walkable pose at Q4 resolution against all 14 corners
(26,507,261 lookups):

| corner class | lookups | hits | rate |
| --- | --- | --- | --- |
| all | 26,507,261 | 45,085 | 0.1701% |
| served by the bake (correct there) | 17,981,440 | 31,272 | 0.1739% |
| certified fallback, exact path | 28,669 | 378 | **1.3185%** |

In the shipped ROM the local-projection bake masks this for every corner it
covers, so the exposure is the certified fallback corners and any build with
`TSPF_LOCAL_PROJECTION=0` — including the host tooling. It is a one-frame 45
degree bearing pop where it does reach the screen, which is precisely the kind of
artificial temporal event the certificate architecture cannot tolerate.

## Rung 2: does the representation survive a coarse-cell crossing?

`tools/rung1/cross_cell_check.c`. The projection's front half is a per-coarse-cell
baked field; crossing a cell swaps the whole record set. The question is not
whether anything changes when the player moves — it must — but whether a boundary
step changes **more than an ordinary step in open space**.

`tools/rung1/local_projection_host.h` transcribes the ROM's baked evaluator onto
the host: the cell parse from `projection_load_cell`, the leaf indexing and the
truncate-toward-zero scaled multiply from `tilesector_polar_projection_gg.s`. The
bake bytes are lifted verbatim from the generated bank units the ROM is built
from. Validated against what the bake was fitted to: **worst 3.98 Q12 units**,
against an emit threshold of 4.

Steps are classified by what they cross, and **both** integer paths are scored
against continuous geometry rather than against each other. The first version of
this harness used `bearing_q12` as "exact" and would have charged every
near-diagonal defect to the bake.

PHANTOM transitions — the integer path moves to a new raster state where geometry
stays put, i.e. a manufactured event:

| step class | baked L1 / L3 / L4 | bearing_q12 L1 / L3 / L4 |
| --- | --- | --- |
| interior | 1.041% / 1.021% / 0.985% | 1.437% / 1.403% / 1.342% |
| leaf boundary | 1.344% / 1.319% / 1.202% | 3.673% / 3.533% / 3.332% |
| **CELL BOUNDARY** | **2.358% / 2.312% / 2.248%** | 2.370% / 2.361% / 2.271% |

MISSED transitions — geometry moves and the integer path does not:

| step class | baked | bearing_q12 |
| --- | --- | --- |
| interior | 0.993% / 0.964% / 0.919% | 1.412% / 1.369% / 1.278% |
| leaf boundary | 0.635% / 0.616% / 0.565% | 0.953% / 0.917% / 0.852% |
| CELL BOUNDARY | 0.557% / 0.557% / 0.532% | 1.273% / 1.262% / 1.175% |

Static disagreement with geometry at a single pose: baked 1.56% of run instances,
`bearing_q12` **3.99%**.

### Reading

**Cross-cell continuity holds, with a bounded penalty.** A coarse-cell crossing
raises the phantom-event rate from 1.04% to 2.36% — about 2.3x, not a cliff. A
crossing costs roughly what two ordinary steps cost. Leaf boundaries are barely
worse than open space (1.34%), so the quadtree refinement inside a cell is not a
source of artificial events at all.

**And the same inversion as the depth result.** The baked field is not an
approximation of the exact path; it is **more faithful to geometry than the exact
path is**, by 2.5x on static disagreement and on every phantom and missed
transition measure except cell-boundary L1, where they tie. The bake was fitted
against continuous truth; `bearing_q12` is an integer atan2 with a byte-ratio
defect. This is the second time the "legacy path is the oracle" assumption has
been the wrong way round.

Crossing cost: 466 coarse cells carry a record, 43,805 B total, 94.0 B mean. A
crossing re-parses one whole cell record; nothing is reloaded within a cell.

## Rung 2, restated precisely, with magnitudes

Two phrasings in the entry above are withdrawn.

**"the integer path moves to a new raster state where geometry stays put."** The
projected geometry never stays put: the player moves through a fixed map, so the
continuous projection of every edge changes on every step, and a raster
transition is *expected* whenever that moving edge crosses a quantization
threshold. The measurement was right; the sentence was not. The implementation
compares, for each step, whether the scored path's quantized raster state changed
against whether the **reference path's** did, where the reference path is the
identical raster pipeline driven by continuous-truth bearings quantized to Q12
only at the bearing step. So the quantity is:

> an approximation-induced transition occurs when the baked (or integer)
> projection changes quantized raster state although the continuous reference has
> not crossed the corresponding raster boundary.

That is what the code counts, verified by reading it; only the prose was loose.
Isolating the bearing source is the right control for Rung 2, because a
coarse-cell crossing changes nothing else.

**"Continuity holds with a bounded penalty."** Stronger than the metric supports.
The result is: *no catastrophic coarse-cell discontinuity was observed;
approximation-induced raster transitions rise from 1.041% for interior motion to
2.358% on coarse-cell crossings.*

### And the magnitudes, which incidence alone cannot supply

The depth investigation taught this lesson once already, so the harness now
carries an explicit 18x20 cover bitmap rather than a hash, and measures how large
each manufactured transition is:

| path | step class | endpoint jump <=1px | p95 | max | cells changed <=2 | p95 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| baked | interior | 94.94% | 2 | **3** | 60.57% | 4 | 6 |
| baked | leaf boundary | 94.04% | 2 | **2** | 62.39% | 4 | 8 |
| baked | CELL BOUNDARY | 90.27% | 2 | **2** | 61.73% | 4 | 8 |
| bearing_q12 | interior | 81.91% | 2 | **32** | 55.60% | 6 | 28 |
| bearing_q12 | leaf boundary | 54.15% | **32** | **32** | 45.07% | 24 | 32 |
| bearing_q12 | CELL BOUNDARY | 82.38% | 2 | 3 | 63.03% | 4 | 6 |

For the baked field every manufactured transition is a threshold crossing
arriving a step early or late: **the largest endpoint jump anywhere in the sweep
is 3 pixels**, and 90-95% are one pixel or less. The multi-cell figures are the
same events seen on tall spans, where a one-row shift moves several cells at once.

`bearing_q12` is a different story: 32-pixel jumps, and at leaf boundaries a p95
of 32 with only 54% at one pixel. That is the `ratio_q8_exact` defect appearing
as exactly the kind of large jump that incidence alone would have hidden.

## Separating the proof vocabulary from the runtime architecture

The 432 trajectories are the complete set of **finished strings** the renderer
emits over this map and domain. Their endpoints carry span-length and termination
information, which is world-specific. They are excellent evidence and a possible
implementation; they are not automatically the thing to store, and turning them
into another dictionary would repeat the mistake this whole investigation was
started to undo.

The generic result underneath them is the algebraic one:

```
WORLD-SPECIFIC     visible wall span -> projected endpoints
                   -> depth-plane parameters -> span length / stop X
                            |
GENERIC STATE               v      10-bit DDA phase + step
                            |
GENERIC MACHINE             v      emit move, advance, ... stop after span length
```

Nothing below that line knows a wall id, a map cell, or a neighbouring span.

## The DDA compiled into a transducer

`tools/rung1/dda_transducer.c` builds that machine directly rather than inferring
it from strings. State `(phase, step)`, input "advance one column", output
`(ndown, jump)`, successor `phase' = (phase + step) mod 1024`. Span length stays
outside. It reads no map data, no pose and no sampled trajectory, and it
self-checks against the direct shape computation over 200,000 random cases with
zero mismatches.

The parameter space is `1024 phases x 3103 reachable steps = 3,177,472` states.
The reachable step set is derived from the shipped tables (205 distinct step
factors, 226 distinct inverse depths).

States surviving L-column output equivalence — two states merge when they emit
the same output for the next L columns:

| L | fam 0 | fam 2 | both |
| --- | --- | --- | --- |
| 1 | 9 | 9 | 13 |
| **6** (one chunk) | **146** | **146** | **175** |
| 12 | 812 | 812 | 905 |
| 20 (longest run) | 3238 | 3238 | 3461 |

At the chunk length the renderer actually uses, 3.18 million parameter states
collapse to **175 behaviours**. Note this is *smaller* than the 432 trajectories,
because it does not encode termination.

### But a small behaviour space is not a cheap runtime

Naming a class costs nothing; **mapping a `(phase, step)` to it is the whole
cost**, and a map over 3.18 million parameter states is not a table anyone ships.
That is precisely where the sampled dictionary died, in a new guise. So the
question is how the classes sit along one step's phase line:

```
distinct 6-column behaviours among the 1024 phases of ONE step
  mean 8.0, worst 8; distribution  1:3  2:4  4:6  8:3090
contiguous behaviour bands per step
  mean 7.98, worst 8
```

Eight, and they are **contiguous bands**, not interleaved. They are not the eight
128-wide blocks of phase (only 25 of 3103 steps), so it is not a bare shift. But
the edges have a closed form, and it is verified for **all 3,103 steps**:

> every band edge lies at `p = (-c * step) mod 1024` for some `c` in 0..7

which is exactly where it must be: a row boundary is crossed when the accumulator
passes a multiple of 1024, and a 6-column span inspects columns 0..7. The eight
predicted phases are a *superset* of the actual edges (mean 7.98 of 8 are real),
which is the safe direction: a distance computed against them never overstates.

Two consequences:

1. **Indexing is a rank among eight computed phases, not a lookup.** That is what
   would make a compiled transducer cheaper than re-running six columns of DDA
   arithmetic — and it is the first version of this idea that does not need a
   dictionary.
2. **The distance to the nearest band edge is the safe region.** While the phase
   stays inside its band, the emitted 6-column span does not change at all. That
   is the temporal certificate, in closed form, rather than as a measured
   coincidence.

### Candidates, sized; none chosen

| representation | cost |
| --- | --- |
| arithmetic DDA | 0 B of table, per-column arithmetic |
| compiled 6-column transducer | a band rank + a 175-entry table |
| 432 packed trajectories | 1,586 B moves + 1,296 B index |
| 220-state trajectory DFA | 866 B, but accepts finished strings |

The last two encode span termination and are therefore world-specific. The first
two keep length external, which is the property worth having. Which is fastest is
a Z80 cycle question; this only sizes them.

## Rung 3: the phase-band certificate, validated against real player motion

`tools/rung1/temporal_certificate.c`. Not a census — a falsification test. For
every retained span it computes what the certificate *claims* is safe, then moves
the player one unit at a time and records when the emitted raster actually
changes.

```
certificate SAFE at step k  <=>  step unchanged
                             AND projected columns c0,c1 unchanged
                             AND no band edge crossed by any chunk's phase
```

The property is one-sided: conservative is fine (wake early), claiming safe after
the output has already changed is fatal. Over **1,458,755 spans per motion axis**,
every walkable cell sampled at 1-in-2, 8x8 sub-cell, 64 headings, 32 motion steps:

| bearings | motion | spans | **UNSAFE** | conservative | exact |
| --- | --- | --- | --- | --- | --- |
| continuous truth | +X | 1,458,755 | **0** | 16.1% | 83.9% |
| continuous truth | +Y | 1,458,755 | **0** | 33.9% | 66.1% |
| continuous truth | +yaw | 1,458,755 | **0** | 30.0% | 70.0% |
| bearing_q12 | +X | 1,450,811 | **0** | 16.0% | 84.0% |
| bearing_q12 | +Y | 1,450,811 | **0** | 33.8% | 66.2% |
| bearing_q12 | +yaw | 1,450,811 | **0** | 30.1% | 69.9% |

Zero unsafe cases, and the certificate is *exact* — predicting the change on the
very step it happens — in 66-84% of spans.

### A real bug the test caught

The first run reported 5-6 unsafe cases per axis, on **both** bearing paths, so
not the `ratio_q8_exact` defect. Dumping them showed every one identical:
`step == 0`, phase sitting exactly on 0, moving backwards. With `step == 0` all
eight edges `p = (-c*step) mod 1024` collapse onto phase 0, and my crossing test
treated "sitting on an edge" as uncrossable in *both* directions. Crossing is
asymmetric: forward by m crosses edge e when `(e-p) mod 1024` is in `[1,m]`;
backward by m crosses it when `(p-e) mod 1024` is in `[0,m-1]` — zero counts.
Fixed, and the violations went to zero. Worth noting because the failing case was
a degenerate one (a wall at constant depth across the span) that no amount of
typical-case testing would have produced.

### What actually forces re-evaluation — and it is not the phase

| motion | phase band | **step** | endpoint column | left frame | unattributed | no change in 32 |
| --- | --- | --- | --- | --- | --- | --- |
| +X | 0.5% | **21.8%** | 29.1% | 3.1% | **0.0%** | 45.5% |
| +Y | 0.9% | **43.8%** | 26.1% | 3.4% | **0.0%** | 25.8% |
| +yaw | 7.2% | **80.3%** | 9.2% | 3.2% | **0.0%** | 0.0% |

**Zero unattributed changes**: every real raster change has an identified cause.

The phase band is *not* the binding constraint — it accounts for 0.5% of changes
under +X. What forces re-evaluation is `step`, then the endpoint column. That
redirects the next work: the phase machinery is nearly free, and the leverage is
in certifying `step` and the endpoints.

And `step = (invd * sf_q4[class][yaw]) >> 4`, so:

* under **translation** `step` moves only through `invd`, the wall distance, so an
  `invd`-stability certificate would extend the safe region directly;
* under **rotation** `sf_q4` is indexed by yaw and changes almost every unit,
  which is why +yaw has a median safe distance of **1**. The current certificate
  is close to useless for turning.

The fix for rotation is the one already visible in the band result: certify that
`step` stays in the same *behaviour* region rather than that it keeps the same
*value*. This harness tests exact equality, which is sound but the most
conservative possible choice.

### How long it holds

| motion | median safe steps | p90 | still safe at 32 |
| --- | --- | --- | --- |
| +X (1/16 cell) | 22 | 32 | 40.1% |
| +Y (1/16 cell) | 5 | 32 | 18.6% |
| +yaw (1 unit) | 1 | 1 | 0.0% |

Under forward-ish translation the certificate typically proves ~1.4 cells of
travel safe for a given span, and 40% of spans are still provably unchanged after
2 full cells. That is real leverage. Rotation is not, yet.

## Visual pack

`build/rung1-certificate.png` — six panels: the phase x step behaviour atlas with
the analytic edges overlaid; how long the certificate holds per motion axis;
predicted vs observed first change with the unsafe region marked empty; the cause
breakdown; the Rung 2 worst tail; and the exhaustive saturation curve over all
466 cells.

`build/rung1-phase-bands.png` — annotated phase-band strips for six representative
steps, showing the eight edges, the raster each band emits, and a worked safe
distance. One detail visible there and relevant to the Z80 indexing question: the
edge labels are **not monotonic** along the phase line (step 143 runs c0,c7,c6...;
step 287 runs c0,c7,c3,c6,c2...), so the band rank is not a simple ordering of c
and a naive eight-way compare is not obviously the cheapest decision procedure.

## Framing correction

"The last two encode termination and are world-specific" was stronger than
warranted. Some or all of those 432 complete trajectories might well occur in
other maps, because the tiny raster imposes the same possibilities. The accurate
objection is narrower: **their claimed completeness is established only for the
span lengths and geometries this domain exercises, whereas phase + step with an
external stop condition describes the generic raster process itself.**

## Two plot bugs, fixed

**Panel 3 had the unsafe region on the wrong side.** Axes are x = observed first
change, y = certificate-predicted first change, and soundness requires
`predicted <= observed`, so the forbidden region is **above** the diagonal. The
title said "nothing may fall below the diagonal" and — worse — the shading and the
"UNSAFE region" label were both drawn *below* it, over the safe conservative side.
Title and geometry corrected, and the conservative side is now labelled as such.

**Panel 4 and the run's table use different denominators.** The plot conditions on
spans whose raster changed; the table is a share of all spans, which is why its
+X row totals ~54%. Both are useful; the plot axis now says which it is.

## Rung 3b: behavioural-region stability instead of exact-step equality

Exact step equality was the wrong invariant, as suspected. Four tiers are now
measured on the same corpus, each checked against the quantity **it** claims
rather than against raster equality, which only STRICT implies. All four are
sound: zero violations everywhere.

| motion | certificate | median | p90 | safe at 32 | unsafe |
| --- | --- | --- | --- | --- | --- |
| +X | STRICT (nothing to do) | 23 | 32 | 41.0% | 0 |
| +X | PROGRAM (program + columns) | 28 | 32 | 46.8% | 0 |
| +X | **SHAPE (program only)** | **32** | 32 | **59.9%** | 0 |
| +X | CEILING (raster unchanged) | 27 | 32 | 45.5% | — |
| +Y | STRICT | 5 | 32 | 19.1% | 0 |
| +Y | PROGRAM | 12 | 32 | 26.5% | 0 |
| +Y | **SHAPE** | **18** | 32 | **39.5%** | 0 |
| +Y | CEILING | 14 | 32 | 25.8% | — |
| +yaw | STRICT | 0 | 0 | 0.0% | 0 |
| +yaw | PROGRAM | 0 | 1 | 2.8% | 0 |
| +yaw | **SHAPE** | **0** | **5** | **6.5%** | 0 |
| +yaw | CEILING | 0 | 1 | 0.0% | — |

Dropping exact-step equality is worth a lot for translation: SHAPE roughly doubles
the +Y median and takes +X past the 32-step window. SHAPE can legitimately exceed
CEILING because it certifies less — the program is reusable while the rows or
columns move, which is a cheap update rather than a rebuild.

**It does not rescue rotation.** +yaw SHAPE still has a median of 0 and a p90 of
5. Behavioural-region stability was the right idea and it is not enough.

## Rung 4: is there a compact deterministic yaw transition?

`tools/rung1/yaw_determinism.c`. Partition refinement run as a measurement: start
from the smallest plausible state, check whether every sample carrying it produces
the same successor under +-1 yaw, and add a field only when the data forces it.

**A — successor = the next full state.** Conflicting states, by state / by instance:

| state | states | conflicting | by state | by instance |
| --- | --- | --- | --- | --- |
| prog | 7,230 | 5,558 | 76.87% | 69.98% |
| + band | 16,582 | 11,888 | 71.69% | 73.66% |
| + columns | 101,510 | 58,844 | 57.97% | 58.99% |
| + rows | 145,288 | 74,951 | 51.59% | 47.38% |
| + step | 221,366 | 92,425 | 41.75% | 28.99% |
| + yaw | 289,614 | 98,138 | 33.89% | 24.70% |
| + class + iq | 293,136 | 98,731 | 33.68% | 24.36% |
| + invd + wall id (everything) | 304,814 | 99,135 | **32.52%** | 23.46% |

Adding `iq`, `invd` and the wall id barely moves it. **No compact yaw-successor
machine exists over the span's own state** — and nor does a large one. The
obstruction is identified in section C.

(The first run of this table printed "137% conflicting". States were pooled over
the two directions as `S/2` while conflicts were summed. That is how the bug
announced itself; fixed.)

**C — the obstruction, and the factorisation.** The successor depends on the
projected columns at the *next* yaw, which come from the corner bearings and
therefore from the pose, which no span-state field carries. Factor the columns out
and the program is exactly determined:

| state | states | conflicting |
| --- | --- | --- |
| invd + class + yaw | 30,896 | 77.023% |
| invd + class + yaw + c0 | 101,358 | 10.738% |
| **invd + class + yaw + c0 + c1** | **146,568** | **0.000%** |
| + wall id | 152,407 | 0.000% |

This is implied by `dp_derive`'s own signature, so on its own it confirms the
harness rather than discovering something. What it establishes is the negative:
**nothing else is needed** — not the pose, not `iq`, and not the wall identity
beyond its normal class. Combined with A, the decomposition is

```
pose, yaw  ->  corner bearings  ->  c0, c1       an endpoint event stream
pose       ->  wall distance    ->  invd
(invd, class, yaw, c0, c1)      ->  raster program, exactly, pose-free
```

**D — is there a rotational program vocabulary?** Hold `(invd, class, c0, c1)`
fixed, sweep yaw through a full turn, and count:

```
distinct (invd, class, c0, c1) configurations   11,432
distinct 256-yaw program sequences               4,325
collapse                                           2.6 : 1
program changes per full turn                    163.5 of 256
mean yaw units held between changes               1.55
hold lengths   1:73.2%  2:15.8%  3:5.8%  4:2.4%  5:1.0%  6+:1.4%
```

**No.** The static trajectories collapsed 6,193,693:1; rotational sequences collapse
2.6:1. The program changes on 64% of yaw ticks and **73.2% of holds are exactly
one yaw unit**. The "nothing changes for another six yaw units, then transition to
program 37" model does not hold for this renderer.

So rotation must **encode transitions cheaply rather than avoid them**. A hold
counter would idle 73% of the time at zero. Killed with dignity, as instructed.

What survives is the factorisation: the per-tick program is a pure function of a
five-field pose-free state, so the rotational cost is one program derivation per
tick, not a projection rebuild — and the endpoint stream is a separate, slower
event source worth certifying on its own.

## Edge orderings, measured and then left alone

Over all 3,103 reachable steps the eight edges appear in only **20 distinct
orderings** along the phase line. That bounds the cold-start "which of eight bands
am I in" problem to a 20-entry permutation table. Per the standing direction this
is recorded and not optimised: a renderer that already knows its band needs only
the two neighbouring edges, and the non-monotonic labelling stops mattering the
moment the band is state rather than a classification.

## Withdrawing an overstated conclusion

**"No compact yaw machine exists" was wrong.** What Rung 4 actually showed is:

> no deterministic +-1-yaw successor machine exists over the span-state
> representations tested.

Those are different claims, and the second one does not imply the first. Worse,
the whole framing was wrong: a renderer takes an arbitrary `(dx, dy, dyaw)`
between frames. Unit-step experiments are a fine way to *discover* boundaries and
a bad model of the runtime. Nothing should ever imply "the player turned 5 units,
so execute five yaw transitions". The runtime question is:

> given a retained state and an arbitrary delta, can we cheaply prove the
> destination is in the same behavioural region; and if not, can we classify the
> destination directly without replaying what was crossed?

Margins, not counters. The translation numbers above (median 23, 28, 32) should
be read as displacement budgets tested against whatever `dx` a frame brings, not
as decrements.

## Rung 5: arbitrary-delta endpoint projection

`tools/rung1/endpoint_machine.c`. Every delta is applied **directly**; nothing is
derived by iterating single steps. Deltas tested: +-1, 2, 3, 5, 8, 13, 21, 34, 55,
89, 128.

### The rotation shortcut is exact

Under pure rotation the camera does not move, so every corner's absolute bearing
is fixed and only the camera angle changes. From `project_key`,
`st = signed_q12(a0 - (yaw << 4))`, so with the pose fixed

```
st' = signed_q12(rel - dyaw*16)      for ANY dyaw,   len unchanged
```

and the wrap, the +-512 clip, `angle_x`, the columns and both real-edge flags all
follow. Checked against the full pipeline rather than assumed:

```
224,493 comparisons, 224,493 exact, 0 mismatched
columns, pixel endpoints and both edge flags all agree, for every delta
including +-128, computed in one shot
```

So arbitrary-delta rotation needs **no projection at all**: two 12-bit values per
run, one subtract, and two `angle_x` lookups. No `bearing_q12`, no atan.

### The successor conflicts were a lossy variable, exactly as suspected

Is the destination column determined by the retained state?

| retained state | states | conflicting | by state |
| --- | --- | --- | --- |
| c0, c1 (the column alone) | 3,392 | 2,950 | **86.97%** |
| c0, c1, len | 165,885 | 12,181 | 7.34% |
| x0, x1 (pixel endpoints) | 61,779 | 4,297 | 6.96% |
| **rel, len (angular state)** | 205,795 | **0** | **0.00%** |

Two endpoints at screen x 36.51 and 37.46 share column 37 and separate under a
turn. The column had destroyed the sub-column angular phase. Retain the angular
state instead and the destination is determined exactly. **The Rung 4 conflicts
were a representation defect, not physical complexity** — the same lesson the DDA
work already taught once.

### But the yaw margin really is near zero

```
7,275 spans   mean 0.55   median 0   p90 1 yaw unit
distribution  0: 70.8%   1: 25.9%   2+: 3.3%
```

One yaw unit is 1/256 of a turn over a 90-degree, 160-pixel screen: about 2.5
pixels, a third of a tile column. A column change per tick is close to
unavoidable, and no better retained variable can rescue that — it is geometry.

**So rotation is not a reuse problem, it is a recomputation problem, and the
recomputation is nearly free.** The architecture for turning is "classify the
destination directly in O(1) from retained angular state", not "prove nothing
changed". That is the same answer the unit-step experiment gave, arrived at
honestly, but it now comes with the mechanism that makes it cheap instead of a
verdict that rotation is expensive.

### Independent margins are NOT composable

```
mean translation margin        X 19.71,  Y 18.39  sixteenths of a cell
moves inside BOTH X and Y margins    7,031 tested,  376 broke the columns  (5.35%)
moves inside BOTH X and yaw margins  4,302 tested,  476 broke the columns  (11.06%)
```

Per-axis margins cannot be conjoined. A certificate has to represent a genuinely
joint region of pose space, or use a conservative test that accounts for the
coupling. This is measured, not assumed, and it is a constraint on any future
certificate design.

Translation margins themselves are healthy: a mean of about 19 sixteenths, so
more than a cell of travel before the projected columns move.

## Rung 6: arbitrary-pose destination evaluation, all three axes at once

`tools/rung1/arbitrary_pose.c`. The architectural statement being tested:

> stateless arbitrary-delta **destination evaluation** is the foundation;
> temporal certificates are optional shortcuts on top of it.

Correctness must never depend on how fast the player moved. Every delta is
applied in **one shot**, and boundary crossings and combined `(dx,dy,dyaw)` are
first-class cases rather than an afterthought, so that the pleasant same-leaf
case is not the only thing proved.

Retained per run: its identity `(sid, v0, v1)` and its angular state `(rel, len)`.
Destination evaluation then locates the destination cell and leaf, evaluates the
baked bearings there, subtracts the destination yaw, runs `angle_x`, derives
`invd` and `(iq, step)`, and emits the raster — with no traversal of anything
crossed.

| delta | what it crossed | cases | endpoint | iq/step | raster |
| --- | --- | --- | --- | --- | --- |
| translation only | same leaf | 28,437 | 0 | 0 | 0 |
| | crosses a leaf | 1,638 | 0 | 0 | 0 |
| | crosses a coarse cell | 19,226 | 0 | 0 | 0 |
| | crosses several cells | 12,463 | 0 | 0 | 0 |
| rotation only | same leaf | 42,371 | 0 | 0 | 0 |
| combined dx,dy,dyaw | same leaf | 26,247 | 0 | 0 | 0 |
| | crosses a leaf | 1,413 | 0 | 0 | 0 |
| | crosses a coarse cell | 13,784 | 0 | 0 | 0 |
| | crosses several cells | 16,903 | 0 | 0 | 0 |
| **TOTAL** | | **162,482** | **0** | **0** | **0** |

**Exact at every layer**, including jumps of up to 144 sixteenths crossing
several coarse cells, and including combined motion on all three axes at once.

### A harness bug the dump caught

The first run reported 922 endpoint mismatches, concentrated in the
several-cells rows. Dumping them showed `a1` agreeing *exactly* while `a0` was
477 Q12 units out — a signature, not noise. The cause: `find()` matched runs by
`sid`, and one wall can be reached through several keys with **different corner
pairs**, so a retained run was being compared against a different key's run at
the destination. Matching on `(sid, v0, v1)` took the mismatches to zero. The
architecture was never in question; the pairing was.

### The cheap in-leaf shortcut does not work

```
54,538 same-leaf translations, 36,156 disagreed (66.295%)
```

Adding `Ax*dx + Ay*dy` to a retained bearing is **not** the same as evaluating
the leaf's affine record at the destination, because the ROM truncates each
product toward zero separately. So the incremental form is not a legal
optimisation even inside one leaf, and destination evaluation is the primitive.
This is exactly the case that "linear within a leaf" would have invited someone
to assume.

### Safe regions are half-planes, not boxes

`build/rung6-safe-region.png` plots, for representative spans, `dx` against `dy`
at fixed yaw and `dx` against `dyaw` at fixed position, coloured by whether the
projected columns are unchanged, with the rectangle the independent per-axis
margins would claim overlaid.

The regions are bounded by slanted, staircased lines that cut straight across
that rectangle. That is what an affine bearing field inside a leaf predicts:
inside a leaf the relative angle is `base + Ax*x + Ay*y - yaw`, so a column
boundary is a single constraint on that whole expression — one plane in
`(x, y, yaw)` space, not three independent margins. It explains the 5.35% and
11.06% composability failures exactly, and it says the eventual certificate is a
small polyhedron, or a conservative test on the coupled expression, rather than a
box.

## Two corrections to how the last result was stated

**"The foundation claim now holds end-to-end" was too strong.** The accurate
statement is: *the arbitrary-destination architecture reproduces the renderer
exactly across the tested translation, rotation, combined-motion and
multi-cell-crossing matrix.* 162,482 cases is strong validation; it is not the
exhaustive proof the 2.68-billion-instance trajectory census was.

**"Retaining `(sid,v0,v1)` and `(rel,len)`" should not read as four values
predicting arbitrary translation.** They do two different jobs. `(rel, len)` is
the angular state, and it advances directly for rotation. `(sid, v0, v1)` is
geometric *identity*, and for translation nothing is advanced at all: the
identity is what lets the destination cell and leaf be located so the baked
geometry can be re-evaluated there. The architecture is

> retain enough identity to cheaply re-evaluate the right generic baked geometry
> at the new destination

not "incrementally massage yesterday's answer forward".

## Fixing ratio_q8_exact

`ratio_q8_exact(n, d)` is called only with `n <= d`, so the true Q8 ratio is in
`[0, 256]` and reaches 256 exactly when `n == d`. 256 does not fit the byte, the
cast wrapped it to **0**, and every caller read that as a ratio of zero. In
`bearing_q12` that turned a diagonal into an axis direction.

It now saturates:

```c
return (uint8_t)(q > 255u ? 255u : q);
```

One compare. The residual is about one Q12 unit — `atan_q12[255]` is
`atan(255/256)`, 44.89 degrees against a true 45 — which is inside the table's own
quantisation. Widening the atan table to 257 entries would remove even that, at
the cost of a 16-bit index on the Z80; not worth it for one LSB.

| measurement | before | after |
| --- | --- | --- |
| `ratio_q8_exact(n,n)` | 0 for all 255 n | 255 for all 255 n |
| worst `bearing_q12` error, 361,200 vectors | 512 Q12 (45.0 deg) | **4.45 Q12 (0.39 deg)** |
| worst error on the vectors that trip the wrap | 512 Q12 | **1.28 Q12 (0.11 deg)** |
| baked field vs `bearing_q12`, worst | 515 Q12 (80.5 px) | **8 Q12 (1.25 px)** |

The same helper had been copied verbatim into the lattice floor-light runtime in
`tools/apply_lattice_floor_light_runtime.py`; fixed there too, so a corrected bug
does not survive in a copy.

### The poisonous tails are gone

Re-running Rung 2. The baked field is unchanged, as it must be — it never had the
defect. `bearing_q12`, the supposedly trustworthy fallback, improves sharply:

| measure (bearing_q12) | before | after |
| --- | --- | --- |
| worst endpoint jump, interior | **32 px** | **4 px** |
| worst endpoint jump, leaf boundary | **32 px** | **2 px** |
| jumps <= 1 px, leaf boundary | 54.15% | **79.65%** |
| worst cells added+removed, interior | 28 | 12 |
| approximation-induced transitions, leaf boundary | 3.673% | **2.482%** |
| static disagreement, leaf boundary | 4.563% | **3.841%** |

The rare catastrophic tail that made retained state unsafe is eliminated. The
baked field is still the more faithful path everywhere, but the fallback is now a
trustworthy slow path rather than the opposite.

Arm E of the geometry regression test no longer *reports* this as a finding; it
**asserts** it, so the saturation cannot be lost silently. Rung 6 re-verified
after the source change: still 162,482 cases, zero mismatches.

## Three follow-ups to the fallback fix

### ratio_q8_exact is now ratio_q8_sat

The name was actively dangerous. The helper is **not** exact at unity — 1.0 is 256
in Q8 and does not fit a byte — so anyone reading "exact" could reasonably decide
the clamp is a wart and remove it, which is precisely the bug it fixes. Renamed
in all six files that referenced it, with the reasoning written at the definition.

### A shared vector suite, because the copies cannot be shared

`tools/rung1/helper_vectors.h` holds 30 canonical `{n, d, expected, tolerance}`
vectors, checked inside the geometry reference gate.

Expected values are derived from the **ideal**, `round(n*256/d)`, not from the
implementation — recording whatever the code prints would make the suite circular
and unable to catch the next regression. Two of my hand-derived values were wrong
on the first run, and the reason is worth keeping: the helper rounds through a
reciprocal table, so `100/101` is 253.47 ideally but the table rounds `65536/101`
up to 649 and carries it to 254. That vector now carries an explicit tolerance of
1 and says why, rather than the expectation being quietly bent to match. Unity,
zero, exact binary fractions and near-unity all carry tolerance **zero**.

### A lint against divergent copies

`tools/check_duplicated_helpers.py` fails the build if any copy of the ratio
helper drops the saturating return or if the old name reappears in code (a
historical mention in a comment is allowed). Self-tested: reintroducing
`return (uint8_t)q;` makes it exit non-zero, and restoring the clamp makes it
pass. It runs in CI before the geometry gate.

Lint and vectors both exist because either alone would have missed this bug: the
vectors only cover the copy they are compiled against, and the lint only covers
the shape.

### The representation handoff, sought out rather than stumbled upon

Rung 6 gained a `REPRESENTATION HANDOFF` class: a move where a corner changes
which representation serves it, baked field to `bearing_q12` fallback or back. It
outranks the geometric classes, because whichever boundary was crossed, that is
the interesting fact.

A uniform sweep lands on one by accident — 276 cases out of 1.1 million — and
that is a coincidence, not a regression. So the fallback corners are now
enumerated first and moves are **aimed across** their cell boundaries in both
directions:

```
7 certified fallback corners map-wide
632 moves aimed across a representation boundary, 0 mismatched   EXACT
both directions, deltas from 1 to 96 sixteenths,
compared at the endpoint, iq/step and raster
```

632 is modest, and it is bounded by there being only seven such corners in the
whole map. But it is now deliberate coverage of the seam rather than incidental.

The denser Rung 6 run also stands: **1,100,367 cases, zero mismatches** at the
endpoint, `iq/step` and raster layers.

## The Z80 race: measured, and the elegant answer loses

`tools/race/`. Three implementations of **one** function — given `(iq, step)` and
a span length supplied from outside, emit the raster move stream — built as a
Game Gear ROM and timed under Gearsystem. Units are Z80 T-states: Gearsystem
accumulates what `RunInstruction()` returns, which is the instruction's own cycle
count.

* **DDA** straight-line arithmetic, no table at all.
* **BAND** the transducer, implemented honestly: setup once per run computes the
  eight edge phases; each chunk classifies its band and replays a six-byte
  program, memoising each band's program the first time it is needed.
* **PACKED** copies a stored stream and is **charged nothing** for identifying
  which stream it needs. Deliberately flattered: it is a floor on what any
  table-driven approach could cost, not a proposal.

Span length is external for DDA and BAND, so chunk chaining is charged rather
than hidden by benchmarking only the six-column case. The profiler refuses to
print timings until the ROM has proved all three byte-identical against the
generated expectation.

### The race caught my own hand-wave first

The first run reported 66 of 88 cases disagreeing, and the host checker localised
it: DDA correct everywhere, BAND diverging on the **second** chunk of every
multi-chunk span. The cause was the phrase from the earlier analysis — "the band
index is a rank among eight computed phases". That was never a valid index.
Counting edges *within half a turn behind* the phase puts two different bands on
the same number, so a memoised program gets replayed for a chunk it does not
describe. The correct index is the count of edges **at or below** the phase, which
needs no wrap case because the `c = 0` edge always sits at phase 0. With that,
88 of 88 agree.

### T-states per span

| columns | DDA | BAND setup | BAND chunks | **BAND total** | **PACKED** |
| --- | --- | --- | --- | --- | --- |
| 3 | 7,500 | 2,123 | 12,178 | **14,301** | **1,228** |
| 6 | 13,926 | 2,081 | 15,058 | **17,139** | **1,844** |
| 12 | 26,741 | 2,081 | 29,141 | **31,222** | **3,286** |
| 18 | 39,340 | 2,123 | 41,524 | **43,647** | **4,427** |

Per column: DDA 2,500 / 2,321 / 2,228 / 2,186; BAND 4,767 / 2,857 / 2,602 / 2,425;
PACKED 410 / 307 / 274 / 246.

**The band transducer loses to the plain DDA at every length** — 1.91x at three
columns, 1.23x at six, 1.17x at twelve, 1.11x at eighteen. The gap narrows as
memoisation amortises but never closes, and the reason is structural: building a
band's program *is* a six-column DDA, so BAND pays the DDA's work plus
classification plus replay. It could only win if bands repeated often within one
run, and with at most four chunks in a twenty-column run they do not.

That is a clean negative result for the prettiest idea in this investigation, and
it is exactly why the race was worth running before building it.

**PACKED replay is 6.1x to 8.9x faster than the DDA**, with identification free.
That gap is the real finding: it is the prize available to any scheme that can
name a stream cheaply — and naming is precisely where the sampled dictionary
died. The question is therefore not "which of the three kernels", it is whether
the generic machine can be arranged to fetch as cheaply as replay does.

### Footprint

| | code | RAM held between calls |
| --- | --- | --- |
| DDA | — | **0 bytes** |
| BAND | — | 65 bytes (8x2 edges + 8x6 programs + 1 valid mask) |
| PACKED | — | 0 bytes; 1,433 B of ROM for these 88 spans (1,169 move bytes + 264 B of index) |

Code size per kernel is **not cleanly separable** from these build artifacts: the
map file has no end markers for the static functions, and an empty translation
unit still emits 1,824 bytes because SDCC keeps them. All three together are
about 1.8 KB of SDCC C. Rather than report a number I cannot defend, that is
stated as unmeasured here.

### What this does and does not establish

These are **SDCC C**, like the renderer's own C paths, not hand assembly. All
three would improve, and PACKED would improve most — a byte-copy loop becomes
`LDIR`. So the ratios are, if anything, conservative in PACKED's favour and the
DDA's 2,200 T-states per column is far above what hand-written assembly achieves;
the shipped materializer is hand assembly for exactly this reason. What is being
tested is which **shape** of algorithm the Z80 prefers, and the answer is that
classification-plus-replay is not worth it at these span lengths.

`tools/race/build_and_run.sh` reproduces the whole thing.

## A framing correction, and the contract

Saying "the question is whether the generic machine can be arranged to fetch as
cheaply as replay" framed an already-decomposed problem as a fresh unknown. The
projection, depth-plane and cell-metadata work had already identified stream
identification as the central implementation problem and named the candidate
solution family. What the race added is a **measured value** for solving it, and
the elimination of one expensive way of doing it.

That is the same over-generalisation as "no compact yaw machine exists": moving
from *this representation failed* to *this architecture space is closed*. The
harnesses keep catching it. The distinction now has its own line in
`docs/RASTER_ARCHITECTURE_CONTRACT.md`, which freezes the shape, the five rules,
what is settled and what is open, so later work is measured against it.

It also records the separation the BAND result needs: **BAND as an execution
strategy is dead; band boundaries as mathematics are not.** An experiment can
lose the implementation race and still contribute the material the certificate
work needs.

## The race in hand assembly: the real selector budget

`tools/race/race_asm.s` adds hand-written Z80 versions of the two kernels the
decision rests on. Arguments arrive in globals rather than on the stack, so the
calling convention costs nothing on either side. Rows are biased by +64
throughout: they span -7..8, so the bias makes every comparison unsigned, which
the Z80 does in one `CP` instead of a sign/overflow dance per column. `PACKED`
becomes a single `LDIR`, which is the whole reason replay is hard to beat.

Both are validated by the same gate as the C kernels: the profiler prints nothing
until all five produce byte-identical output against the generated expectation.

| columns | DDA C | BAND C | PACKED C | **DDA asm** | **PACK asm** | **budget** |
| --- | --- | --- | --- | --- | --- | --- |
| 3 | 7,626 | 14,155 | 1,429 | **1,675** | **254** | **1,421** |
| 6 | 13,926 | 17,202 | 2,087 | **2,819** | **338** | **2,481** |
| 12 | 26,825 | 31,160 | 3,445 | **5,366** | **595** | **4,771** |
| 18 | 39,591 | 43,417 | 4,754 | **7,830** | **684** | **7,145** |

Per column the hand DDA costs 558 / 470 / 447 / 435 T-states against SDCC's
2,542 / 2,321 / 2,235 / 2,200 — a factor of **4.55 to 5.06**. My earlier guess
that C exaggerated the budget "by roughly an order of magnitude" was wrong; the
measured factor is about five.

The assembly `LDIR` replay costs about 27 T-states per emitted byte, which is
`LDIR`'s own 21 plus setup amortised. That the number lands where the instruction
timing says it should is a useful sign the measurement is real rather than an
artifact of the harness.

### What the budget means

**The selector budget is 1,421 T-states at three columns, 2,481 at six, 4,771 at
twelve and 7,145 at eighteen** — about 400 T-states per column. That is what a
scheme which *names* a stream must cost less than, to beat *deriving* it with the
hand DDA.

At six columns that is roughly 600 Z80 instructions. It is a large budget, not a
tight one, and it is the number the selector work should be priced against — not
the C figures, which are a comparison of algorithm shapes.

The hand DDA is still 6.6x to 11.4x slower than hand replay, so the gap the
earlier C run found survives implementation quality. It narrows from the C
ratio of 6.1x-8.9x only at the short end; at eighteen columns it widens.

## Selector key screening: no cheap key names the body

`tools/race/selector_keys.c`. Before writing Z80 for a selector, two host
questions decide whether one can exist: is a candidate key **sufficient** (every
state sharing it emits the same body), and how big is its table over the
**complete** domain — all 1024 phases against every step the shipped depth tables
can produce, 3,177,472 states, which overstates reachability in the safe
direction for a ROM claim.

Over that domain there are **175 distinct six-column behaviours**, so the map is
massively many-to-one. The question is whether it *factors* through anything
cheap.

### A. Quantised (phase, step)

| | phase>>0 | >>1 | >>2 | >>3 | >>4 | >>5 | >>6 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| step>>0 | 3,177,472 | — | — | — | — | — | — |
| step>>1..6 | — | — | — | — | — | — | — |

A dash is *not sufficient*. **Only the exact pair works**, at 3.2M entries. Even
`phase>>1` breaks it: two adjacent phases can emit different bodies, because band
edges can sit one apart.

### B. Structured keys

| key | entries | sufficient | what must be formed |
| --- | --- | --- | --- |
| step + band index | 24,755 | **yes** | 8 edges + 8 compares — BAND's own cost |
| edge-ordering family + band index | 150 | **no** | |
| edge-ordering family + full phase | 20,480 | **no** | |
| step high byte + full phase | 14,336 | **no** | |

### The reading

**The twenty edge-ordering families are not a sufficient selector dimension.**
The structural result was real — 3,103 steps do collapse to 20 orderings — and it
is operationally insufficient, which is exactly the caution that was raised
against it. An ordering says where the eight edges sit *relative to each other*;
it does not carry their *spacing*, and the spacing is what determines the
crossing pattern. `ordering + full phase` fails too, so this is not a matter of
pairing it with something finer.

The only sufficient keys are the exact pair (3.2M entries, ~6.4 MB of index) and
`step + band` (24,755 entries, ~49.5 KB) — and the second **also** costs BAND's
classification to form. It loses on both axes at once.

### Two corrections to how this was first stated

**The information-content claim was wrong.** I wrote that "the body's identity
carries essentially the full information content of `(phase, step)`, so naming it
cannot be cheaper than deriving it". That does not follow and is not true. There
are only **175 bodies**, so an identity fits in eight bits, while the input pair
spans 3,177,472 states. The mapping is enormously many-to-one and the output
retains almost none of the input's information.

What the screening actually shows is narrower, and is a statement about
**computational structure, not information**: the mapping from exact phase and
exact step onto the 175 bodies does not factor cleanly through any of the cheap
coarse keys tested so far. That is a real and useful result. It is not a proof
that lookup cannot beat derivation.

**The ROM figure was also wrong.** I quoted ~6.4 MB for the exact-state index by
assuming two bytes per entry. There is no reason to store a 16-bit pointer when
only 175 destinations exist: one byte of body ID per state plus a 175-entry table
from ID to descriptor. The flat exact-state table is therefore **~3.03 MiB**, not
~6.4 MB. Still an absurd production trade, but useful as a controlled speed
ceiling, and it scales over the intrinsic generic raster state rather than over
map position or camera pose -- which makes it categorically different from the
old pose dictionaries even at that size.

### What is actually closed, and what is not

**Closed:** the twenty-family direct selector, and naive coarse quantisation of
phase or step as a sufficient direct key.

**Not closed:** exact-state indexing, exact-step phase-interval lookup, and
coarse first-stage keys followed by one or two cheap residual tests. The
`step + band` result at 24,755 entries over 3,103 steps is an average of almost
exactly eight bands per step, and those boundaries depend only on the exact step
-- which means they can be **precomputed** rather than reconstructed at runtime.
That is the difference between BAND and an interval lookup, and it has not been
priced.

### Where that leaves the prize

Three routes survive, and none of them is a table selector:

1. **Make the derivation itself cheaper.** The hand DDA is 435-558 T-states per
   column. That is the thing to attack directly, not to route around.
2. **Find a decomposition where the body is generated rather than named** — the
   closed form already needs no table at all.
3. **Do not regenerate at all when nothing changed.** This is where the band
   boundary mathematics genuinely pays, and it is the one route the screening
   strengthens rather than weakens.

The contract's memory-scaling rules did their job here: every candidate that
could have looked attractive on speed alone was rejected on size or on key cost
before a line of Z80 was written for it.

## Selector census: the band distribution, and the ambiguity structure

`tools/race/selector_census.c`, plots in `build/selector-census.png`, raw data in
`build/selector/`.

### Bands per exact step: a spike, not an average

```
mean 7.98   median 8   p90 8   p95 8   p99 8   MAX 8
histogram   1 band: 3 steps   2 bands: 4   4 bands: 6   8 bands: 3,090
phases with no in-family body: 0 of 3,177,472
```

This is the best possible shape for the interval hypothesis. "About eight" is not
an average hiding pathology: **no step anywhere in the domain has more than
eight** merged contiguous phase bands, and 3,090 of 3,103 have exactly eight. The
thirteen exceptions are degenerate steps at round values, visible in the plot as
a regular comb.

A selector that searches an exact step's stored intervals therefore has a **hard
worst case of eight comparisons**, not a distribution with a tail.

Storage if every step keeps its own record: 24,755 bands. At two bytes of
threshold plus one of body ID that is ~74 KB; packing the ten-bit thresholds
brings it to ~56 KB. The point of storing them is precisely that BAND's cost was
*constructing* these boundaries at runtime, and they depend only on the exact
step, so they can be built offline.

### Body occupancy is flat

Cumulative share of states: top 1 body 4.7%, top 5 17.0%, top 10 23.3%, top 25
37.0%, top 50 55.5%, top 100 82.6%. **No small hot subset dominates**, so
special-case fast paths for a handful of bodies will not buy anything, and an
expected-cost selector has little skew to exploit.

### Ambiguity structure: three keys are properly dead, one is not

Sufficiency alone was the wrong test, so each failed key was measured for how
badly it fails, and whether one cheap Z80 bit test rescues it. Permitted residual
predicates: the ten phase bits, the eight low step bits, and the step sign — all
single `BIT` instructions.

| key | key values | unique keys | unique states | mean candidates | max | +1 bit |
| --- | --- | --- | --- | --- | --- | --- |
| edge-ordering family + band | 150 | 0 | 0.00% | 3.23 | 4 | → 0.00% |
| edge-ordering family + full phase | 20,480 | 0 | 0.00% | 7.58 | 24 | → 0.00% |
| step high byte + full phase | 14,336 | 0 | 0.00% | 7.28 | 11 | → 0.07% |
| **step >> 4 + full phase** | 208,896 | 134,905 | **64.24%** | 1.40 | 7 | → **66.60%** |

The first three are not merely insufficient, they are **thoroughly** insufficient:
not one key value resolves to a single body, and a cheap bit adds essentially
nothing. The edge-ordering family is confirmed dead in every form tested.

The fourth is genuinely different — 64% of states are already unique — but it
needs 208,896 key values, which is *worse* than the exact-step interval records
on size, and a cheap bit only reaches 66.6%, leaving a third of states needing
more. On present evidence the hybrid route looks weaker than the interval route,
not stronger.

### Cross-step structure

The 120-step crop of the body map shows clear **diagonal wedges**: boundaries
move smoothly as step changes, and neighbouring steps share body sequences over
long runs. That is the structure cross-step compression would exploit, and it
supports censusing it — **after** the uncompressed interval selector has been
priced, not before.

### Status, worded correctly

Not "selector key screening — done, answer is no". Rather:

> **Simple selector factorisation screening — done.** Edge-family and tested
> coarse direct keys rejected. **Exact-state lookup, exact-step phase-interval
> lookup, and coarse-key-plus-cheap-residual selectors remain to be priced before
> selector work is closed.**

## Rung 9 — the selector ladder priced on real Z80 timing

Everything before this was about whether a selector *could* exist. This is the
first measurement of what one *costs*, on the emulator's cycle counter, with the
correctness gate still refusing to release timings unless every kernel emits
byte-identical move streams. Eleven kernels now run per case and all eleven
agree, across 22 steps and span lengths 3, 6, 12 and 18.

Tools: `tools/race/gen_selector_tables.c` (bakes the tables and does the ROM
accounting), `tools/race/race_asm.s` (the hand-written kernels),
`tools/race/plot_ladder.py`. Output: `build/race/ladder.txt`,
`build/race/selector-tables.txt`, `build/selector-ladder.png`.

### What each rung of the ladder is

A **span body** is one of the 146 distinct six-column raster behaviours for the
descending-row family. (The earlier figure of 175 counted a signature over both
row families; a family-zero replay does not need that distinction.) A **selector**
is whatever turns live renderer state into the address of one.

- **A0** — the ideal oracle. A dense one-byte-per-state table indexed by step
  ordinal and the exact ten-bit phase, with the ordinal *supplied free*. Not a
  proposal: it is the lower bound any naming scheme is racing.
- **A1** — the same table, paying the real step-to-ordinal conversion and address
  formation. A0 and A1 bracket what "just look it up" costs.
- **B** — the exact-step phase-interval selector, in three forms: eight fixed
  slots scanned linearly, eight fixed slots searched in three compares, and the
  packed variable-length record scanned linearly.

Every one of them is charged its whole path: bank selection, address formation,
the search, the body fetch, the replay, and chunk chaining with the span length
supplied from outside. All share one replay, so the difference between any two is
the cost of *finding* the body and nothing else.

### The replay is an LDIR, which changes the arithmetic

The packed-replay comparator had already shown that copying a stored byte stream
is the floor for emitting moves — the Z80 copies a byte per 21 T-states with no
loop overhead. So the body is now stored **as its move stream**, not as a
six-byte descriptor to be expanded, and a chunk's replay is one `LDIR`.

The run terminator is the only complication: the last column emits the terminator
instead of its jump byte. Six prefix lengths are baked per body — the byte count
up to the end of each column's downs — so a chunk that ends the run copies a
prefix and writes the terminator, and a chunk that does not copies the whole
stream. Nothing is counted at run time. For 146 bodies that is 1,612 bytes of
stream, 292 of pointers and 1,168 of prefixes: small enough to live in the fixed
bank, so no selector pays a bank switch to reach it.

### Results, T-states per span

| columns | hand DDA | packed floor | A0 oracle | A1 honest | B fixed linear | B fixed balanced | B packed linear |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 3 | 1,671 | 313 | 1,114 | 1,357 | 1,546 | 1,583 | 1,381 |
| 6 | 2,926 | 425 | 1,226 | 1,496 | 1,574 | 1,639 | 1,381 |
| 12 | 5,362 | 571 | 1,860 | 2,103 | 2,642 | 2,735 | 2,458 |
| 18 | 7,603 | 743 | 2,550 | 2,849 | 3,675 | 4,104 | 3,444 |

Speedup against the hand-written DDA, which is the thing to beat:

| columns | A0 | A1 | B fixed linear | B fixed balanced | B packed linear |
| --- | --- | --- | --- | --- | --- |
| 3 | 1.50x | 1.23x | 1.08x | 1.06x | 1.21x |
| 6 | 2.39x | 1.96x | 1.86x | 1.79x | **2.12x** |
| 12 | 2.88x | 2.55x | 2.03x | 1.96x | **2.18x** |
| 18 | 2.98x | 2.67x | 2.07x | 1.85x | **2.21x** |

**Every selector beats deriving the raster.** That is the headline, and it is the
first time in this investigation that a table has beaten the generic machine on
the hardware rather than on paper. It reverses the reading that "no cheap key
names the body" implied about the architecture: no cheap key does, but the
*expensive* key — the exact step and the exact phase — is affordable after all,
because the search over eight intervals is short and the replay is a copy.

The three-column case is the exception, at 1.06x–1.21x. A three-column span is
one short chunk, so the per-span setup is amortised over almost nothing. That is
a real limitation, not noise: very short spans are close to a wash.

### Balanced search loses to linear scan

This was worth measuring rather than assuming, and the assumption would have been
wrong. The balanced search is **slower at every length**, and its disadvantage
*grows* with span length (1.79x vs 1.86x at six columns, 1.85x vs 2.07x at
eighteen). Three compares beat an average of 4.8 only if a compare is cheap, and
on the Z80 it is not: each one is a subroutine call, a 16-bit offset load and an
`add hl,de`, so the constant-time search pays about as much per compare as the
linear scan pays per *slot*, and then pays `push de` / `pop de` on top to free a
register pair it does not otherwise need.

The linear scan also gets a structural gift the balanced search cannot use: slot
zero's threshold is always zero and the phase is never negative, so the walk
terminates by construction and needs no counter, no bound and no index at all.

### Fixed eight-slot versus packed variable-length records

Both layouts store the same slot — two threshold bytes and one body byte — so the
comparison is like for like. They differ only in how a step's record is found and
how the scan knows where to stop.

| | FIXED (8 slots always) | PACKED (n slots) |
| --- | --- | --- |
| records | 3,103 × 24 B = 74,472 B | 23,210 × 3 B = 69,630 B |
| bank padding | 64 B | 58 B |
| pointer table | none (base from the ordinal) | 6,206 B |
| **total** | **74,536 B (72.8 KiB), 5 banks** | 75,894 B (74.1 KiB), 6 banks |
| scan length | 4.763 slots/lookup | 4.243 slots/lookup |
| measured speed | 2.07x | **2.21x** |

**The fixed layout wins on ROM and loses on cycles, which is the opposite of the
usual expectation both ways round.**

It wins on ROM because the band count is *nearly* saturated. Padding every step
to eight wastes 1,614 slots, 6.5% — but the two-byte pointer that a
variable-length record needs to be found at all costs 6,206 bytes, which is more
than the padding it saves. Packing here buys nothing and costs a pointer table.

It loses on cycles for two reasons, both measurable:

1. **The padding is not free at run time.** A descending scan that starts at slot
   seven steps over the padding before it reaches any real threshold. Weighted by
   phase across the whole domain, fixed examines 0.520 more slots per lookup than
   packed does. The thirteen degenerate steps pay up to 7 extra.
2. **Twenty-four is not a power of two.** This is where the original argument for
   fixed-size records — waste a little ROM to get predictable addressing — does
   not cash out. Three bytes a slot makes the record stride 24, so forming the
   base is five shifts and adds plus a second base pointer, roughly 70 T-states
   more per span than packed's single pointer load. The addressing advantage that
   motivated the whole layout is absent *because the slot is three bytes wide*.

A split layout — sixteen bytes of thresholds and eight of body ids, both true
shifts — would restore it, at the cost of turning the scan's hit back into an
index that has to be converted before the body can be fetched. That variant is
**not measured** and should not be assumed either way.

Where the fixed layout's predictability does pay off unambiguously is banking.
A0's rows are 1,024 bytes and 1,024 divides a 16 KB bank exactly, so a step's row
never straddles one and the bank is the ordinal's high nibble. That is the
fixed-size argument working as advertised, one level up from the slot.

### Cartridge banking

At 72.8 KiB the interval records need five 16 KB banks, plus one for the
step-to-ordinal map (256 B of page index and 7,168 B of pages — only 14 of 256
step high bytes are reachable) and the small shared body tables in the fixed
bank. Six banks of a 512 KiB cartridge. The step does not change inside a span,
so the bank switch is hoisted to span setup and costs one `ld (0xFFFF),a` per
span, not per chunk — that placement is charged in every measurement above.

The earlier "~56 KB with ten-bit thresholds" figure should be read with two
corrections: it counted 24,755 bands over a signature spanning both row families,
where a family-zero replay needs 23,210; and bit-packing ten-bit thresholds would
make every comparison a bit-extraction, which on the Z80 costs far more than the
12 KiB it saves. The plain byte-aligned layouts above are the ones worth pricing.

### The thirteen exceptional steps

The steps with fewer than seven bands are exactly the multiples of 256:

| bands | steps |
| --- | --- |
| 1 | 0, ±1024 |
| 2 | ±512, ±1536 |
| 4 | ±256, ±768, ±1280 |

This is the sanity check the census was missing. Band edges sit at
`(-c·step) mod 1024` for c in 0..7, so a step of 1024k collapses all eight edges
onto phase 0 (one band), 512×odd onto {0, 512} (two), and 256×odd onto
{0, 256, 512, 768} (four). The exceptions are not anomalies to be explained away;
they are the direct arithmetic consequence of the eight-edge structure, and their
appearing exactly where the theory says they must is a check on the whole band
derivation.

Note also that family-zero merging changes the distribution the census reported:
3 steps with one band, 4 with two, 6 with four, **1,545 with seven and 1,545 with
eight**. The count is not saturated at eight. Half the steps have one adjacent
pair of bands that resolve to the same family-zero body and therefore merge.

### What this does and does not settle

Settled by measurement: an exact-step phase-interval selector is **real**. It
beats the hand-written DDA by 1.9x–2.2x from six columns up, at 73 KB of ROM and
six cartridge banks, with identical raster output as the gate.

Bounded by measurement: the exact-state oracle, which is the cheapest any naming
scheme can possibly be, runs at 2.4x–3.0x. So **at most another 35% is available
to any cleverer selector**, and A1 shows that about half of A0's margin
evaporates as soon as the step-to-ordinal conversion is paid for honestly.

Not settled: rung **C** — the strongest coarse keys plus automatically searched
depth-1..3 residual decision trees — has not been priced. The census bounds it
but does not close it: of the four coarse keys measured, three resolve *no* state
uniquely, and one cheap bit adds at most 0.07%. For the key with the tightest
residual (edge-ordering family + band, at most 4 candidates) a two-bit residual is
information-theoretically possible, but that key requires computing the band,
which is the same interval search B already does — so it is circular unless a
cheaper band classifier turns up. C was worth running to shrink the 73 KB; with B
measured and working, the case for it is weaker, and the ceiling above bounds
what it could win. It remains open and is explicitly *not* claimed dead.

Also not done: integrating the winner into the renderer and re-profiling a
complete update, which is where end-to-end accounting finally applies.

## Rung 10 — validation at full domain, and what the real span distribution costs

Two things happen here, and the second one revises the first's headline.

Tools: `tools/race/selector_exhaustive.c`, `tools/race/stress_kernels.c` plus
`stress_asm.s` and `stress_run.cpp` (driven by `tools/race/build_and_stress.sh`),
`tools/race/span_census.c`, `tools/race/project_end_to_end.py`,
`tools/race/plot_weighted.py`. Output: `build/stress/`, `build/race/per_length.csv`,
`build/race/span_weights.csv`, `build/selector-weighted.png`.

### Full-domain tables, and a correction the race could not see

The race held all 22 of its steps in a single cartridge bank, so its pointer was
a two-byte offset and its bank was a constant. At full domain the records occupy
five banks, and that changes the layout arithmetic:

- **PACKED** now needs three bytes per step, not two: fourteen bits of in-bank
  address plus a bank index does not fit in sixteen. The step map and the pointer
  table are therefore merged — one three-byte entry gives bank and address
  directly — costing 14 pages × 768 B = 10,752 B.
- **FIXED as raced** does not survive banking. `ordinal × 24` overflows sixteen
  bits past ordinal 2,730, and 24 does not divide 16,384, so the bank cannot be a
  shift of the ordinal at all. It needs a division, a per-step pointer (which
  discards the entire reason for the layout), or power-of-two bank packing that
  wastes 4 KB a bank.

  This rules out the **interleaved 24-byte** variant, not the fixed family.
  **Splitting into 16-byte threshold and 8-byte body arrays restores exactly what
  the fixed layout was supposed to buy** — 1,024 and 2,048 records a bank, both
  powers of two, so bank and offset are pure shifts of the ordinal with no waste
  and no straddle. The cost is that the scan's hit becomes an index that has to
  be converted before the body can be fetched, which is per chunk rather than per
  span. That variant is **unmeasured**, and it is deliberately left unmeasured:
  packed is proven and works under banking, so it is what integration carries. If
  a later profile says selector addressing is still a hotspot, split-fixed is the
  first design to resurrect, and it should not be treated as dead in the meantime.

Production totals: 69,630 B of records plus 58 B of boundary padding in five
banks, 10,752 B of step map in one, and the body stream, pointer and prefix
tables in the fixed bank. A span pays two bank switches and one three-byte read,
both per span rather than per chunk because the step does not change inside one.

### Exhaustive host validation

The reference is deliberately *not* the race kernels — it is the closed-form
column rule written out in wide integers, so a shared 16-bit mistake cannot hide
inside both sides of the comparison.

| sweep | span evaluations | mismatches |
| --- | --- | --- |
| every step × every phase × twelve span lengths | 38,129,664 | 0 |
| translation invariance, four accumulator offsets | 3,649,128 | 0 |
| the thirteen exceptional multiples of 256, every phase, lengths 1..24 | 319,488 | 0 |
| **total** | **42,098,280** | **0** |

Translation invariance is the premise the whole selector rests on: the body is
looked up by phase alone, so the same phase at different accumulator offsets must
produce the same moves. If that were false the selector would be wrong in a way
no single-offset sweep could detect.

**Mutation control.** A sweep that cannot fail proves nothing, so three realistic
defects were injected one at a time and all three were caught: a threshold
compare that loses the equal case (8,927 of 759,240 cases), a terminal chunk
using the previous column's prefix (303,696), and a map entry whose bank number
loses its top bit (43,872). The first one being rare is the point — an equal-case
bug only shows when the phase lands exactly on a band edge, which is precisely
why the on-device sweep tests every edge rather than random phases.

### On-device stress sweep

The host sweep cannot test what only exists on the cartridge: five record banks
reached through a real Frame-2 mapper write, a three-byte map entry carrying a
bank number, and the hand-written Z80 scan rather than a C transcription of it.

All 3,103 steps, every one of the eight band edges at the edge, one below and one
above, at five span lengths, against the hand-written DDA:

> **323,974 cases compared, byte-identical. 48,386 skipped** because the span
> cannot be placed inside the renderer's eight-bit depth domain at any
> accumulator congruent to that phase.

Phases are adversarial rather than random on purpose: a scan over sorted
thresholds fails at a boundary or not at all.

### The span distribution, and the headline correction

The race timed 3, 6, 12 and 18 columns because they are tidy multiples of the
six-column chunk. That was a bad sample. Censusing 238,592 poses — every walkable
map cell at four sub-cell offsets, yaw swept by 2 — gives 1,027,997 spans:

- **mean span is 6.80 columns**, 4.31 spans per pose
- the distribution peaks at **three to five columns**, not twelve or eighteen
- **27.33% of spans are three columns or fewer**, though they carry only 8.55% of
  the column work
- 20-column spans (the full screen) are 3.11% of spans but 9.14% of columns
- **99.12% of spans take the depth-plane path** the selector needs; the rest fall
  back because the wall plane crosses zero inside the run

So the race was re-run at **every length from 1 to 20** rather than interpolating,
because the selector's cost is a step function in chunks while the DDA's is
linear in columns — interpolation between sampled lengths would have been wrong
in a structured way.

| cols | hand DDA | B packed | speedup | | cols | hand DDA | B packed | speedup |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 856 | 1,271 | **0.67x** | | 11 | 4,972 | 2,414 | 2.06x |
| 2 | 1,231 | 1,524 | **0.81x** | | 12 | 5,231 | 2,321 | 2.25x |
| 3 | 1,612 | 1,318 | 1.22x | | 13 | 5,846 | 3,352 | 1.74x |
| 6 | 2,937 | 1,463 | 2.01x | | 18 | 7,874 | 3,370 | 2.34x |
| 7 | 3,434 | 2,417 | 1.42x | | 20 | 8,807 | 4,258 | 2.07x |

The sawtooth is the chunk boundary: a seven-column span pays two full chunk
lookups for one extra column of work. **The selector loses outright on one- and
two-column spans**, and so does the ideal oracle, so that is not selector
inefficiency — it is the per-span setup having nothing to amortise against.

Weighted by the measured distribution:

| kernel | T-states/span | per update | vs DDA |
| --- | --- | --- | --- |
| hand DDA | 3,279 | 14,132 | 1.00x |
| **B packed, linear** | **1,951** | **8,407** | **1.68x** |
| B fixed, linear | 2,140 | 9,225 | 1.53x |
| A1 honest oracle | 1,782 | 7,680 | 1.84x |
| A0 ideal oracle | 1,525 | 6,571 | 2.15x |
| packed replay floor | 442 | 1,904 | 7.42x |

**The rung 9 headline of 2.1x–2.2x was measured on spans longer than the renderer
mostly produces.** Against the real distribution the packed interval selector is
**1.68x**, and the absolute ceiling — the ideal oracle, ordinal free — is 2.15x,
not 2.98x. Both numbers are lower than rung 9 implied and neither changes the
conclusion that the selector works; they change what it is worth.

### Short-span specialisation is not worth building

The obvious response to a negative result at one and two columns is to dispatch
on span length and use the DDA there. Projected with the measured per-length
costs and a generous 25 T-states charged for the test:

> 1.68x becomes **1.71x**. The ideal oracle's hybrid moves 2.15x to 2.14x.

Short spans are numerous but cheap — 27% of spans, 8.5% of the column work — so
specialising them recovers almost nothing. This was worth measuring rather than
assuming in either direction, and the answer is: leave it alone.

### Status

Correctness is closed as far as it can be closed short of integration: 42 million
host evaluations against an independent reference, three injected defects all
caught, and 323,974 on-device cases through the real cartridge path. The number to
carry forward is **1.68x on the raster kernel, weighted by real spans**, with a
2.15x ceiling.

What remains is integration and a real profile — and the gap between 1.68x
projected and whatever integration measures is itself the interesting quantity,
because it is where span scheduling, continuation state and bank interactions
with the surrounding renderer show up.

## Rung 11 — the projection with the modelling taken out

Rung 10's 1.68x weighted per-length race timings by the span-length histogram.
That corrects for span length and leaves a second bias untouched: within a
length, the race sampled `(step, phase)` from 21 hand-chosen steps at centred
accumulators. Both kernels care about more than length — the linear scan's cost
depends on which of the eight phase bands the span starts in, and the replay's on
how many move bytes the body expands to — so a length-weighted figure is still a
model.

Tools: `tools/race/gen_corpus.py`, `tools/race/corpus_kernels.c`,
`tools/race/corpus_run.cpp`, driven by `tools/race/build_and_corpus.sh`. The pose
corpus now dumps the actual `(iq, step, columns)` tuples the raster walker is
handed, a uniform seeded sample is baked into the cartridge, and the ROM replays
exactly those through both kernels.

### The clamp, which has to be dealt with before integration

`tilesector_polar_renderer.c:815` clamps the inverse depth to 0..255 per column
before deriving the row. **The raced DDA and the baked bodies both implement the
unclamped rule**, so wherever a column clamps, the selector and the shipping
raster are different functions — not slower or faster, different.

| | spans | share of accepted |
| --- | --- | --- |
| inside the unclamped domain, selector-eligible | 926,762 | 90.95% |
| touch the clamp on at least one column | 92,203 | 9.05% |

Selector-eligible share of *all* spans: **90.15%** (the plane test rejects 0.88%
on top). Detecting it is two compares on the span endpoints, because the step has
a fixed sign within a span, so routing those spans to the existing path is cheap —
but it is mandatory, not optional, and it was invisible until the real tuples
were extracted.

### Measured on the renderer's own workload

8,000 tuples drawn uniformly (seed 20260918) from the 926,762 eligible ones; the
largest span-length share discrepancy between sample and corpus is 0.55
percentage points. Both kernels byte-identical on all 8,000 — which is also a
third independent correctness sweep, this time on the real workload.

| | T-states per span |
| --- | --- |
| hand DDA | 2,763.9 |
| B packed, linear | 1,912.3 |
| **speedup** | **1.445x** |
| saving | 851.6 T-states per span, 30.8% of the DDA's cost |

### Decomposition: where 1.68x went

| | speedup | effect |
| --- | --- | --- |
| all spans, length-weighted (rung 10) | 1.681x | — |
| eligible spans only, length-weighted | 1.634x | population −0.047 |
| eligible spans, real tuples **measured** | **1.445x** | joint distribution −0.189 |

So most of the correction is the joint `(step, phase)` distribution, not the
population change. The mechanism is measurable rather than inferred: **at matched
span lengths the race's synthetic cases emit more move bytes than real spans do**,
and the gap widens with length — 26.9 against 23.5 at twenty columns, 16.2
against 15.2 at twelve. Every extra move byte costs the DDA a whole loop
iteration but costs the selector only an `LDIR` byte, so steeper synthetic
geometry inflated the DDA more than it inflated the selector. That accounts for
the DDA side, which measured 8.5% cheaper than projected. The selector's residual
(3.5% dearer than projected) is smaller and has **not** been isolated; scan
position is the likely cause but that is not established here.

### What to quote

**1.445x on the raster kernel, over 90.15% of spans**, saving 851.6 T-states per
eligible span. At 4.31 spans a pose that is roughly **3,300 T-states saved per
update** — but the fraction of the whole raster subsystem that represents depends
on what the ineligible 9.85% cost, which is not measured here. Integration is
what settles that.

The lesson is worth recording separately, because it generalises: **a synthetic
benchmark corpus chosen for tidiness will systematically mis-rank two kernels
whose costs scale with different quantities.** The race's cases were picked to be
well-spread in step and centred in the depth domain, which made them steeper than
real geometry; the DDA pays per move and the selector pays per chunk, so the
tidiness landed asymmetrically. The fix is not a better synthetic corpus — it is
replaying the real one.

## Rung 12 — moving up a level, and what it says about the selector

The instruction was to integrate the packed selector conservatively and then
measure the whole loop. Measuring first changed the answer, so this rung reports
the measurement and the integration blocker it exposed, and does **not** contain
an integration.

Tools: `tools/frame/frame_timeline.cpp`, `tools/frame/gen_traces.py`,
`tools/frame/run_timeline.sh`, plus a `TSPF_TRACE_INPUT` build option in
`src/main_tilesector_polar_gg.c` and a `POLAR_EXTRA_OBJS` hook in the Makefile so
a measurement build can link a baked input trace without the shipping object list
knowing about it. Output: `build/frame/<trace>.csv`.

### The frame timeline

Deterministic scripted input (`cruise`), 120 loop iterations, warmup discarded.
A Game Gear NTSC frame is 59,739 T-states at 60 Hz.

| whole loop | mean | p50 | p95 | p99 | worst |
| --- | --- | --- | --- | --- | --- |
| T-states | 353,919 | 477,890 | 595,277 | 598,597 | 601,307 |

That is **5.92 sixty-hertz frames on average and 10.07 at the worst** — the loop
runs at roughly 7 Hz. 65.8% of iterations exceed even the 30 Hz budget. The
distribution is strongly bimodal: p25 is about 59,700 (one video frame) while p50
is 477,890, because a third of the iterations are poses where the recipe grid
selects almost nothing. That bimodality is itself worth knowing — a mean is
close to meaningless here.

| loop stage | mean | share |
| --- | --- | --- |
| render | 304,282 | 85.97% |
| vsync | 33,285 | 9.40% |
| input/motion | 8,745 | 2.47% |
| VRAM upload | 7,539 | 2.13% |

| inside render | mean | share of loop |
| --- | --- | --- |
| **materializer** | 146,807 | **41.48%** |
| geometry walk | 31,496 | 8.90% |
| nametable/VRAM | 15,562 | 4.40% |
| arith helpers | 14,701 | 4.15% |
| unattributed | 95,715 | 27.04% |

Cross-checked against the existing PC-range function profiler on the same ROM,
which agrees: materializer ≈43.3% of render (`tsp_polar_p_fill` alone 21.8%),
geometry walk ≈14.9%, projection/setup ≈20%. The 27% my harness leaves
unattributed is a limitation of its symbol grouping, not a disagreement — the
function profiler resolves it into `project_key`, `screen_depth_plane`,
`bearing_*` and friends.

### Why the selector cannot be integrated conservatively

The race compared two ways of producing a **move stream** — down a tile row,
next column, next column up N. The shipping renderer does not contain that
function. `tsp_polar_run_geometry_fast` computes, per column, two clamped
inverse depths and four **pixel** endpoints, and hands them to
`tsp_polar_surface_column_fast`, which floors them to tile rows itself.

So the selector's body vocabulary is strictly coarser than what the renderer
consumes. There is no drop-in. A body that *could* drive this renderer would have
to store per-column deltas of `half = (iq + 32) >> 7` — eight times finer than a
tile-row move — and the band edges multiply accordingly: roughly 48 per step
instead of 8, so about 450 KB of records rather than 73 KB. That is a new design,
not an integration.

Two further consequences worth stating plainly:

1. **The race's baseline was not the renderer's.** The geometry step costs
   55,817 T per update over 44.52 columns, which is **1,254 T-states a column**.
   The hand-written DDA the selector beat costs about 447 T a column. The race
   measured the selector against something 2.8x cheaper than the real thing, so
   the 1.445x does not transfer in either direction without redoing it.
2. **The ceiling is small regardless.** Even if a correctly-shaped table made the
   per-column geometry free, it removes 11.5% of render time. The materializer is
   43%.

### What to attack next

The dominant cost centre is the **materializer**, and inside it `tsp_polar_p_fill`
at 21.8% of render on its own, called 146.55 times an update. Second is
projection and setup at ~20%, where `project_key` (6.95%) and
`screen_depth_plane` (5.13%) lead. The raster geometry walk the entire selector
line was aimed at is third, at ~15%, and the selector could address at most
three-quarters of that.

On the measured evidence the selector work should stop where it is: correct,
validated, documented, and not worth integrating against a stage that is 11.5% of
render when a 43% stage is sitting next to it.

### Not done

- **No integration.** Blocked as above; building it would have been a
  body-vocabulary redesign, not the conservative drop-in that was asked for.
- **No old-versus-new comparison**, because there is no new renderer to compare.
- **Two new maps not built.** The world is a 16-vertex, 32-segment format
  (`k_tspf_keys` packs sid:5, v0:4, v1:4) with a 48x24 potentially-visible-set
  grid and hand-written walkable rectangles in `tsp_is_walkable_q4`. Making an
  open-and-coarse map and a dense-corridor map means writing a level compiler for
  that format — vertices, segment normals, keys, the PVS grid, the sub-cell
  selector predicates and walkability. That is a self-contained piece of work and
  it is the right next one, because the materializer's cost scales with drawn
  area and the projection's with visible key count, so the two maps will move
  those two stages in opposite directions and tell us which dominates in which
  kind of level.
- Only the `cruise` trace was run. `spin`, `corners` and `stress` are generated
  and buildable but not yet measured.

## Rung 13 — materializer census: the mask is not the problem

Step one of the materializer ladder, run before touching any code. Tools:
`tools/frame/mat_census.cpp`, `tools/frame/run_mat_census.sh`, plus ten read-only
alias labels in `src/tilesector_polar_materialize_gg.s`. Those aliases are labels
on existing storage — no instruction is added, moved or changed — so the build
measured is cycle-identical to the shipping one. Probes sample at the exported
`_tsp_polar_p_fill` and `_tsp_polar_p_span` labels; zero ROM instrumentation.

### Where p_fill iterations actually go

| trace | iterations/update | already claimed | visible, already correct | visible and changes |
| --- | --- | --- | --- | --- |
| cruise | 170.6 | 9.22% | **82.29%** | 8.49% |
| spin | 249.7 | 0.36% | **92.48%** | 7.16% |
| corners | 191.9 | 5.06% | **88.38%** | 6.56% |
| stress | 270.9 | 1.27% | **94.03%** | 4.70% |

**The hypothesis that most fill iterations are hidden behind nearer geometry is
wrong.** Occlusion rejects between 0.36% and 9.22%. The overwhelming majority —
82% to 94% — are rows that are visible, that the loop happily materializes, and
whose name-table word already contains exactly the right value. The Z80 is
spending its time *proving that the picture has not changed*.

The interior-mask prediction was right in shape and wrong in consequence:

| trace | interior wholly unclaimed | partially occluded | wholly occluded | unclaimed runs per interior |
| --- | --- | --- | --- | --- |
| cruise | 82.19% | 10.98% | 6.83% | 0.93 |
| spin | 98.62% | 0.89% | 0.49% | 1.00 |
| corners | 83.69% | 9.91% | 6.40% | 0.94 |
| stress | 95.27% | 3.81% | 0.92% | 1.00 |

"Whole interior visible" is indeed extremely common (82–99%), and the unclaimed
rows form essentially one contiguous run (0.93–1.00 per interior), so a mask-first
enumeration is trivially expressible. But it is not worth much *as an occlusion
filter*, because there is almost nothing to reject.

It is still worth doing, for a different reason: the loop calls
`polar_row_unclaimed_fast$` **once per interior row**, 170–271 times an update,
and mask-first would call it once per span, 22–43 times. The saving is the
ownership query on every iteration, not the 0.4–9% that get rejected. That is a
real and safe win, and the census reframes why.

### Temporal identity, and how the key changes the answer

| trace | keyed on raw pixel endpoints | keyed on the materialized result |
| --- | --- | --- |
| cruise | 24.36% | **35.31%** |
| spin | 10.82% | **46.96%** |
| corners | 20.83% | **38.27%** |
| stress | 17.81% | **41.89%** |

Keying a column descriptor on raw pixel endpoints roughly halves the hit rate
against keying it on what the materializer actually produces — the tile rows and
the tile word. A one-pixel wobble in `top_l` changes the endpoints and changes no
tile at all. **Any temporal reuse must be keyed on the quantised result, not on
the geometry that produced it.**

Even so, 35–47% of columns is well short of the 82–94% of *cells* that are
unchanged, and the reason is granularity: a column whose single top row moved
fails the whole-column test while seventeen of its eighteen cells are untouched.
Column-level reuse captures roughly half of the available redundancy.

### Row-major transpose: supported, but not by much

| trace | mean same-tile run | share of runs that are a single cell |
| --- | --- | --- |
| cruise | 2.80 | 62.5% |
| corners | 3.31 | 58.2% |
| spin | 4.58 | 59.5% |
| stress | 5.18 | 61.1% |

The distribution is bimodal: most runs are one cell (wall edges and the boundary
between surfaces), and a small tail of full-width 20-cell runs (2.6–9.8% of runs,
but about a fifth of all cells) which are the ceiling and floor base rows rather
than wall interiors. Horizontal structure is real but it is concentrated in the
background, not in the thing p_fill is drawing. **The transpose is not yet
justified by the data** and should stay behind the two cheaper changes.

### The Amdahl check, restated

The materializer is ~146,807 T of a ~353,919 T mean loop. Free materializer
leaves ~207,000 T, about 17.3 updates a second against a 20 Hz budget of
~179,000 T. Materializer work alone cannot finish the job, which argues for
changes that produce reusable identities rather than merely faster loops.

### Recommended order, on the evidence

1. **Mask-first interior enumeration.** Safe, semantics-preserving, touches 100%
   of iterations by removing the per-row ownership query. Justified — but by the
   query cost, not by occlusion.
2. **Temporal reuse keyed on the quantised column result.** 35–47% of columns,
   and the census says exactly which key to use. This is the one that also buys
   skips *above* the materializer.
3. **Row-major transpose.** Not justified yet. Revisit if 1 and 2 leave the
   materializer dominant, or if a denser map moves the run-length distribution.

Not done: the two new maps, which remain the outstanding piece and would move the
run-length and interior-height distributions that decide item 3.

## Rung 14 — mask-first interior fill: 5.8% to 16.5% off the whole loop

Item one of the materializer ladder, implemented exactly as the census justified
it and gated on byte-identical output.

### The change

`polar_mark_span_fast$` already computes, per byte of the 18-bit coverage mask,
both the span this surface wants and the rows nearer geometry already owned. It
now also records whether *any* row was already owned, in one flag byte, at a cost
of three instructions per mask byte.

`draw_plain_interior$` branches on that flag. When it is clear — which the census
said is 82% to 99% of spans — the interior runs a new loop that is the same
store, compare and dirty-mark sequence with the per-row ownership query removed.
When it is set, the original loop runs unchanged.

That is the whole change. The old loop is untouched, so the occluded case cannot
regress, and the new loop differs from it by exactly one removed call.

### Output equivalence, first

Every measurement below was withheld until the name table matched. A digest of
all 720 bytes of `g_map` is taken after each loop iteration and the digest stream
compared against a baseline captured before the change:

> cruise, spin, corners, stress — **name table identical over 60 frames each**.

### What moved

Paired, same 100 loop iterations, same traces, same build flags.

| trace | loop mean before | after | change | materializer before | after | change |
| --- | --- | --- | --- | --- | --- | --- |
| cruise | 412,756 | 388,861 | **−5.79%** | 176,168 | 154,075 | −12.5% |
| corners | 394,835 | 357,201 | **−9.53%** | 169,541 | 137,526 | −18.9% |
| spin | 364,416 | 309,459 | **−15.08%** | 171,949 | 125,705 | −26.9% |
| stress | 379,193 | 316,470 | **−16.54%** | 177,780 | 124,883 | −29.8% |

The spread is not noise and it is not luck: it tracks the census exactly. The
traces with the highest "interior wholly unclaimed" rate — stress at 95.27% and
spin at 98.62% — take the largest win, and cruise at 82.19% takes the smallest.
The census predicted the ordering before the code existed.

Iterations still reaching the old loop, measured on the same probe:

| trace | before | after | share moved to the open path |
| --- | --- | --- | --- |
| cruise | 170.6 a update | 41.7 | 75.6% |
| spin | 249.7 a update | 2.1 | 99.2% |

### Caveats worth keeping

The tail moves much less than the mean: cruise p95 597,026 → 544,357 (−8.8%),
spin p95 367,342 → 364,548 (−0.8%). Worst-case frames are dominated by something
other than the ownership query, so this change does not help the spikes that
decide whether a frame budget is met.

And the Amdahl position is unchanged in kind. The loop still runs at roughly 9 to
11 updates a second against a 20 Hz target. This is a real 6–17% for a change
that adds three instructions per mask byte and removes one call per interior row,
but it is not the thing that closes the gap.

### Next

Item two of the ladder — temporal reuse keyed on the quantised column result,
which the census measured at 35.3%–47.0% — is now the largest remaining
materializer item. Item three, the row-major transpose, is still unjustified by
the run-length data. The two new maps remain outstanding and bear directly on
item three.

## Rung 15 — the delta census, and a classifier that was wrong first time

Steps one, two and four of the temporal ladder, run on the optimized `79445c4`
build before implementing anything.

### Where the expensive tail goes now

Top 5% of frames against the median frame, by stage:

| trace | median | p95+ | ratio | materializer share of the excess | projection share |
| --- | --- | --- | --- | --- | --- |
| cruise | 477,931 | 588,374 | 1.23x | **52.5%** | −2.6% |
| corners | 399,131 | 541,184 | 1.36x | **48.6%** | 21.8% |
| stress | 298,798 | 504,997 | 1.69x | **44.4%** | 31.9% |
| spin | 298,772 | 453,641 | 1.52x | **37.0%** | 33.8% |

The materializer still dominates both the mean and the tail, so the ladder's
ordering survives the mask-first change. Projection is a clear second on the
three traces that turn hard.

### Ground truth: how much actually changes

Measured from `g_map` directly, so no probe can be wrong about it.

| trace | cells changed a frame (of 360) | columns changed (of 20) | columns identical |
| --- | --- | --- | --- |
| stress | 31.38 (8.72%) | 6.95 | 65.25% |
| corners | 38.75 (10.76%) | 8.25 | 58.74% |
| cruise | 39.51 (10.97%) | 8.68 | 56.62% |
| spin | 52.40 (14.56%) | 12.15 | 39.24% |

**About nine tenths of the name table is identical from one frame to the next.**
The materializer currently visits roughly 170 interior rows an update to effect
about 40 cell changes.

### A classifier that was wrong, and what it hid

The first pass asked whether a column's changed cells formed one contiguous
block, and reported 81.6%–87.9% "scattered". That reading would have killed the
interval-delta idea. It was wrong: it treated a column as a single interval when
the materializer already treats top and bottom as separate mirrored edges, so a
symmetric FULL wall moving one row — two short runs, one at each edge — was being
counted as scatter.

Counting contiguous **runs** instead:

| trace | 1 run | **2 runs** | 3+ runs | longest run 1–2 cells |
| --- | --- | --- | --- | --- |
| cruise | 18.4% | **67.2%** | 14.4% | 79.6% |
| corners | 17.6% | **75.5%** | 6.8% | 80.6% |
| stress | 12.1% | **86.5%** | 1.5% | 76.8% |
| spin | 13.0% | **86.9%** | 0.1% | 79.3% |

**One or two runs covers 85.6% to 99.9% of changed columns**, and the longest run
is one or two cells in about 78%. Mean changed cells in a changed column is 4.3
to 4.7 — two short runs of about two cells, exactly the signature of a mirrored
wall whose top and bottom edges both moved by one tile row.

So the interval-delta representation is supported, on the condition that it keeps
top and bottom as separate intervals. A single-interval-per-column model would
see the same data as scatter, which is precisely the mistake made above.

### What that is worth, as an estimate rather than a measurement

The materializer costs 154,075 T of a 388,861 T loop on cruise after the
mask-first change. If it only compared a small retained descriptor per column and
then touched the cells the descriptor says changed, the work would be on the
order of twenty comparisons plus forty cell writes. That is an order-of-magnitude
reduction on paper and it is **not measured**; a micro-race against real retained
descriptor transitions is the next step and should come before any
implementation.

### A correction to earlier reporting

The per-function figure "p_fill is 21.8% of render" over-attributes. The profiler
builds ranges from one exported symbol to the next, and the labels inside the
fill loop are followed by unexported helpers (`full_tile_low$`,
`map_ptr_row_col$`, `polar_mark_dirty_fast$`, `polar_row_unclaimed_fast$`) which
fall inside the same range. The same applies to the new `p_fill_open` at 19.02%.
The stage-group figures in the frame timeline do not have this problem, because
mis-attribution inside a group does not move the group, and those are the numbers
to trust: materializer 39.6% of the loop on cruise after the change.

### Also corrected

The claim that a 75.6% fast-path rate "matches" the 82.19% wholly-unclaimed
figure was loose: they count different populations, spans versus interior rows.
The gap most likely means partially occluded interiors contain more rows than
open ones, which is testable and has not been tested.

### Not done

Step three — a separate census of quantised **edge** deltas — was folded into the
run-structure analysis rather than probed independently; the one- and two-cell
runs above are mostly edges, but their tile-word transitions were not classified.
Step five, the micro-race of current-fill against whole-column-skip against
interval-delta, is the next piece and has not been started.

## Rung 16 — the delta oracle: the representation is sound, and two cells wide

Before the four-rung race, the question that has to be answered first: can a
compact retained descriptor **predict** the changed cells, or does it have to
rediscover them by scanning? If the descriptor has to be the eighteen words
themselves then comparing it costs what the scan costs and the whole idea is
circular.

Tool: `tools/frame/delta_oracle.py`, fed by raw per-frame name tables dumped from
the emulator (`build/frame/maps/*.bin`, 720 bytes a frame). Each column, each
frame, is fitted to:

> top edge (row, word) × k · fill (first, last, word) · bottom edge (row, word) × k

and the predicted dirty set is computed **from two consecutive descriptors
alone**, then checked against the real `g_map(N−1) XOR g_map(N)`.

### Results, 11,920 column transitions across four traces

| trace | representable, k=1 | representable, k=2 | unsound | over-prediction at k=2 |
| --- | --- | --- | --- | --- |
| cruise | 72.75% | **83.12%** | 0 | +124.8% |
| spin | 55.64% | **88.39%** | 0 | +34.2% |
| corners | 76.64% | **89.13%** | 0 | +71.0% |
| stress | 72.11% | **90.10%** | 0 | +49.8% |

**Zero unsound transitions.** Across every trace and every column transition, the
predicted set never missed a cell that actually changed. That covers the three
cases flagged as most likely to break it — a boundary moving and exposing a
farther surface, a nearer surface entering or leaving a column, and a shade or
border change with stationary geometry — because all three appear in the corpus
and none produced a miss. A shade change is caught because the descriptor carries
the fill *word*, so the whole interval is predicted dirty, which is correct and
shows up as over-prediction rather than as a miss.

**Zero ragged columns.** The surface cells in a column are always one contiguous
row range, never a range with holes, in all 11,920 transitions. That is a
stronger structural guarantee than the run census implied and it is what makes
the fill-plus-two-edges shape adequate.

**The descriptor should be two edge cells per side, not one.** Widening k from 1
to 2 moves representability from 55.6–76.6% up to 83.1–90.1% and cuts
over-prediction from +158–194% down to +34–125%. The edge-cell histogram says
why: a side of the fill carries zero, one or two cells in 85–94% of columns.

The 10–17% that remain unrepresentable need a fallback; the figures above already
charge those as a full eighteen-cell column rewrite, so the over-prediction number
includes the fallback cost.

### What this is worth, in cells

On cruise, 150 frames: 3,911 cells actually change, the scheme would touch 8,792
— about 59 cells a frame against 26 strictly necessary. The current materializer
visits roughly 170 interior rows an update, each one a compare that usually finds
nothing, to effect about 40 changes. So the delta scheme touches roughly a third
as many places, and each one is a direct write rather than a compare-and-maybe-write.

### The gap this does not close

The oracle fits descriptors **from the output**. It proves the representation is
adequate and the prediction sound; it does not prove the runtime can produce the
same descriptor from projection without doing the work it is trying to avoid.
That is what rung four of the race has to charge, along with resolving a newly
exposed owner when a nearer surface shrinks.

### A bug worth recording

The first run reported spin at exactly 0.00% representable. The cause was
inferring the background word per row as that row's most common word. `0x000F` is
the mid-shade full-wall tile and is the modal word in every row a wall usually
covers, so background and wall swapped places and nothing fitted. The renderer's
own `base_word` — ceiling `0x0000`, horizon `0x0002`, floor `0x0001` — is now
used directly. Inferring a constant that the source already states was the
mistake, and the exact-zero result is what gave it away.

### Next

The four-rung race: current materializer, whole-column descriptor skip, two-edge
plus fill interval delta, and the same with the reveal fallback charged, on the
real transition corpus, charging descriptor load, comparison, address formation,
dirty marking, writes and fallback ownership resolution. Not started.

## Rung 17 — descriptor generation versus application, measured

The column asked for before the race: how much of the materializer's cost is
*producing* the descriptor, and how much is *applying* it? If generation
dominates, a delta scheme only moves the furniture.

The boundary already exists as an exported symbol. `_tsp_polar_p_span` sits
immediately after `polar_mark_span_fast$`, so the range from
`tsp_polar_surface_column_fast`'s entry to it is exactly the descriptor's
geometric generation — four `row_floor`s, top and bottom min/max, the coverage
span, and the ownership mask. Everything after it is emission.

Current build, 40 updates, 44.52 columns an update:

| | T-states/update | share of render | per column |
| --- | --- | --- | --- |
| **descriptor geometry generation** | 22,503 | **4.76%** | **505** |
| application: p_span tail | 21,304 | 4.51% | 479 |
| application: p_symbot | 35,018 | 7.41% | 787 |
| application: p_edge | 15,275 | 3.23% | 343 |
| application: p_cap | 14,092 | 2.98% | 317 |
| application: p_fill_open | 89,889 | 19.02% | 2,019 |
| **application total** | **175,578** | **37.15%** | **3,944** |

**Generation is about one eighth of application** — 505 T-states a column against
3,944. That is the answer the race needed before it was worth building: the
expensive half is emission, not deciding what to emit.

A bound rather than a headline: if a delta scheme cut application by 70% — which
the oracle's 59-cells-touched against ~170 interior rows plus edges suggests is
the right order — it would save roughly 123,000 T-states an update, about 26% of
render and 22% of the whole loop. That is an estimate from two measurements, not
a measurement.

### Two things this figure does not include

The "generation" range covers the descriptor's **geometry** only. The edge *tile
word* is computed by the LUT inside `prepare_edge$` and `prepare_symfull_edges$`,
which fall inside the application ranges, so generation is undercounted by
however much that costs. And these PC ranges over-attribute in general, as
recorded in rung 15: unexported helpers land in whichever exported range precedes
them. The figures are indicative of the ratio, not exact.

### Accepting two corrections

The oracle did **not** solve reveal handling. It proved that the final-state
representation encodes reveals soundly — the predicted set never missed a cell
when a nearer surface shrank and a farther one appeared. It did not show that a
runtime descriptor built from projection can arrive at that final state
economically. Something still has to resolve the newly exposed owner, and that is
rung four of the race.

"Zero ragged columns" is an empirical property of these four traces on this one
map, not a proved invariant of every map, profile and portal arrangement. The
fallback stays regardless, which is what makes the scheme safe rather than the
observation.

### The race, specified but not built

Four rungs on a corpus of real column transitions, driving the **real**
`tsp_polar_surface_column_fast` as rung one rather than a reimplementation of it,
so the baseline cannot drift:

1. current materializer, as shipped;
2. whole-column descriptor skip, falling through to (1) on any difference;
3. two-edge plus fill interval delta, emitting only the components that changed;
4. (3) with the reveal fallback charged when a nearer owner shrinks and the
   exposed rows must be resolved.

Charging previous-descriptor load, new-descriptor construction, comparison,
component-change detection, address formation, `g_map` stores, dirty extent
updates, and the full fallback for the 10–17% unrepresentable columns. Reporting
generation and application separately per rung, and gated on `g_map` matching
rung one exactly.
