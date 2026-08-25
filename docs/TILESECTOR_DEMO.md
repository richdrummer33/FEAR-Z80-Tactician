# Game Gear TileSector demo

Experimental ground-up Doom-ish renderer for the stock Game Gear. It does **not** cast one world ray per screen column. Authored wall line-segments are transformed once, projected into screen-space intervals, then compete for twenty coarse horizontal ownership/depth slots.

## Build

```sh
make tilesector-test
make tilesector-host
make GBDK_HOME=/path/to/gbdk gg-tilesector
```

ROM: `build/gg-tilesector-demo.gg`

Host visual sample:

```sh
./build/tilesector_preview 150 frame.ppm
```

## Controls

The ROM starts in recorded-play mode. The camera accelerates through the first room, crosses the broad open passage into the second room, performs a smoothed right/left steering sequence, then brakes to rest.

- D-pad Up/Down: forward/reverse with ~0.53 s 0-to-run / run-to-0 ramp.
- D-pad Left/Right: turn.
- Any directional input immediately interrupts recorded play and gives control to the player.
- Start: reset position and restart recorded play.

## Renderer hot path

1. Player position is Q8; map vertices are Q4.
2. A 256-entry signed sine table rotates each unique wall vertex into camera X/depth once.
3. Endpoint projection uses a 128-entry reciprocal-depth table instead of general per-endpoint division.
4. Every surviving projected segment covers an interval of the twenty 8-pixel screen columns.
5. Reciprocal depth is incrementally interpolated over that interval. The larger reciprocal wins the column. This is screen-space conflict resolution, not ray/world traversal.
6. Column height is derived directly from reciprocal depth.
7. Three wall brightness classes provide moderate distance fog; authored per-wall bias gives cheap orientation/material variation.
8. Segment ownership/depth discontinuities select prebuilt black left/right-edge tile variants.
9. Top/bottom wall boundaries use eight sub-tile offsets. The background name table is the framebuffer; no pixel framebuffer exists.
10. The Game Gear front-end finds each dirty row's minimal changed X span and performs one native 16-bit tilemap burst for that run immediately after VBlank.

The demo uses 207 generated background tiles: ceiling, floor/horizon, plus full/top/bottom wall variants for three fog shades and four vertical-edge states. They are generated once at boot and remain resident in VRAM.

## Deliberate constraints

This first cut uses one visible opaque wall layer per coarse screen column. The open passage is therefore represented as actual gaps in the wall graph rather than an upper arch/header that would require a second visible vertical layer. Collision is intentionally a cheap union of room/corridor regions rather than polygon collision.

The second room's upper boundary is stored as several real line segments rather than orthogonal grid faces, so the prototype exercises arbitrary segment projection and corner ownership.

## Next profiler-led changes

Only pursue these if the actual ROM says they are worth the cycles/VRAM:

- replace the rare 32-bit near-plane clipping divide with a tiny LUT/reciprocal clipper;
- add quantized sloped top/bottom edge families so the 8-pixel screen columns stop stair-stepping at wall silhouettes;
- add portal traversal / remaining-FOV interval masks so rooms with many hidden segments do not transform geometry that cannot be seen;
- add a second vertical visibility interval per column for true arch headers/windows/raised sectors;
- move the hottest transform/interpolation kernels to Z80 assembly if compiler output is measurably poor.
