# FULL-wall maze — Scale Shock

Purpose: stress the retained Polar renderer with a real maze-like level while keeping
geometry and runtime representation deliberately small.

## Hard constraints

- FULL wall profile only. No LINTEL, RISER, RAISED, windows, stairs, or height changes.
- Flat floor and centred eye height.
- 32 authored vertices maximum; this level uses exactly 32.
- 32 physical surfaces maximum; this level uses 28.
- Only two non-axis-aligned walls, both on the central island.
- Runtime uses the existing 5-bit FULL-only key path and conservative yaw-aware PVS.

## Spatial design

The level is not a perfect corridor maze. It is a small spatial labyrinth built from
large changes in scale:

1. The player begins near the southwest edge with a long eastward sightline.
2. The first internal snake repeatedly pinches movement into narrow bypasses, then
   releases into much larger pockets.
3. A second staggered snake produces several alternate approaches into the middle
   instead of a single correct corridor.
4. The central pentagonal island is the visual/topological pivot. Two diagonal faces
   break the orthogonal mental grid without spending vertices everywhere.
5. The northeast and southeast doglegs create room-like pockets and apparent
   destinations, but both can be approached from more than one direction.
6. Large open areas are deliberately re-entered from different narrow routes, so a
   familiar space can arrive from an unfamiliar bearing.

The intended rhythm is compression -> release -> choice -> loop -> re-entry. Repeated
right-angle motifs weaken orientation, while the rare diagonals become strong but
ambiguous landmarks.

## Why this topology

The design borrows several reliable maze/level-design ideas without copying a specific
map: looped routes rather than dead-end spam; revisitation of recognizable spaces from
new angles; contrast between cramped and open geometry; multiple connections into major
spaces; and selective irregular geometry to disturb the player's internal compass.

The outer shell is intentionally plain. The vertex budget is spent on topology and
scale transitions, not ornamental wall detail.

## Geometry

World bounds: x=8..144, y=8..104.
Spawn: (18,92), facing east.
Surfaces: 28 FULL.
Vertices: 32.
Diagonal surfaces: 2.

The authored vertices and surfaces live in `tools/full_maze_bake.py`.

## Bake / runtime

Run:

```bash
python3 tools/full_maze_bake.py \
  --repo-root . \
  --out build/full-wall-maze/generated/optimized_renderer_map_data.inc \
  --floor-out src/generated/full_maze_floor.h \
  --preview-out build/full-wall-maze/full-maze.svg
```

The baker flood-fills the spawn-connected walkable component, emits a compact row-run
collision oracle, and bakes conservative 16x16-cell x 16-yaw PVS masks.

Local pre-commit bake validation:

- walkable cells: 11,742
- floor runs: 274
- PVS cells: 54
- PVS bytes: 3,456
- mean candidate surfaces: 11.13
- p95 candidate surfaces: 22
- maximum candidate surfaces: 27

CI builds the playable Game Gear ROM with `TSPF_FULL_MAZE=1`, then runs the existing
Gearsystem movement/turn verification and asserts the eye height remains exactly flat.
