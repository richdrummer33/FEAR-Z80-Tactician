# Solid interior wall bake experiment

Branch: `experiment/gg-solid-interior-wall-bake`

## Purpose

The room-bundle PoC originally treated every authored wall as an infinitely thin vertical
surface segment. That representation is appropriate for the outer room/perimeter boundary,
but awkward for free-standing interior architecture: a pillar needs four hand-authored
segments, an interior wall has no physical end faces, and a window cut through a wall has no
automatically derived jamb/reveal faces.

This experiment keeps the Game Gear runtime format unchanged and adds a host-only volumetric
authoring layer.

## Authoring semantics

- Perimeter/boundary geometry remains the existing zero-thickness `add_seg()` surface.
- An interior solid wall is authored as one centerline plus thickness.
- The experiment default total thickness is 1 world unit, i.e. +/- 0.5 world unit about the
  centerline.
- `add_solid_wall_line()` derives the two broad vertical faces plus both exposed end caps.
- `add_solid_wall_line_caps()` can suppress a cap when an endpoint is buried in another
  wall/perimeter.
- `add_solid_window_line_caps()` cuts a rectangular opening through a solid wall and derives
  the two vertical jamb/reveal faces automatically.
- Full-height solid-wall endpoints and lighting geometry retain Q4 (1/16 world-unit) XY
  precision in the host scene.

Adjacent solid lines may touch/overlap. Their buried duplicate/internal faces are harmless to
the far-to-near host bake but are not yet removed by a true 2D CSG/contour-union pass. A later
cleanup can union footprints and emit only exposed contour faces if authoring density makes
the redundant surfaces significant.

## Critical renderer change

The old room-bundle raycaster selected only the nearest XY segment for each of the 160 screen
columns. That cannot represent a real window because the lower wall band, upper wall band,
jamb and farther geometry can all intersect the same screen ray at different vertical ranges.

The experiment now collects every XY segment hit for a screen column, sorts hits far-to-near,
and submits every vertical Z span to the existing semantic compositor. The compositor's normal
overdraw then acts as a painter's algorithm: nearer wall pixels overwrite farther pixels while
uncovered pixels remain visible through openings.

There is still no Game Gear runtime Z buffer. This work occurs only during the offline bake.

## Exact vertical spans

Room-bundle segments already carried arbitrary `z0/z1`, but the point-light receiver/occluder
scene previously reduced them to four coarse profile heights. `TSPHostSceneSegment` now
optionally carries exact Q4 vertical bounds. Lighting occlusion and wall receiver reconstruction
honor those exact bounds, so a ray can pass through a window opening rather than treating its
whole centerline as a floor-to-ceiling light blocker.

## Converted proof geometry

The branch converts representative interior architecture:

- linear-room baffle -> one solid centerline
- T-room divider -> one solid centerline
- turn-room baffle -> one solid centerline
- gallery wall -> solid attached wall with an automatically generated through-window and
  vertical jambs
- second gallery fin -> solid attached wall
- both pillars -> one centerline + thickness each, replacing four manually authored faces per
  pillar

The original pillar footprints are preserved by using 12-world-unit thickness on the existing
14-world-unit centerlines.

## Horizontal surfaces - implemented for local wall reveals

The host compositor now has an optional per-pixel camera-forward depth buffer used only by the
room-bundle baker. Ordinary Polar callers keep their existing painter behavior.

The solid-window helper emits two horizontal quads in addition to its vertical faces:

- sill top at the opening's lower Z
- lintel underside at the opening's upper Z

Vertical wall spans and horizontal quads therefore compete by exact host-side depth before the
final semantic pixels are canonicalized into Game Gear tiles. This remains entirely offline;
no Z buffer or polygon state is added to GG WRAM.

The next generalization is to expose this horizontal-quad path as an ordinary authoring
primitive for low-wall caps, tables, platforms, beams and similar architecture.

## Stairs - risers now derived from floor regions

The quarter-stair bundle no longer hand-authors its riser list. A host preprocessing helper
compares adjacent floor rectangles and emits a vertical face wherever their floor heights differ.

This derived five risers, not the four in the old manual list. The extra face is real: the old
geometry omitted the z=1 -> z=2 face where the stair turns onto the height-two landing. The
derived topology therefore closes an existing geometry hole rather than merely rewriting the
same four segments.

The authored floor regions remain the source of truth; the generated riser faces disappear into
the same RBP2 bake as every other static surface.

## Runtime invariants

No wall thickness, opening geometry, normals, or new Z structure is added to Game Gear WRAM.
The output is still exact room-bundle name-table patches plus scheduled 32-byte tile-pattern
loads. The runtime continues to replay pre-baked RBP2 packets.

## Verification - green host bake

The room-bundle generator performs early geometry assertions:

- one isolated solid line expands to four vertical faces
- the test through-window expands to the expected vertical face count
- a generated jamb spans the authored wall thickness
- the window emits sill + lintel horizontal quads
- neighboring floor-height regions derive the expected riser topology

Final host proof passed under both ordinary warning-clean compilation and ASan/UBSan. The full
multi-room bake also passes exact patch replay, canonical seam begin/end, bidirectional routes,
three-portal split routes and quarter-stair height rebasing.

Manifest proof markers:

- `solid_interior_wall_expansion=PASS`
- `window_vertical_reveal_generation=PASS`
- `window_horizontal_reveal_generation=PASS`
- `derived_stair_risers=PASS`

Measured runtime-stream impact:

- gallery/window route 0->1: scheduled tile budget remains 20 uploads/VBlank. Adding sill +
  lintel changed total scheduled tile loads only from 2,123 to 2,126 across 192 frames; patch
  bytes and changed-name-word count remained unchanged.
- stair route 0->1: the newly closed fifth riser raises total tile loads from 4,411 to 4,541 and
  patch bytes from 38,397 to 38,702, but the scheduled peak/budget remains 40 uploads/VBlank.

CI also emits normal traversal videos plus a dedicated 96-frame close orbit around the solid
window so the jambs, sill and lintel can be inspected at oblique views.
