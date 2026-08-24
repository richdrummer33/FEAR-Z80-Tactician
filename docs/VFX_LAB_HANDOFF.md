# Game Gear VFX Lab — implementation handoff

Branch: `feature/gg-vfx-lab`
Baseline: `main` / v0.5.0 Stage 5 (`FEAR-Z80-Tactician`)
Status: first code slice implemented and host-tested; Game Gear integration still pending.

## Why this branch exists

This branch is a focused rendering/VFX laboratory built on the real Stage 5 simulation rather than a toy renderer. The aim is to preserve the existing office-loop map, actor/collision data, camera, HUD and deterministic simulation, while bypassing or scripting selected NPC behavior so repeatable weapon/VFX scenarios can exercise the actual Game Gear renderer.

The target is deliberately ambitious but practical: make a Game Gear ROM that demonstrates unusually rich top-down tactical effects by exploiting the VDP instead of pretending it is a framebuffer GPU.

## Hardware mental model and constraints

- Game Gear visible area: 160 x 144 pixels, 20 x 18 visible 8x8 cells.
- Background is one tile/name-table plane plus hardware sprites; background priority can visually cover sprites.
- Tile patterns are 8x8, 4 bpp planar, 32 bytes each.
- VRAM is byte-addressable through VDP I/O ports. It is legal to read/write arbitrary pattern bytes, but reads are buffered and read-modify-write is more expensive than streaming known data.
- Sprites have pixel X/Y positions; projectile movement is not restricted to tile coordinates or eight directions.
- Sprite graphics do not have hardware rotation/flipping. Draw procedural streak geometry instead of rotating bitmaps.
- Eight sprites per scanline is the key projectile/particle bottleneck; long beams should prefer background scratch tiles.
- H/V scroll registers provide very cheap camera shake. Background scroll does not automatically move sprites, so world sprites must receive the matching render offset while HUD can remain stable.
- Palette/CRAM changes are extremely cheap relative to graphics regeneration.
- Display blanking can intentionally become part of an effect and also creates a favourable VRAM-transfer window.
- The Game Gear exposes only the centre of the inherited SMS raster; extended blanking around the visible LCD region can be exploited for additional VRAM transfer time.
- H-scroll can be varied on scanlines for raster distortion. This shifts an entire scanline; it is not a true local distortion-mask shader.

## Renderer philosophy

Gameplay resolution is immediate. Cosmetic work is allowed to complete over subsequent frames.

Use a small VFX job scheduler with separate CPU-work and VRAM-transfer budgets. Effects enqueue work instead of monopolising the frame. Under load, reduce cosmetic richness first: fewer glow cells, fewer visible particles, slower beam construction, shorter afterglow. Never delay gameplay outcomes.

Hardware limitations should become part of the effect language:

- draw lines instead of rotating sprites;
- spatial/temporal dither instead of alpha blending;
- palette exposure instead of HDR lighting;
- background scratch tiles instead of long sprite chains;
- scroll registers instead of redrawing for shake;
- intentional blackout frames as VRAM-upload opportunities;
- per-scanline H-scroll as a brief shockwave approximation.

## Implemented in this branch now

`src/fx.c` and `src/fx.h` are portable, host-testable primitives. They do not yet drive the Game Gear renderer.

Implemented primitives:

- planar 4-bpp tile clear/set/get pixel;
- Bresenham-style line rasterization into an 8x8 pattern;
- procedural dithered blast ring;
- procedural tracer tile generated from motion delta rather than angle/trig;
- 1/3 and 2/3 three-phase spatial/temporal dither masks;
- optional high-bit glow operation for palette-index lighting experiments;
- deterministic 16-bit LFSR helper;
- fixed-point arbitrary-angle projectile motion;
- top-down debris simulation with hidden Z, gravity, floor landing/bounce, X/Y wall ricochet, drag, size classes and lifetime.

`tests/test_fx.c` verifies the above on the host. Existing Stage 5 simulation tests remain unchanged and passing.

## Debris model: top-down projection of rough 3D blast dynamics

Particles have screen/world X/Y plus a hidden Z height. Z is not drawn directly; it controls whether a fragment is airborne, bouncing or grounded.

Two initial material/size behaviours exist:

- SMALL: can launch faster but loses X/Y energy faster, bounces less strongly, settles sooner.
- CHUNK: generally slower/heavier-looking, retains lateral momentum better, ricochets harder and can bounce vertically before settling.

While airborne, X/Y motion is projected directly into the top-down view. Walls use the existing 2D collision map. On contact, the relevant axis velocity reverses and is attenuated. Z receives gravity and floor bounce/landing logic. Once grounded, stronger floor drag takes over; when motion becomes tiny or lifetime expires, the particle can be removed or visually dither-faded by the presentation layer.

This is intentionally not rigid-body physics. The objective is that viewed from directly overhead, fragments read like a compressed 3D explosion rather than 2D billiard balls.

## Planned deterministic VFX scenes

### Scene 1 — arbitrary-angle projectile/tracer

- Preserve fixed-point continuous X/Y physics.
- Generate the visual streak from current minus previous position; no atan2, sin/cos or runtime sprite rotation.
- Prefer one 8x8 sprite containing head + short trail.
- Trail structure: solid near head, approximately 2/3 duty middle, approximately 1/3 duty far tail.
- Use seed/phase offsets so neighbouring tracers do not blink in lockstep.
- Long tracers may use background scratch tiles if scanline sprite pressure becomes high.

### Scene 2 — beam cannon

Gameplay raycast/hit resolves on frame zero. Visual afterimage is a deferred job.

Recommended hybrid:

- immediate impact and/or muzzle highlight: 1–2 sprites;
- long beam body: temporary background scratch tiles;
- traverse only touched 8x8 cells using 2D grid DDA;
- for each touched cell, remember the original 16-bit tilemap entry;
- build a 32-byte scratch pattern from canonical tile art in ROM/cache, then composite the local beam line;
- point the name-table cell at the scratch pattern;
- form from impact back toward shooter over several frames;
- hold briefly, then decay progressively;
- during fade, alternate scratch/original tilemap entry for temporal opacity instead of regenerating/reuploading pattern bytes;
- cleanup is normally only restoring the original two-byte tilemap entry and recycling the scratch slot.

Do not read base art back from VRAM unless a transformation genuinely depends on current VRAM pixels. Known graphics should be copied/generated from ROM or RAM and streamed outward.

### Scene 3 — grenade

Compose multiple cheap primitives:

1. damage/physics resolve instantly;
2. immediate palette flash;
3. directional initial screen kick;
4. short irregular decaying shake;
5. procedural ring and/or focal sprites;
6. hidden-Z debris using existing map collision;
7. particle multiplexing so more particles may be simulated than displayed;
8. local/environment exposure response;
9. pseudo-random electrical flicker;
10. selected display-off frames used both aesthetically and as bonus VRAM-transfer windows;
11. optional short raster shockwave experiment.

Screen shake uses VDP H/V scroll for background plus the same screen-space offset for world sprites. Keep HUD unshaken.

Initial shake direction should be physically suggestive rather than pure noise: derive from blast position, dominant nearby impacted NPC, wall geometry, or a seeded combination, then decay into smaller irregular vibration. A roughly two-second perceptual tail is acceptable, but a literal two-second exponential time constant would linger too long; use a faster decay with an explicit cutoff.

### Scene 4 — combined stress scene

Exercise bullets, beam, grenade, lighting and particles concurrently. The scheduler should degrade optional presentation gracefully while simulation stays deterministic.

## Lighting / exposure tricks

### Global fake exposure

For bright beams, grenade flash or future flashbangs, darken most environment palette entries while preserving a few reserved beam/highlight colours. The bright object appears dramatically more luminous because the rest of the scene is suppressed.

This can affect both background and sprites, but sprites share their sprite palette. Asset/palette conventions must reserve entries that stay hot during exposure shifts.

### Local tile glow

Two practical tiers:

1. Prefer cached/pre-authored bright variants of common floor/wall tiles and swap tilemap indices around the effect.
2. For custom scratch tiles, experiment with a reserved high palette bit: ordinary colours 0–7 and bright counterparts 8–15. Turning on the high bitplane maps underlying colour to its glowing counterpart without general RGB arithmetic.

Glow should be coarse and time-sliced: beam core first, nearest cells later, outer glow only if work budget permits, then decay inward/outward as chosen.

## Flashbang / lighting-failure FX

Flashbang reuses existing primitives rather than needing a unique renderer:

- near-white palette/exposure flash;
- optional white backdrop/display-off frame;
- desaturated/dim recovery;
- irregular electrical flicker;
- occasional fullscreen display-off frames;
- decaying shake if desired;
- blackout/whiteout frames can flush deferred VRAM jobs.

Do not use temporal blanking as ordinary smooth brightness control when palette dimming will do. Low duty cycles such as ~30% visible will read as aggressive flicker/strobe. Use palette changes for the continuous brightness envelope and sparse pseudo-random off frames for unstable electrical lighting.

## Direct VRAM processing idea

VRAM patterns are byte-addressable, not sprite-block-only. Because a tile row is four bytes, very small transforms can theoretically work row-by-row:

1. set VDP read address;
2. read the four planar row bytes into Z80 registers;
3. transform masks/bitplanes;
4. set write address;
5. write four bytes back;
6. repeat.

This avoids a 32-byte RAM working copy, but it is not automatically faster: VRAM reads are buffered and read/write direction/address changes cost I/O. Benchmark against generating known output in RAM/ROM and write-only streaming.

Good candidates for direct RMW experiments: dither/dissolve masks, reversible XOR-like visual transforms, lighting-bitplane manipulation, damage mutation where current pattern contents genuinely matter.

Beware shared patterns: mutating one pattern changes every map cell/sprite that references it. Local effects need unique scratch slots.

## Raster shockwave experiment

A FEAR-like circular distortion mask cannot be reproduced literally because the VDP does not provide per-pixel displacement. The feasible approximation is:

- local circular ring artwork around the explosion;
- for ~2–6 frames, use line interrupts to apply a small H-scroll offset table around the blast Y region;
- shape the table like an expanding ring/wave, alternating +/- displacement;
- because the artwork anchors the effect locally, the eye may interpret whole-scanline shear as blast-space warping.

Keep this optional and isolated. If Z80/interrupt timing or visual quality is poor, remove it without affecting the rest of grenade rendering.

## Future lightning weapon

Reuse the beam infrastructure. Replace the straight path generator with a deterministic jagged polyline: create several points along source-to-target and offset them along the perpendicular using an LFSR/seed. Rasterize segment-to-segment using the same scratch-tile, glow, exposure and fade systems. Branching arcs are a later experiment because touched-tile and VRAM cost grows quickly.

## VFX scheduler / scratch architecture to implement next

Suggested modules:

- `vfx_lab.c/.h`: scripted scenes and timelines;
- `vdp_fx.c/.h`: GG-only register/VRAM/palette/blanking backend;
- `gg_world.c/.h`: reusable world/background rendering extraction if useful;
- optional tiny debug font/overlay helper.

Suggested public concepts:

- `FxJob`: type, phase, priority, work remaining, per-effect state;
- CPU work budget per video frame;
- VRAM byte/job budget per blanking period;
- scratch tile free list + original name-table entry per touched cell;
- queued palette writes;
- queued tile pattern writes;
- queued name-table restores/swaps.

Under pressure, preserve in this order:

1. gameplay outcome;
2. actor/world render correctness;
3. effect core/impact;
4. screen shake / palette exposure;
5. beam body / blast ring;
6. nearest local glow;
7. secondary glow;
8. particle count / decorative sparks.

## Demo/debug overlay

Retrofit the existing readable HUD/debug approach. Useful counters:

- current VFX scene and phase;
- current frame / scene frame;
- CPU VFX work units consumed/budget;
- queued/transferred VRAM bytes;
- scratch tiles used/free;
- active sprites;
- worst sprites on one scanline;
- active/simulated debris and visible subset;
- display intentionally blanked yes/no;
- exposure/palette mode;
- raster-warp enabled yes/no.

Runtime toggles should allow A/B comparison: temporal dither, local glow, exposure dimming, particle multiplexing, raster warp, extended blanking.

## Build/test expectations

Host first:

```sh
make test
```

On this VFX branch, `make test` includes both the existing simulation regression suite and `tests/test_fx.c`.

Game Gear once GBDK is available:

```sh
make gg GBDK_HOME=/path/to/gbdk
make gg-seed42 GBDK_HOME=/path/to/gbdk
```

For the dedicated VFX ROM, add a separate target rather than replacing the canonical Stage 5 ROM until the integration is stable, for example:

```sh
make gg-vfx GBDK_HOME=/path/to/gbdk
```

The VFX ROM should still use the real map/collision/sprite renderer, but bypass autonomous GOAP decisions for selected actors and drive deterministic scripts.

Verify with Gearsystem/Emulicious or equivalent accurate GG emulation. Preserve evidence in the style already used by this repository: `.gg`, symbols/map where useful, full recording, selected screenshots and a compact runtime verification note. Curated ROMs remain committed according to the repository's existing versioning policy.

## Current validation status

Actually completed in the working sandbox before this branch handoff:

- `tests/test_fx.c`: PASS (`fx tests: ok`).
- Existing `tests/test_sim.c`: PASS on all deterministic office-loop seeds.
- Portable VFX code compiled with host GCC.

Not yet completed and must not be represented as done:

- no VFX-specific `.gg` ROM has been built;
- `fx.c` has not yet been compiled with GBDK in the current environment;
- no VFX ROM has been run in Gearsystem/Emulicious;
- no scratch-tile beam implementation exists yet;
- no grenade/beam scripted scene harness exists yet;
- no raster-shockwave implementation has been hardware-timed yet.

## First next steps for a human or GPT desktop agent

1. Pull/checkout `feature/gg-vfx-lab`.
2. Run `make test` and preserve the green baseline.
3. Compile `src/fx.c` with GBDK as a standalone object to catch SDCC/Z80-C portability issues before integration.
4. Add a separate deterministic `gg-vfx` entry point/target; do not disrupt canonical v0.5.0 targets.
5. Implement the smallest visible proof first: arbitrary-angle procedural tracer + palette exposure + scroll shake.
6. Capture screenshots/video and measure VRAM/sprite pressure.
7. Add scratch-tile allocator and beam construction.
8. Add hidden-Z grenade debris and intentional flicker/blanking.
9. Add optional raster shockwave last.
10. Run the combined stress scene and tune graceful-degradation thresholds based on measured behaviour.

## Scope guard

The VFX lab exists to prove rendering techniques and their real GG cost. It should reuse simulation state but not become a second AI architecture project. Keep scripted NPC actions deterministic until the VFX primitives are measured and stable; reconnect autonomous GOAP behaviour afterward.
