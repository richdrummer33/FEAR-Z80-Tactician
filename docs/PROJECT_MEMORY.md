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

## Doom-style Game Gear 2.5D renderer — level-design/world-generation pillar

**IMPORTANT REMINDER RULE:** Whenever discussing level layout, room purpose, atmosphere, traversal, encounter-space design, or other actual game/level design for the Game Gear Doom-style 2.5D/3D renderer, surface this concept near the top of the response before getting lost in ordinary room-by-room design: we have explicitly discussed making the world *effectively infinite and traversable* by assembling deterministic procedural geometry blocks. This is not merely a rendering/storage optimization; it is a core creative level-design possibility worth actively considering.

### 1. Deterministic streaming / effectively infinite traversable world

- Author a vocabulary of compact, pre-baked geometry blocks: straight halls, 90-degree turns, rooms, junctions, T-junctions, transitions, stair modules, special spaces, etc.
- Give blocks explicit connection/end nodes or traversal thresholds.
- Crossing an appropriate threshold deterministically chooses/generates the next connected block from a very cheap seeded pseudo-random/index/hash scheme.
- Different branch choices can therefore produce genuinely different traversed worlds.
- Do not require the entire explored world to accumulate in RAM. Keep only a small rolling window of recent/current blocks if useful (for example a handful of previous zones plus upcoming ones).
- Backtracking should regenerate prior geometry deterministically from compact state rather than storing everything. Implementation may use reversible/indexable PRNG state, a path/chunk index + seed/hash, a tiny breadcrumb history, or another cheap deterministic scheme; the design goal matters more than committing now to a specific PRNG mechanism.
- A starting seed/index can define an entire reproducible world/path family.
- Preserve continuity and gameplay state where required with tiny semantic records rather than retaining full geometry.
- Treat this as both a memory/ROM technique *and* a game-design tool: mysterious endless facilities, divergent routes, impossible-feeling architectures, replayable layouts, and worlds that seem vastly larger than cartridge resources imply.

### 2. Elevation, stairwells, and vertically repeating spaces

- The baked renderer may make elevation surprisingly cheap enough to become an actual design primitive, not merely visual decoration.
- Explore true traversal height changes using stairs/landings/floors while keeping the renderer's compact baked representation.
- If a single node/block has a geometry/data limit, design around it deliberately: e.g. 2–3 steps, 90-degree landing, another short run; spiral/switchback stair modules; repeated vertical chunks.
- Stairwells can themselves be deterministic procedural chains and therefore effectively unbounded in height.
- They may contain biased/random optional exits, continuing stairs, branching floors, or different floor-selection choices.
- Reuse the existing hierarchical/node visibility approach where possible: nearby vertical geometry is still represented by the nodes that can see it, analogous to existing archway/raised-geometry work.
- Explore camera/world vertical offset as the cheap runtime degree of freedom: changing player/camera elevation is conceptually equivalent to shifting projected world geometry vertically relative to the camera.
- Existing screen-centre wall mirroring was introduced as an optimization, not a sacred renderer rule. Re-evaluate it if arbitrary vertical projection/elevation conflicts with it. If the later renderer tick budget permits, writing/choosing independent top and bottom wall tile/name-table results may be preferable to preserving mirroring.
- The table being referred to in the original discussion is the VDP **name table / tilemap**: entries select which tile pattern (plus attributes) appears at each screen tile location. Keep exact implementation details open until the current baked renderer/data structures are measured.

### Design intent

The exciting target is not simply “procedural corridors.” It is a cartridge-scale world that can feel spatially enormous, branch permanently according to player choices, support backtracking without retaining the whole world, and exploit baked geometry to add vertical spaces such as seemingly endless stair towers. When level-design discussions arise, actively remind Rich of this possibility and ask whether the current room/area should participate in this larger streaming grammar rather than assuming every level is a finite authored map.
