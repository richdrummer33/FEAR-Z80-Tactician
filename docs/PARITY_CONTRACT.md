# Parity contract

The optimized Z80 research path is the design/performance target. The Game Gear ROM is considered at parity only when the exact ROM executes an equivalent eliminated-work architecture on the same deterministic workload.

## Required evidence per rung

- target rung named;
- generated/linked GG source identifiable;
- deterministic output/hash exact or explicitly equivalent;
- exact-ROM cycle delta measured on the same trace;
- ROM/code-size effect recorded;
- status reflected in `docs/PARITY_STATUS.json`.

A benchmark-only Z80 result is never a ROM implementation claim.
A composed whole-update estimate is never a target-measured end-to-end result.

## Current source of truth

- Human-readable matrix: `docs/RENDERER_PARITY_MANIFEST.md`
- Machine-readable status: `docs/PARITY_STATUS.json`
- Ordered GG integration builder: `tools/build_parity_materializer.py`
- Generated-source enforcement: `tools/check_renderer_parity.py`
- Manifest validation: `tools/validate_parity_manifest.py`

The build must preserve the post-transform assembly and baseline-to-generated diff so reviews inspect what is actually assembled, not merely the pre-transform source.
