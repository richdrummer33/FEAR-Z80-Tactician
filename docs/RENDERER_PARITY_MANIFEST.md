# Renderer parity manifest

This file is the integration contract between the optimized Z80 research renderer and the Game Gear ROM renderer.

The rule is simple: a renderer optimization is **not** considered integrated merely because executable Z80 exists somewhere in `tools/`, a benchmark is exact, or a composed whole-update estimate includes it. It is integrated only when the corresponding behavior is present in the code assembled into the `.gg` ROM and is validated on the exact ROM path.

Status vocabulary:

- `ROM-EXACT`: present in the exact ROM path and validated against the logical-update oracle/hash.
- `ROM-EQUIV`: implemented differently in the ROM, but validated as semantically equivalent.
- `ROM-EXPERIMENT`: real ROM integration rung exists, but it has not yet graduated into the canonical source path.
- `Z80-ONLY`: executable/cycle-counted Z80 research exists, but the ROM does not yet execute the same architecture.
- `N/A`: research mechanism does not apply to this ROM path.

## Current parity matrix

| Capability | Z80 research target | GG ROM status | ROM implementation / validation hook | Remaining gap |
| --- | --- | --- | --- | --- |
| Run endpoint carry | Carry previous column right endpoint into next left endpoint | `ROM-EXPERIMENT` | `tools/apply_integrated_materializer_rung.py` | Graduate into canonical materializer after exact-ROM A/B remains green |
| Border hoist | Compute physical border state at run ends, not every column | `ROM-EXPERIMENT` | `tools/apply_integrated_materializer_rung.py` | Same as above |
| Edge-local DDA carry | Compute local edge coordinate once, then advance by -8 per row | `ROM-EXPERIMENT` | `tools/apply_edge_local_dda_rung.py` | Graduate after exact-ROM A/B |
| Edge LUT | Replace repeated edge tile derivation with LUT lookup | `ROM-EQUIV` | Existing materializer edge LUT path | Keep target/ROM indexing semantics locked by hash tests |
| Edge LUT row-base hoist | Hoist invariant LUT row/base work out of repeated row path | `ROM-EXPERIMENT` | exact-ROM row-base A/B rung | Graduate after target measurements |
| FULL vertical symmetry | Derive mirrored bottom edge from top edge | `ROM-EQUIV` | `_tsp_polar_surface_column_fast` FULL symmetry path | None known |
| Persistent ownership / coverage | Mark owned row span and avoid nearer-overdraw | `ROM-EQUIV` | `polar_mark_span_fast` / unclaimed masks | None known at semantic level |
| Sequential interior claim walk | Carry ownership bit/byte while walking contiguous interior rows | `ROM-EXPERIMENT` | `tools/apply_interior_claimwalker_rung.py` | Confirm fixed pointer-lifetime version remains hash exact and cheaper |
| Painter-style ordering | Exploit far-to-near ordering to simplify ownership/materialization work | `ROM-EXPERIMENT` | painter integration rung | Decide whether it supersedes or complements current ownership path |
| DDA_G complete materializer shape | Incrementalize the materializer rather than repeatedly re-derive per-cell/per-row state | `PARTIAL` | run carry + edge-local DDA + claim walker cover subsets | ROM still retains older surface-column/materializer skeleton |
| Finite edge programs | Encode edge trajectories as reusable finite programs | `Z80-ONLY` | benchmark/tooling generated programs | No equivalent ROM executor yet |
| PROGJOIN | Join finite edge program chunks and reduce repeated setup | `Z80-ONLY` | Z80 research harnesses | No equivalent ROM executor yet |
| Run-edge invariant descriptor/threshold hoist | Resolve family/step-derived bases once per run edge | `Z80-ONLY` | post-PROGJOIN Z80 audit | Depends on finite-program executor existing in ROM |
| Direct rank table / later dispatcher work | Replace repeated threshold/rank logic with pre-resolved table lookup | `Z80-ONLY` / research | Z80 audit only | Do not port before program-executor architecture is chosen |

`PARTIAL` is intentionally not a graduation state. It means several mechanisms have crossed over, but the surrounding execution model is still materially different.

## Canonical parity rule

For every future optimization claim, documentation and CI output must answer all four questions explicitly:

1. **What exact Z80 research rung is the target?**
2. **What exact source or generated source is assembled into the Game Gear ROM?**
3. **Is logical output exact/equivalent on the same deterministic pose/input trace?**
4. **What is the exact-ROM cycle delta on the same trace?**

If question 2 is answered with a build-time transformation, CI must preserve the post-transform assembly as an artifact. Reviewing only `src/tilesector_polar_materialize_gg.s` is not sufficient while integration rungs are implemented as source-rewrite scripts.

## Graduation rule

An experimental integration rung may move to `ROM-EXACT` or `ROM-EQUIV` only when:

- the transformed source builds into a `.gg` ROM;
- deterministic logical-update hashes match the baseline/oracle for the validation corpus;
- the exact ROM is exercised under the target-aware profiler;
- the cycle delta is measured rather than composed;
- the resulting implementation is either committed directly into the canonical runtime source or generated deterministically by a checked-in generator whose output is preserved and diffed in CI.

## Architecture parity target

The near-term goal is **behavioral and cost-model parity**, not textual identity. The Game Gear implementation may use different register allocation, banking, VDP/name-table plumbing, or target-specific ownership bookkeeping. The important criterion is that the ROM no longer performs a class of repeated runtime derivation that the Z80 target has eliminated.

The largest remaining architectural gap is the finite-program execution model. The current ROM has absorbed several DDA/hoist recurrences while retaining the older surface-column skeleton. Closing parity therefore eventually requires either:

- porting the finite edge-program/PROGJOIN execution model into the ROM, or
- demonstrating with exact-ROM measurements that a target-specific executor reaches the same eliminated-work frontier by a different mechanism.

Until then, simulator composed whole-update estimates and ROM measurements must remain separately labelled.