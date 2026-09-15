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
