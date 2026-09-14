# Renderer parity entry point

For parity work, start here rather than inferring integration state from benchmark files or commit recency.

1. Read `docs/PARITY_STATUS.json` for the current machine-readable status.
2. Read `docs/RENDERER_PARITY_MANIFEST.md` for rationale and graduation criteria.
3. Run `python3 tools/build_parity_materializer.py` to produce the post-transform materializer at `build/parity/tilesector_polar_materialize_gg.generated.s` without leaving `src/` dirty.
4. Run `python3 tools/validate_parity_manifest.py` to validate status references.
5. Treat exact-ROM hash/profile workflows as the authority for correctness and cycle claims.

The optimized Z80 research implementation is the target, but benchmark-only code remains explicitly separate from Game Gear ROM integration until exact-ROM evidence exists.
