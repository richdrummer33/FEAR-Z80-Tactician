# Project memory / durable conventions

> # IMPORTANT: PROJECT MEMORY IS NOT THE SCRATCHPAD
>
> Use the project's transient scratchpad / working notes for on-the-fly measurements, hypotheses, partial experiments, profiler dumps, and things that may be wrong tomorrow.
>
> **This file is the promotion layer.** Put ideas here once they have become durable decisions, validated lessons, explicitly approved next directions, or high-value concepts that future work must not forget. Consolidate rather than dumping every intermediate measurement here.
>
> The exact scratchpad filename/path is intentionally not hard-coded here unless independently confirmed; do not invent one.

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


## Doom-style Game Gear 2.5D renderer — next major architecture

### Compiled state-transition / name-table patch renderer

**NEXT BIG STEP — APPROVED DIRECTION:** Move the static-world renderer toward a host-compiled transition system. The Z80 should increasingly stop answering "what changed?" and instead be told the already-computed answer by ROM.

Core model:

```text
HOST / PC BAKE:
camera/view state A -> exact final GG name table A
camera/view state B -> exact final GG name table B
A vs B -> locate changed words -> compress/deduplicate patch

ROM:
state A + crossed transition -> state B + patch ID
no visible change -> zero/empty patch

Z80:
update/identify state -> fetch transition -> if patch non-empty, apply exact name-table patch -> upload known dirty row bursts
```

Important consequences and rules:

- This is **not corner-specific**. Convex/reflex occluding corners, portal edges, depth changes, camera translation, rotation, and screen quantization can all *cause* transitions, but the runtime representation should be the general state/transition graph and its output deltas.
- Bake three layers conceptually: **(1) world visibility events**, **(2) quantized display events**, **(3) exact transition output patches**.
- The host should do the expensive comparison. If two adjacent/reachable baked states produce the same final 20x18 name table, runtime should not XOR/scan/re-render to rediscover that fact. Store an empty transition or equivalent "same answer" result.
- If a transition changes output, precompute the exact affected name-table words and, where useful, row spans/min-max extents. The runtime should preferably replay the patch rather than project, sort, resolve ownership, walk spans, rasterize, materialize, then discover dirty cells.
- Preserve the exact host renderer as the oracle. A compiled patch transition is accepted only if replay reproduces the exact expected name table for the tested state transition.
- Measure state entropy before committing to a final ROM representation: number of unique display states, outgoing transitions/state, mean and 95th-percentile changed words, zero-patch rate, patch-dictionary reuse, and total packed ROM size.
- Near geometry may create more frequent transitions, but that should mean **more frequent cheap patches**, not a return to the full general renderer.
- Dynamic actors/projectiles/effects can remain a separate sprite/overlay/exception layer rather than multiplying the static-world state graph.
- After this architecture is measured and stable, the surviving tiny transition/patch executor is a strong candidate for hand-written/generated Z80 assembly. Do not first rewrite the present general renderer wholesale in assembly.
- Keep mapper changes coarse: organize transition/state data into phase/cell/region packets where practical rather than bank-switching per primitive.

### Perceptual motion cheats — deliberately later

Keep a separate back-pocket presentation idea: when true static geometry quantizes to the same name-table result across small movement, cheap visual activity may still communicate motion without waking the full 3D renderer. Candidates include tiny hardware-scroll adjustments, velocity-dependent floor/edge tile cycling, palette/light shimmer, or other deliberately non-geometric temporal cues. These are **presentation experiments after the raw transition renderer is validated** and must not contaminate geometry-performance benchmarks.

### VDP / VBlank-aware patch scheduling — deliberately later

Once the exact patch representation is stable and its cost can be cheaply predicted from baked metadata such as changed-word count and dirty row spans, investigate scheduling work around the Game Gear's display cadence. Possible experiments include front-loading patch preparation when a cheap update leaves CPU time available, deferring non-urgent work until a safe later display interval, and choosing when to perform the actual row bursts so the Z80/VDP pipeline wastes as little time as practical. Keep this as a **late optimization only**: respect proven VDP access/VBlank constraints, do not treat hidden/off-screen rows as free bandwidth, and do not complicate the transition architecture until its raw CPU/ROM behaviour is measured.


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
