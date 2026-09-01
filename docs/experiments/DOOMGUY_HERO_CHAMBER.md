# Doomguy Hero Chamber

Status: host geometry/light proof green; real GLB import pending.

## Asset normalization target

User-supplied Doomguy source metadata:

- one mesh, no textures
- about 1,481,242 vertices
- about 500,086 triangles
- reported source bounds: 0.68 x 0.62 x 0.97
- real-world Doomguy height target: 2.03 m

Normalize the source height to 19.0 renderer world units. This implies:

- source scale: 19 / 0.97 = about 19.59
- target footprint from reported bounds: about 13.3 x 12.1 world units
- 32-world-unit room height corresponds to about 3.42 m
- existing 16-world-unit eye height corresponds to about 1.71 m

The model sits on a low 3-unit plinth at the room centre.

## Chamber composition

Bundle 11 is an isolated proof chamber:

- canonical west/east traversal portals
- zero-thickness NORTH perimeter wall
- true perimeter porthole: x 66..78, z 8..22
- exterior point source at approximately (62,-96,18)
- four 8x8 solid corner pillars
- central 20x18x3 plinth
- current procedural hero proxy occupying the incoming Doomguy target bounds
- hard cast-light mode to preserve tile vocabulary

The outside-light position and aperture were chosen geometrically. Rays from the
light through the porthole enclose the normalized hero volume; at the south wall
the projected silhouette expands to near room height while retaining illuminated
margin around it.

## Mesh-cast shadow implementation

TSPHostCompositeScene now supports an optional host-only arbitrary occluder
callback. The room mesh baker supplies rmb_segment_occluded().

The current shadow query:

1. rejects the whole mesh with a world-space AABB test when possible;
2. only if that passes, intersects the light-to-receiver segment against mesh
   triangles;
3. returns blocked/unblocked to the existing point-light bake.

This does not add geometry, ray tests, or shadow state to the Game Gear runtime.

For the real high-resolution source, do not feed all ~500k source triangles
directly to every shadow query. Preserve the source as the authoring master,
then derive separately:

- a silhouette-preserving visual bake mesh;
- a much cheaper shadow proxy preserving head / shoulders / weapon / body /
  rock silhouette.

## Verification

Isolated bundle-11 proof:

- warning-clean host compile: PASS
- ASan/UBSan: PASS
- mesh shadow focused self-test: PASS
- independent route replay: PASS
- canonical seam: E4F108D3C424CCE3
- route 0->1: 192 frames, 48,235 patch bytes, 5,682 tile loads,
  raw peak 83, scheduled budget 47 uploads/VBlank
- route 1->0: 192 frames, 48,235 patch bytes, 5,685 tile loads,
  raw peak 79, scheduled budget 47 uploads/VBlank

This is deliberately close to the 48-upload promotion ceiling, so the real
asset import should prioritize stable visual vocabulary rather than maximizing
retained triangle count.

## Secondary organic-mesh test

A Japanese-tree GLB was also supplied for a later organic-shape stress test.
Reported metadata is approximately 5,589 triangles / 3,697 vertices, one mesh,
one material, no textures, with bounds about 0.891 x 1.000 x 0.611.
