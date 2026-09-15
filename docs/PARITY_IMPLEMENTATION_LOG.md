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
