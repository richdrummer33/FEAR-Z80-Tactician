# Project memory / durable conventions

Current checkpoint: **v0.5.0 — Stage 5, banked persistent individual GOAP**.

Durable decisions for future work:

1. Keep authored world data compact and simulation-centric. Static wall occupancy is bit-packed; doors and other sparse dynamic features live in separate compact tables/state.
2. Preserve room-graph long-range navigation plus cheap local steering/BFS fallback. Do not regress to full-map BFS for every actor every tick.
3. Keep individual GOAP persistent: reuse valid plans across ticks and replan only on invalidation or goal change.
4. Keep squad HTN out until the individual brain/combat layer is stable and profiled.
5. Use GBDK autobanking / Sega mapper rather than fighting the 16 KiB fixed bank. Bank high-level AI by subsystem and minimize mapper transitions (currently one brain-bank entry per actor round).
6. Treat the SD flashcart as ROM storage, not extra Game Gear RAM. The challenge target remains a conventional single cartridge image where practical.
7. Curated Game Gear ROM builds are part of the development record and stay committed under `roms/` with versioned filenames. Do not globally ignore `*.gg`.
8. Versioning follows `vMAJOR.MINOR.PATCH`; pre-1.0 capability stages map to the minor number. Current Stage 5 is `v0.5.0`; patch releases are bug/perf/build-only changes.
9. Deterministic reference seeds: seed 2 exercises max 4v7 / eleven actors; seed 42 is the secondary cross-check.
10. Current next direction: enrich autonomous individual combat/GOAP semantics while retaining the compact room-graph foundation; task bindings and shallow squad HTN come afterward.

Deferred renderer TODOs:

- Preserve camera translation precision through rendering. `TSState.x_q4/y_q4` retain 4 fractional bits (1/16 world-unit), but the current TileSector render path truncates them with `>> 4` before camera transform. Carry Q4 (or an equivalently cheap fractional correction) through projection so slow translation does not visually advance in whole world-unit steps.
- Treat selective intra-tile compositing as a **general visible-boundary conflict mechanism**, not only a portal special case. It must cover front-to-back combinations such as player -> foreground pillar -> room wall / portal frame -> farther room walls, including conflicts where unrelated visible boundaries land in the same 8x8 hardware tile.
- Handle both screen-space boundary axes in the selective compositor: vertical-ish boundaries (pillar sides, jambs, wall endpoints) and horizontal/sloped boundaries (wall tops/bottoms, lintels, sills, raised-floor transitions). Corner cells may require both masks in one tile; a small window-like portal is a canonical stress case.
- Gate intra-tile splits by projected visual significance / representable precision. Tiny slivers that are below the current projection precision or below a chosen perceptual threshold should collapse to the nearer surface instead of spending compositor work or producing unstable shimmer.
- Explore **selective generative 8x8 line/composite tiles** only for rare sub-tile conflicts where precomputed edge/corner tiles cannot represent the visible split cleanly. Keep precomputed LUT-selected tiles as the normal fast path; profile CPU + VRAM cost before adopting any dynamic tile generation.
- Improve projected edge continuity with an error-carry / tile-scale Bresenham-style selector: choose precomputed 8x8 edge pieces so adjacent tile endpoints remain connected while accumulated error converges toward the ideal projected line. Extend the slope vocabulary only as needed; exploit H/V flip and palette reuse.
- Tooling idea: build a repeatable Gearsystem desktop-debugger recording layout using its Dear ImGui dockspace. Keep game view, disassembly/trace, registers/memory and VRAM/tile/background viewers in one persistent layout and record the whole GUI window; optionally add a live pane for TileSector profiling symbols later.

Interaction / reporting conventions:

- `/TTS` responses should use normal readable paragraphs rather than inserting a line break after nearly every sentence. Keep paragraph breaks frequent enough for listening, but group related sentences into visually readable chunks; use bullets only where structure materially helps. Preserve technical detail and TTS-friendly punctuation without turning prose into one-sentence-per-line formatting.
