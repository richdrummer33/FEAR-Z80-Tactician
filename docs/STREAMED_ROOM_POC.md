# Streamed Room PoC

Status: experimental branch `experiment/gg-streaming-room-poc`

## Goal

Prove that the existing exact host-baked Game Gear renderer can be composed into
a deterministic, reversible, effectively unbounded room graph before solving
ROM compression.

The Game Gear should only replay local baked packets. Runtime world identity,
portal choices, height and backtracking are kept as tiny semantic state.

## Proven foundations

### 4 MiB banked cartridge execution

A 256-bank / 4,194,304-byte GG image executes banked functions placed in banks
4, 63, 64, 127, 191 and 254 under Gearsystem.

This proves the current GBDK/runtime path beyond the historical 1 MiB boundary.
The workflow always publishes a runnable `.gg` artifact.

### Deterministic topology

`tilesector_world_stream_poc.*` derives stable 32-bit child node keys from:

- run seed;
- parent node key;
- selected exit index.

Forward traversal records a compact breadcrumb. Backtracking restores the exact
parent descriptor. Tests cover long forward/reverse walks, sibling branches and
vertical floor deltas.

### Canonical portal seam

The reusable connector is an S-shaped double-occlusion throat.

The mathematically proven safe leg is:

- old-room retirement: y = 9.0;
- new-room reveal: y = 16.5;
- guaranteed neither-room-visible length: 7.5 world units.

The host test exhausted 1,501,560 rays toward the old aperture and 1,501,560
toward the new aperture with zero visibility escapes.

The hidden leg is the serialization boundary. It can:

1. retire the old room;
2. discard old dynamic tile ownership;
3. restore a canonical VRAM/name-table state;
4. preload the successor;
5. reveal the successor only after it is safe.

The canonical GG name-table hash remains:

`E4F108D3C424CCE3`

across ordinary rooms, splits, and the +4 stair proof.

## Room-local baking

The host compositor now accepts an injected `TSPHostCompositeScene` containing
room-local vertices, segments, horizontal receiver rectangles and static light
placement.

This preserves the original hard-light / shadow / penumbra compositor instead
of creating a separate room lighting implementation.

The GG runtime never sees this scene geometry. It only receives generated name
patches and scheduled tile-pattern loads.

## RBP2 room bundle format

Each authored module contains independently baked directed portal routes.

A route is identified by:

- bundle ID;
- entry portal;
- exit portal;
- local frame.

Each route:

- cold-starts from the canonical cache/name-table state;
- is independently tile-lifetime scheduled;
- must finish on the exact canonical seam;
- does not inherit dynamic slot history from its predecessor.

Reverse traversal is a separately baked/scheduled route. Packet bytes are not
naively reversed.

## Eight-module PoC catalog

Current authored visual bundles:

0. Wide portal-shadow chamber, point-lit.
   The lamp sits behind the flared room-side aperture. The two jambs become
   deliberate shadow casters, giving opening-corner rays and far-wall cuts.

1. Tight inset/spooky chamber, point-lit.
   A low inset lamp and short baffle make a small curious pool and a strong
   asymmetric shadow edge.

2. Broad three-portal T-split, unlit.
   Both branch mouths can be visible from the room while arbitrary successors
   remain hidden behind their individual S-throats. Six directed routes are
   baked: 0<->1, 0<->2 and 1<->2.

3. Quarter-turn stair, unlit.
   Four real riser faces and floor bands 0,+1,+2,+3,+4. The exit seam is
   physically +4 world units while the camera is also +4, so the terminal
   visual state is the same canonical seam.

4. Very wide gallery, unlit.
   Large lateral space with sparse architectural fins.

5. Flat 90-degree L-turn, point-lit.
   One physical authored turn supports opposite handedness by traversing its
   directed routes in opposite portal order.

6. Stepped room, unlit.
   Floor sequence 0,+2,+4,+2,0 with explicit riser faces.

7. Large pillar hall, point-lit.
   Two full-height pillars and a side light create long room-scale cast
   shadows.

This is exactly four lit modules and four unlit modules.

## Measured current-format cost

These figures are deliberately *not* ROM-optimized. They include exact name
patches plus explicit 32-byte dynamic tile-pattern uploads.

Representative directed route costs before the latest hostile-camera rail
revision:

| Bundle | Type | Approx route bytes | Scheduled uploads / VBlank |
| --- | --- | ---: | ---: |
| 0 | wide lit shadow room | 149 KB | 29 |
| 1 | tight lit inset room | 148 KB | 20 |
| 2 | split route | 106-119 KB | 18 |
| 3 | quarter stair | 40 KB | 6-7 |
| 4 | huge unlit gallery | 105 KB | 22 |
| 5 | lit L-turn | 192 KB | 35 |
| 6 | stepped room | 35 KB | 5 |
| 7 | lit pillar hall | 192 KB | 32 |

The important result is that floor-height complexity is cheap. Point-light
shadow vocabulary is the main storage/bandwidth multiplier.

A successful eight-module RBP2 host pack measured about 2.4 MiB of packet data
before C/linker overhead.

## Topology-to-asset catalog

`tilesector_room_catalog_poc.*` separates semantic topology from visual asset
choice.

Examples:

- HALL_STRAIGHT deterministically chooses among bundles 0,1,4,6,7.
- TURN_LEFT uses L-turn 0->1.
- TURN_RIGHT uses L-turn 1->0.
- T_SPLIT maps exit zero/one to split portals one/two.
- STAIR_QUARTER_UP_RIGHT maps to stair 0->1.
- STAIR_QUARTER_DOWN_LEFT maps to stair 1->0.

Unsupported topology is rejected rather than silently represented by the wrong
geometry. The current gaps are the three-forward-exit ROOM_SPLIT and the
opposite stair handedness.

## Current runtime validation sequence

The GG PoC ROM is designed to prove, in order:

1. deterministic ordinary root -> child;
2. child reverse;
3. breadcrumb restores the exact root;
4. root reverse;
5. deterministic T-split;
6. left child out/back;
7. sibling traversal left-portal -> right-portal through the split;
8. right child out/back;
9. left branch key regenerates exactly after visiting its sibling;
10. deterministic +4 quarter stair;
11. ordinary child room streams while logical floor is +4;
12. reverse child and breadcrumb restore;
13. reverse stair descends to the original floor;
14. hostile-camera tour executes the remaining authored visual classes.

The hostile rail deliberately uses lateral movement, off-axis looking and
backward-looking exit traversal rather than a clean centre-line movie.

## Next work after the eight-module 4 MiB ROM is green

1. Verify the full eight-module packet pack still links inside 256 banks.
2. Run the complete Gearsystem progress/identity test.
3. Inspect emulator screenshots against host reference frames, especially:
   - opening-corner cast shadows;
   - inset-light pool;
   - pillar shadows;
   - split sibling crossing;
   - risers and raised-floor camera;
   - widened room-side portal mouths.
4. Run the published `.gg` manually in another emulator / hardware flash cart.
5. Only then begin ROM compaction or arbitrary local state-graph work.

## Deliberate non-goals of this PoC

This branch does not yet solve arbitrary free analog camera positions under the
baked renderer.

The point is to prove that deterministic topology, independently reusable
visual modules, bidirectional traversal, branching, vertical rebasing, lighting
and cartridge banking compose correctly using the current heavyweight bake
format.
