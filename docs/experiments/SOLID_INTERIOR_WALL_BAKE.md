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

## Current limitation: horizontal surfaces

The room-bundle baker is still fundamentally a vertical-wall raycaster. The solid-wall helper
therefore closes vertical sides/end caps, but it does not yet emit horizontal top surfaces,
window sills, window-lintel undersides, or table/platform tops.

A full-height wall that meets the authored floor and ceiling is visually enclosed by those
existing planes. A low wall/table or a fully physical window reveal is not yet a mathematically
closed 3D manifold until horizontal local surface spans are added.

## Stairs

The same philosophy is directly applicable to stairs. The current stair and step bundles
manually author riser segments between floor-height bands. A later host preprocessing pass can
compare adjacent floor regions and automatically emit a vertical riser wherever neighboring
floor heights differ. This should remove the current hand-authored riser list without changing
the RBP2 runtime format.

## Runtime invariants

No wall thickness, opening geometry, normals, or new Z structure is added to Game Gear WRAM.
The output is still exact room-bundle name-table patches plus scheduled 32-byte tile-pattern
loads. The runtime continues to replay pre-baked RBP2 packets.

## Verification hooks

The room-bundle generator now performs early geometry assertions:

- one isolated solid line expands to four vertical faces
- the test through-window expands to the expected vertical face count
- a generated jamb spans the authored wall thickness

The emitted manifest gains:

- `solid_interior_wall_expansion=PASS`
- `window_vertical_reveal_generation=PASS`

Full host bake, pack replay, ROM build, emulator screenshots and runtime verification are still
required before promoting this branch. GitHub currently reports no CI workflow/status for the
branch.
