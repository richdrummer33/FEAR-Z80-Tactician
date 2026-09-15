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
