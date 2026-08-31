# Project memory / durable conventions

## Response/reporting convention — prominent

For every technical development/implementation reply in this project, begin with a descriptive title that immediately identifies the subject. When reporting implementation work, the title must explicitly say it is an update and include the current stage plus whether that stage is IN PROGRESS or COMPLETED (for example: "Implementation Update — Stage 23 — IN PROGRESS — Retained edge deltas"). For non-update technical replies, use the same descriptive-title principle with an appropriate subject label.

Immediately under the title, include a normal-sentence **bold-italic nutshell summary** of current status/progress. This is a status header, not a substitute for any later TL;DR or requested summary; TL;DRs may still appear separately when useful.

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

### Polar renderer carry-over invariants — do not re-learn these

These are accepted lessons from the mature TileSector branches that MUST be checked before adding new polar runtime machinery:

- **Persistent authoritative name table:** do not restore a full 20x18 base map every frame. `g_map` is persistent desired state; only geometry that disappeared is restored. The first polar GG implementation accidentally reintroduced a 360-word reset (~14K T/update). The retained polar begin-frame is ~1.4K T/update and removes the old 720-byte `g_prev_map` shadow.
- **Dirty state is produced where the final renderer changes state:** do not finish a frame and scan 360 words against a second map to rediscover changes. The renderer/materializer already knows when a name-table word changes.
- **Dirty representation should match the VDP transaction:** Stage19 row bursts are only a valid test after dirty state is already known during rendering. With that prerequisite restored in polar, the dirty-bit -> one row burst uploader reduced VBlank upload from ~50.6K to ~20.0K T/update. The earlier polar "scan then burst" regression did NOT falsify Stage19.
- **Hot dirty/lifetime addressing must be amortized:** never call a general row/column dirty helper for every changed tile from the fast materializer. Precompute column mask/group or an equivalent run-native representation once and update local state directly.
- **Final-owner work beats hidden intermediate writes:** the current polar far->near whole-cell overwrite can create dirty work for farther walls that a nearer wall later replaces. Prefer a representation/order where the final owner is known first (or baked), so hidden writes never happen.
- **Connected authored vertices are shared corners:** compute a unique corner bearing once/update at most; connected runs reuse it. Do not regress to two endpoint calculations per span.
- **One connected run crosses C/ASM once:** no per-column C helper/assembly bridge. Z80 walks the complete coarse-column run.
- **Generic C materialization is not the GG path:** direct name-table word generation is the accepted fast path. Do not reintroduce `put_cell` / row-floor / generic profile helpers into geometry/shade mode.
- **Start + step, not recompute:** wall reciprocal is once/span; depth advances incrementally across the run. Continue pushing this principle upstream into baked bearing/range coefficients.
- **Use narrow arithmetic deliberately:** the old reported ~40K `__mullong` polar bucket was a stale profiler-address bug, not linked code. The real surviving generic arithmetic target is ~33K T/update of `__mul16` from byte-sized products promoted through generic math. Profiler helper ranges must follow current linked symbols, never hardcoded addresses.
- **Runtime sort is not blindly deletable from the current pack:** one-byte index sort is accepted, but the current recipe stream is not a universal draw order. On the 360-frame demo, collection order was already far->near only ~42.8% of frames; 15/41 encountered topology signatures had multiple runtime orders. Eliminate this only with a stronger baked angular-event/chain representation.
- **Polar self-hash and legacy visual oracle answer different questions:** exact polar hashes gate optimization equivalence. Repaired legacy comparison currently differs substantially (mean ~51.6% name-table words; framebuffer ~37% at tick64 and ~9.7% at tick128). First-divergence tracing shows the earliest sample cell is never written by polar while legacy projects geometry into it, so that divergence is upstream visibility/projection behavior, not dirty-state corruption.
- **Profiler tax is closed:** profiled/raw ROM A/B is typically only a few tenths of a percent; keep both builds, but do not attribute large renderer costs to the hooks.
- **Before new local-bearing/depth bake work:** retained name-table/dirty machinery and visual/profiler harnesses must be green and measured. Then resume the missing original polar feature: cell-local baked bearing/range coefficients with adaptive refinement/fallback.
- Treat the richer TileSector renderer as the **visual/correctness oracle, not a sacred implementation**. Preserve arches, raised geometry, connected angular edges, fog/shading, portal visibility and Q4 movement while allowing the Game Gear runtime path to become aggressively hardware-specific.
- Performance gates are architectural: roughly **179K Z80 T-states/update for 20 unique rendered FPS** is the solid target and **119K T/update for 30 FPS** is aspirational. Prefer representation/dataflow/platform wins that delete whole classes of work; 5-15% micro-optimizations are only worthwhile inside a surviving measured hotspot.
- During the current ceiling-finding phase, runtime rendering is **name-table only**. All ordinary 8x8 tile graphics/patterns are preloaded in VRAM. Do not add dynamic pattern generation or per-frame pattern uploads while chasing the renderer ceiling; scale such effects back in only after the core path is fast.
- Converged next architecture: collapse `candidate visibility -> depth/interpolation state -> raster -> dirty detection` into a compact GG-native path that emits final **name-table words**. Prefer compressed contiguous surface runs/spans over 360 independent cell decisions where possible. A winning solid run should be recoverable from segment id + compact depth state; derive connected edge interpolation once per run rather than storing left/right Q6 reciprocals for every column.
- Strong experiment: let the GG materializer maintain the authoritative desired name-table shadow and a tiny dirty bitset/coverage representation, so current surface writes mark changed name-table cells directly and stale cells are restored from the base ceiling/horizon/floor template. This could remove full-frame map clear plus broad 360-word dirty comparison. Keep the host/C renderer as the reference oracle while profiling this path.
- Keep temporal-coherence opportunities compatible with the new representation: if a column/run descriptor is unchanged at final pixel/tile quantization, the GG materializer should be able to skip that column/run without reconstructing a generic intermediate frame.
- Future load-time experiment: consider **on-device per-level Adaptive Polar Visibility Field baking** so many/maps levels need not carry fully prebaked spatial fields in ROM. Standard cartridge ROM is read-only, so generated current-level data would have to live in WRAM (or explicitly supported cartridge SRAM/flash hardware), with compactness treated as a hard constraint. Benchmark a screen-disabled loading mode (VDP display disabled; CPU dedicated to baker, unrestricted VRAM access when needed) plus infrequent progress-bar refreshes. Estimate/measure bake T-states, load duration, WRAM footprint, and whether a hybrid ROM seed + on-device refinement is more practical than a full bake.
- Reporting convention for renderer/performance implementation updates: immediately after the title/status header, add a concise **BLUF** section. Include the big-picture result and, when more than one quantitative resource/performance item was measured, a tiny table covering items such as T-states/update, FPS/update rate, ROM/data bytes, WRAM/VRAM footprint, host bake time, and relevant GG capacity/budget. If only one quantitative item matters, state it inline instead of forcing a table.
- Adaptive Polar Field appearance features (AO, directional fake lighting, material/shade variants) must remain **optional/maskable** during GG implementation so geometry-only vs appearance-on performance can be compared. Prefer a runtime debug toggle/button combination or a trivially bypassable branch/jump in the profiling build; do not let optional appearance work obscure core geometry measurements.
- Hybrid load-time Polar Field variant to test later: ROM holds the expensive immutable map seed (authored vertices/segments, reusable critical constraints/chains/configs); a loading-time baker generates only the cheap/bulky current-sector or nearby-sector working tables into WRAM. Ordinary cartridge ROM is not writable. Compare this against fully prebaked per-map ROM fields only after topology packing is mature.
- Adaptive Polar Field V2.2 host POC checkpoint: exact authored-corner angular events + shared leaf-recipe packing produced a **4,635-byte (4.53 KiB)** topology field for the current TileSector demo map, including only **4 bytes** of 2-bit per-corner AO metadata. Host Python bake is ~2.6-3.0 s on the current development environment. Treat these as POC measurements, not GG runtime measurements.
- V2.2 strict 20x18 / 8x8 name-word proxy over the GG demo path: ~10.53% geometry-word mismatch (~37.9/360 words), ~11.81% with material/shade, ~12.90% with AO. This proxy is intentionally stricter than raw pixel mismatch because any edge difference marks the affected 8x8 output descriptor different.
- V2.2 AO decision: fake AO is static authored-geometry metadata, **2 bits per concave corner** (none/mild/medium/strong), with intensity derived offline from free-space corner angle. Runtime only selects preloaded edge/cap tile variants at projected physical corner IDs. Narrow wall/ceiling and wall/floor AO can live inside the existing edge tile as a one-pixel cap variant; do not add a separate ceiling/floor rendering pass for AO.
- V2.2 coarse runtime model (NOT measured Z80 timing): ~21.3K T/update core field path, ~+0.97K T material/shade, ~+0.24K T AO on the current demo trajectory. Use only as go/no-go evidence; authoritative cycle counts must come from GG C/ASM profiler instrumentation.


## Game Gear renderer — wall-light response, authored flicker, and aperture-light ideas

**APPROVED EXPERIMENT DIRECTION — Aug 31, 2026:** Build and measure a cheap wall-wide lighting response on top of the mature baked room/streaming renderer. Preserve a control path so appearance cost is measured against the previous binary point-light bake rather than guessed.

### Hardware dimming conclusion

- The GG VDP does **not** provide a per-pixel or per-tile brightness multiplier/alpha operation. Background name words can select tile pattern, H/V flip, priority and one of the two palettes, but arbitrary mixed wall/floor/ceiling pixels cannot be independently dimmed without palette semantics or a different tile pattern.
- Palette selection is still a powerful coarse accelerator. The baked-light path already uses background palette 1 as the lit transform while reserved indices inside that same palette reproduce ambient colours for shadow-side pixels in mixed tiles.
- Palette 1 is shared with sprites on Game Gear. Treat animated palette entries as a global art-resource contract: sprites using those entries will inherit the same pulse unless deliberately segregated.

### Wall-normal light response experiment

- Static wall response is a **host-bake concern**, not new Z80 lighting math.
- For each visible wall face, use one wall-wide quantized value from the angle between the oriented wall normal and the wall-centre-to-light vector: normal pointing toward light is strongest; approaching 90 degrees approaches ambient/minimum contribution.
- Current experiment target is **16 apparent levels**. Represent intermediate levels with stable ordered coverage between the existing ambient and lit palette semantics rather than requiring sixteen hardware palettes.
- Preserve a nonzero ambient/base wall appearance; the light contribution may fall to zero, but the wall itself does not become black.
- An optional camera term may add only a few quantization steps from the alignment between camera/view direction and the reflected light direction about the wall normal. Keep it broad/subtle, matte-looking, and independently removable if it produces distracting view-dependent shimmer.
- No runtime trigonometry, normalization or dot products are justified for static authored lights in the current architecture. The host computes the result; the GG replays the already-baked name-table/tile packets.
- Main falsifier is **tile-pattern/state entropy**, not Z80 arithmetic. A visually modest extra quantization can still create many distinct 8x8 patterns and increase scheduled pattern uploads. Always compare binary-light control vs angle-light experiment on unique patterns, patch bytes, tile loads, and required uploads/VBlank.

### Authored flickering lights

- Flicker is an authored property of selected reusable room/light assets, not a global effect. The initial target is the inset/spooky light room.
- Prefer a deterministic short-pulse pattern: isolated one-frame twitch, occasional double pulse, and a brief ragged burst.
- The current cheapest mechanism is palette-only animation of the lit palette entries. Leave the reserved ambient/shadow entries unchanged, so only pixels already classified as illuminated pulse.
- Cache the current flicker level and write CRAM only when the level changes. This avoids re-rendering walls, rebuilding tile graphics, or rewriting the name table.
- This naturally works when the same authored room bundle appears in the deterministic streaming maze. Multiple independently flickering light groups visible simultaneously would require a later palette/tile-class budgeting decision.

### Later visual experiment — directional outside light through architectural openings

- Explore narrow arrow-slit / embrasure-like apertures admitting a fake directional sun or moon vector.
- Gate admission by the opening's outward normal vs the authored directional-light vector; openings facing away can remain dark.
- Project the admitted beam onto floor/wall receiver geometry on the host. Low-angle light may intentionally form long stretched rectangular/trapezoidal pools across a room.
- Runtime should remain ordinary baked packet replay. The real cost risk is additional tile/state entropy as long beam edges move across the projected screen.
- Creative intent: imply a larger inaccessible exterior world and provide strong time-of-day / atmosphere cues without actually rendering outdoors.

### Later visual experiment — windows and view-through wall slits

- Treat a small non-traversable opening as portal-like visibility geometry. The first proof only needs to show another authored space through the aperture; cross-room dynamic lighting is explicitly deferred.
- For streamed rooms, the bake must know enough about the visible neighbour to compose the view. Prefer deterministic neighbour/bundle references or a deliberately bounded window-view dependency rather than turning runtime into a general cross-room renderer.
- Small window-like portals are also a useful stress case for the existing selective intra-tile compositor and multi-boundary handling.

### Later visual experiment — bars, grilles, and gratings

- Runtime alpha is not required. Thin bars/grilles can be ordinary baked occluder geometry composed into each camera state.
- Prefer canonical thin-coverage masks/pattern reuse and final-pattern deduplication. Very thin geometry can alias, shimmer, or multiply tile variants as viewpoint changes, so measure pattern entropy before promoting it to a common architectural motif.
- Keep hardware sprites as an optional later alternative for rare foreground grille layers, but the default concept is baked geometry with no runtime transparency.


### IMPLEMENTED PROOF — thick interior/exterior portholes — Aug 31, 2026

- Thick portholes are now a working host-baked geometry class, not merely a future idea. The proof lives on `experiment/gg-wall-angle-flicker`.
- Never model a porthole-bearing wall as an infinitely thin line. The current helper builds a real wall slab in XY with front and back wall faces, closed end caps, a low sill span, high lintel span, and recessed side-jamb/reveal faces through the slab depth.
- Initial vertical aperture is z=8..24 inside ordinary z=0..32 walls. Horizontal top/bottom reveal planes are not separately rasterized in this first 2.5D proof; wall thickness is nevertheless explicit in plan and visibly expressed by front/back parallax plus recessed side jambs.
- Porthole rooms opt into a host-only multi-surface column compositor: gather every ray/segment crossing, sort far-to-near, and composite all vertical spans. Existing rooms retain the old nearest-surface path. This permits a near sill/lintel/reveal and a farther room/object to coexist in one screen column without any runtime Z-buffer.
- Interior proof: a freestanding thick divider inside one room has a non-traversable view aperture; farther geometry in the same room is visible through it.
- Exterior proof: a six-world-unit-thick outer wall looks into an inaccessible exterior lightwell/court with a farther wall and offset masonry depth cue. It suggests space beyond the playable room without making that space traversable.
- Automated host assertions require (a) farther geometry to own visible pixels through the aperture at the straight-on inspection pose and (b) a recessed reveal/jamb to own visible pixels at an oblique inspection pose. These tests prove both see-through visibility and wall thickness.
- First measured porthole route costs remain below the existing 48-pattern-upload/VBlank gate. Interior porthole was ~31 scheduled uploads at peak; exterior was ~11 before the final exterior depth-cue tweak. Re-measure after any geometry changes.
- Do NOT blindly append heavyweight porthole bundles to the current 4 MiB eight-room stream pack. That pack already used about 212 of the packer's 224 generated-bank safety allowance; two complete bidirectional porthole bundles exceeded the deliberate safety cap. The current validation therefore uses a separate porthole-only pack/ROM. Longer-term work should improve bundle deduplication/compression or selectively include authored window rooms.
- Runtime remains packet replay only: no alpha, no runtime window renderer, no dynamic geometry, and no runtime Z-buffer.

### Emulator validation invariant — libretro RAM view

- In Gearsystem 3.9.16's libretro build used by this project, `retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)` did not mirror the live Z80 C000-DFFF work RAM correctly for the streamed-room validation harness; it repeatedly reported zero while the ROM visibly advanced through all 52,000 captured frames.
- Treat libretro as framebuffer capture in this harness. For authoritative RAM/symbol assertions use the native Gearsystem runner and `Memory::DebugRetrieve()`.
- Do not diagnose a ROM/runtime failure solely from the old libretro `RAM addr=... data=0000` output.
