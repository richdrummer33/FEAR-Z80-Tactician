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
