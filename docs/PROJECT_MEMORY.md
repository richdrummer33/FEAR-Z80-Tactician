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
- Exploit **temporal coherence / no-visible-change suppression** after Q4 projection is preserved: if camera motion/rotation does not move a projected boundary enough to change any output pixel/tile, leave the existing tilemap entries untouched. For straight projected edges, consider carrying the edge equation/error state so the renderer can cheaply predict the next column/row at which a pixel-level change becomes possible instead of re-rasterizing every covered tile each update. Profile whether this saves more than the bookkeeping costs before adopting it broadly.
- Revisit a deliberately fixed render cadence (for example ~30 unique rendered updates/sec while simulation/input may tick faster) only after the visibility and VRAM bottlenecks are measured. Treat it as a later budget/latency trade rather than an early architectural assumption.
- Keep **portal/sector traversal** and **selective intra-tile compositing** conceptually separate. Portal traversal/topology prunes which farther geometry can be visible and carries screen-space apertures outward; the selective compositor is a general final-output mechanism for meaningful visible-boundary conflicts that land inside the same 8x8 hardware tile.
- Treat selective intra-tile compositing as a **general visible-boundary conflict mechanism**, not only a portal special case. It must cover front-to-back combinations such as player -> foreground pillar -> room wall / portal frame -> farther room walls, including conflicts where unrelated visible boundaries land in the same 8x8 hardware tile.
- Handle both screen-space boundary axes in the selective compositor: vertical-ish boundaries (pillar sides, jambs, wall endpoints) and horizontal/sloped boundaries (wall tops/bottoms, lintels, sills, raised-floor transitions). Corner cells may require both masks in one tile; a small window-like portal is a canonical stress case.
- `clipTop[20]` + `clipBottom[20]` encode one contiguous vertical aperture per coarse screen column. This is sufficient for the current room/arch/window geometry. If future maps allow multiple disconnected vertical openings in the same screen column (for example stacked windows), prefer a tiny per-column interval stack / recursive aperture representation rather than falling back to a generalized 20x18 Z field.
- Gate intra-tile splits by projected visual significance / representable precision. Tiny slivers that are below the current projection precision or below a chosen perceptual threshold should collapse to the nearer surface instead of spending compositor work or producing unstable shimmer.
- Explore **selective generative 8x8 line/composite tiles** only for rare sub-tile conflicts where precomputed edge/corner tiles cannot represent the visible split cleanly. Keep precomputed LUT-selected tiles as the normal fast path; profile CPU + VRAM cost before adopting any dynamic tile generation.
- Improve projected edge continuity with an error-carry / tile-scale Bresenham-style selector: choose precomputed 8x8 edge pieces so adjacent tile endpoints remain connected while accumulated error converges toward the ideal projected line. Extend the slope vocabulary only as needed; exploit H/V flip and palette reuse.
- Tooling idea: build a repeatable Gearsystem desktop-debugger recording layout using its Dear ImGui dockspace. Keep game view, disassembly/trace, registers/memory and VRAM/tile/background viewers in one persistent layout and record the whole GUI window; optionally add a live pane for TileSector profiling symbols later.

Current renderer performance / architecture priorities:

- Treat the richer TileSector renderer as the **visual/correctness oracle, not a sacred implementation**. Preserve arches, raised geometry, connected angular edges, fog/shading, portal visibility and Q4 movement while allowing the Game Gear runtime path to become aggressively hardware-specific.
- Performance gates are architectural: roughly **179K Z80 T-states/update for 20 unique rendered FPS** is the solid target and **119K T/update for 30 FPS** is aspirational. Prefer representation/dataflow/platform wins that delete whole classes of work; 5-15% micro-optimizations are only worthwhile inside a surviving measured hotspot.
- During the current ceiling-finding phase, runtime rendering is **name-table only**. All ordinary 8x8 tile graphics/patterns are preloaded in VRAM. Do not add dynamic pattern generation or per-frame pattern uploads while chasing the renderer ceiling; scale such effects back in only after the core path is fast.
- Converged next architecture: collapse `candidate visibility -> depth/interpolation state -> raster -> dirty detection` into a compact GG-native path that emits final **name-table words**. Prefer compressed contiguous surface runs/spans over 360 independent cell decisions where possible. A winning solid run should be recoverable from segment id + compact depth state; derive connected edge interpolation once per run rather than storing left/right Q6 reciprocals for every column.
- Strong experiment: let the GG materializer maintain the authoritative desired name-table shadow and a tiny dirty bitset/coverage representation, so current surface writes mark changed name-table cells directly and stale cells are restored from the base ceiling/horizon/floor template. This could remove full-frame map clear plus broad 360-word dirty comparison. Keep the host/C renderer as the reference oracle while profiling this path.
- Keep temporal-coherence opportunities compatible with the new representation: if a column/run descriptor is unchanged at final pixel/tile quantization, the GG materializer should be able to skip that column/run without reconstructing a generic intermediate frame.

Interaction / reporting conventions:

- `/TTS` responses should use normal readable paragraphs rather than inserting a line break after nearly every sentence. Keep paragraph breaks frequent enough for listening, but group related sentences into visually readable chunks; use bullets only where structure materially helps. Preserve technical detail and TTS-friendly punctuation without turning prose into one-sentence-per-line formatting.
