# Versioning

FEAR-Z80-Tactician uses a lightweight semantic milestone scheme:

`vMAJOR.MINOR.PATCH`

- **MAJOR**: incompatible cartridge/data-format or project-direction break after 1.0.
- **MINOR**: a new gameplay/AI capability milestone (GOAP, richer combat, HTN, etc.).
- **PATCH**: fixes, performance work, rendering/debug improvements, or build-only changes that do not add a major capability layer.

During the experimental pre-1.0 period, the historical development stages map directly onto the minor number. The first repository snapshot is therefore **v0.5.0**, corresponding to Stage 5: banked persistent individual GOAP.

## ROM filenames

Committed Game Gear ROM snapshots use:

`FEAR-Z80-Tactician-vMAJOR.MINOR.PATCH-seedN.gg`

Examples:

- `FEAR-Z80-Tactician-v0.5.0-seed2.gg` — max-population 4v7 reference build.
- `FEAR-Z80-Tactician-v0.5.0-seed42.gg` — secondary deterministic reference build.

The seed suffix is part of the artifact identity because the current demo bakes its default seed into the ROM. It does **not** change the project version.

## Repository policy

- Curated `.gg` ROM snapshots are intentionally committed under `roms/`; do not add a global `*.gg` ignore rule.
- Generated linker/debug files, host binaries, framebuffer captures, and videos are reproducible artifacts and are not required to be committed.
- `VERSION` is the single source of truth for the current project version used by the Makefile.
- Each tagged/checkpointed version should add an entry to `CHANGELOG.md` and a checksum file under `roms/`.
