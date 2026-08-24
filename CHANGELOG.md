# Changelog

## v0.5.0 — 2026-08-24

First repository checkpoint.

- 64 KiB Sega-mapper Game Gear ROM using GBDK autobanking.
- Original 46x24 `office-loop` topology with 4 BLUE versus seeded 5-7 RED population.
- Eleven-actor maximum stress configuration.
- 4x4 logical cells with horizontally scrolling battlefield and fixed HUD.
- Bit-packed wall data and compact door state.
- Room-graph long-range navigation with bounded local BFS fallback.
- Imperfect contact memory, LOS, cover, suppression, ammo/reload, doors and combat.
- Per-agent goal arbitration and persistent backward-regressive GOAP.
- Multi-action `RELOAD -> SHOOT` plans verified on host and in emulated Game Gear RAM.
- Seed 2 reference: RED wins at tick 59; 273 replans / 269 persistent-plan reuses.
- Seed 42 reference: RED wins at tick 69.

Historical Stage 1 and Stage 4 verification notes are retained under `docs/history/`.
