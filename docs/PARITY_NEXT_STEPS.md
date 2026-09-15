# Renderer parity convergence order

The parity contract is intentionally separate from optimization selection. This document defines the order for closing the remaining architectural gap without confusing isolated Z80 benchmark wins with ROM integration.

## Phase 1 — lock the exact-ROM integration ladder

1. Keep `CARRY_EDGE_A + BORDERHOIST` in the exact-ROM A/B.
2. Keep edge-local DDA carry in the exact-ROM A/B.
3. Keep the fixed interior claim walker in the exact-ROM A/B.
4. Preserve the post-transform assembly and its diff as CI artifacts.
5. Require logical-update hash equality before treating any cycle result as meaningful.

Exit criterion: all currently integrated recurrences are exact and their target-cycle deltas are measured on identical traces.

## Phase 2 — collapse the patch hydra

Once Phase 1 is green, replace the chain of source-rewrite scripts with either:

- canonical optimized assembly in `src/tilesector_polar_materialize_gg.s`, or
- one deterministic generator with a checked-in/generated output contract.

Do not leave the production renderer defined by the accidental ordering of multiple independent textual patch scripts. The patch scripts are good integration probes, not a durable architecture.

Exit criterion: there is one reviewable source of truth for the assembly that reaches the ROM.

## Phase 3 — quantify residual old-skeleton cost

Use the target-aware profiler to divide mode-0 materialization into these buckets on the same turn and forward traces:

- run setup / endpoint projection;
- row-span ownership marking;
- edge setup;
- edge lookup / edge emission;
- interior fill;
- name-table change/dirty bookkeeping;
- miscellaneous materializer glue.

The purpose is to determine how much cost remains because the ROM retains the surface-column skeleton, versus unavoidable Game Gear bookkeeping.

Exit criterion: every major residual cost has a target-measured bucket and a clear Z80-target analogue.

## Phase 4 — finite-program architecture A/B

The largest remaining semantic gap is finite edge programs / PROGJOIN. Do not transplant the complete research stack in one shot.

Build one ROM experiment with this narrow contract:

1. Pick only the geometry-only FULL-wall edge path.
2. Precompute or generate the same finite edge-program vocabulary used by the Z80 research target for that restricted path.
3. Keep existing ownership, persistent name-table, dirty tracking, and final tile store semantics unchanged.
4. Replace only repeated edge-shape derivation/dispatch with a finite-program executor.
5. Hash-compare against the current exact ROM over identical traces.
6. Measure exact-ROM cycles and ROM bytes.

If it wins, widen to asymmetric profiles. If it does not, profile why before adding PROGJOIN or direct-rank machinery.

Exit criterion: the ROM either executes finite edge programs exactly, or there is measured evidence that a Game-Gear-specific executor reaches the same eliminated-work frontier more cheaply.

## Phase 5 — PROGJOIN and edge-invariant hoists

Only after a finite-program ROM executor exists:

- port PROGJOIN;
- hoist family/step-derived threshold and descriptor bases to run-edge scope;
- evaluate direct rank tables;
- remeasure exact ROM after every rung.

These are downstream optimizations. Porting them before the executor exists would recreate the original error: treating a research result as if its prerequisite architecture were already present.

## Permanent rule

No performance table should contain a row labelled simply “whole update” unless it is one of:

- `TARGET-MEASURED`: one executable target path measured end to end; or
- `COMPOSED`: explicitly arithmetic composition of isolated measurements.

The two labels must never be silently compared as though they were the same experiment.
