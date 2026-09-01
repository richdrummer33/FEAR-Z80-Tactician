# Doomguy Hero Chamber

Status: host geometry/light proof green; compressed-GLB import path implemented and CI-proven; exact user Doomguy binary still pending sandbox mount.

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


## GLB import architecture

The continuation branch `experiment/gg-doomguy-glb-import` adds an actual
host-side source-mesh pipeline rather than teaching the room generator about
glTF directly.

`tools/glb_rmb/convert.mjs`:

1. reads ordinary, `EXT_meshopt_compression`, or Draco GLB data;
2. applies node/world transforms;
3. discards bake-irrelevant attributes such as normals/UVs/material channels;
4. welds the geometry;
5. derives two independently simplified meshes from the same normalized master;
6. recenters X/Y, places the source minimum Z at zero, and scales to the
   requested world height;
7. emits signed-Q8 positions plus uint16 triangle indices as a generated C
   include for the existing host baker.

The default Doomguy command targets roughly 1,800 visual triangles and 350
shadow triangles. These are starting points, not visual-quality policy; the
actual promotion decision is still tile vocabulary / VBlank churn plus visual
inspection.

The room-mesh layer now distinguishes object roles:

- visible + non-shadow-casting: the richer imported statue image;
- invisible + shadow-casting: the cheap silhouette proxy;
- visible + shadow-casting: normal procedural/world mesh objects.

This avoids paying exact high-resolution triangle intersection cost for every
light-visibility query while keeping visual and cast-shadow registration derived
from the same source master.

Imported objects with no requested outline also skip edge-adjacency construction
entirely. This is important for hero meshes: internal tessellation is supposed
to be visually silent, and building an unused edge vocabulary was quadratic
host-side bookkeeping with no GG benefit.

The procedural Doomguy remains behind the default compile path. Defining
`ROOM_BUNDLE_DOOMGUY_GENERATED` switches bundle 11 to the generated visual +
shadow arrays. That leaves the already-proven 47/48 proxy chamber as an exact
A/B control rather than replacing the control while the real asset is tuned.

## Import verification

CI now creates a meshopt-compressed synthetic GLB and proves the complete path:

- compressed GLB decode: PASS
- simplification + Q8 C emission: PASS
- generated visual/shadow symbol contract: PASS
- compile with `ROOM_BUNDLE_DOOMGUY_GENERATED`: PASS
- bundle-11 bake through generated indexed meshes: PASS
- mesh shadow callback/self-test through the imported path: PASS

The normal procedural-control build remains warning-clean and its sanitizer
proof still reports the established 47-upload/VBlank chamber result.

### Current evidence boundary

The user supplied a ZIP containing the real Doomguy and secondary organic GLB,
but the current execution sandbox did not mount the declared
`/mnt/data/GlbModels.zip` path and the corresponding File Library view exposes
only the pasted transcript, not the ZIP bytes. Therefore this branch does **not**
claim the real Doomguy has yet been decoded, simplified, oriented, rendered, or
measured.

Once the binary mount is available, the remaining experiment is intentionally
small: run the converter on the real source, inspect inferred orientation/bounds,
compile the generated include, tune visual/shadow triangle targets against
appearance and the real 48-upload ceiling, then capture the existing route and
detail-orbit proof.

## Hero mesh shading: the coarse lighting proxy and its replacement

### The failure

The imported hero shipped with a "hybrid" lighting arrangement: the visual shell
carried one flat shade (`ROOM_BUNDLE_DOOMGUY_SHADE_LEVELS=1`) and all lighting
came from a separately simplified **lighting proxy** rasterized on top as a
clipped overlay.

At the sweep's own settings that proxy decimates to **18 triangles / 12
vertices** for a 500,086-triangle figure. Reproduced locally, the result is
exactly what it must be: each proxy triangle covers a large screen area, gets
one flat `face_shade()` value, and paints a hard straight-edged wedge across
the model. The wedges cut through the helmet, the shoulder and the chest as one
facet, because at 18 triangles no facet corresponds to any anatomical feature.

Three separate defects compounded:

1. **Resolution collapse.** Quadric decimation minimizes positional error, but
   shading error is a function of the NORMAL field. At an 18-triangle budget the
   normals are area-weighted averages over whole limbs.
2. **No registration.** `convert.mjs` derives the visual, lighting and shadow
   meshes by three INDEPENDENT simplifications of the same master, so the
   proxy's surface deviates freely from the shell's. The lit/unlit boundary
   therefore lands wherever the proxy's facet edges project, not where the
   anatomy is.
3. **No cross-depth test.** `tsp_host_composite_pixel_overlay_depth()`
   depth-tests overlay facets only against each other, never against the shell's
   own `g_depth`. A proxy facet lying behind the visible shell surface still
   paints it. Combined with a near-convex 18-triangle hull, facets spanning
   concavities light the concavity as though it were a convex lit surface.

The overlay IS clipped to shell-owned pixels, so it cannot paint background;
the appearance of lighting spilling into empty space is defect 3 plus the hull
bridging concave regions.

### Measured alternatives

Bundle 11, route 0->1, `ROOM_BUNDLE_SCHEDULER_MAX_UPLOADS=255`:

| Configuration | scheduled budget |
|---|---|
| Flat shell, no lighting at all | 51 |
| Flat shell + 18-triangle lighting proxy (the artefact) | 69 |
| Uniform ambient + binary lit bit on the point-light channel | 58 |
| Per-face 3-shade on the real 5,184-triangle shell | 111 |
| ...+ screen-space shade consolidation | 87-91 |
| **5-stop ramp, smooth normals, static bake, crease, dither** | **138** |

The proxy is cheaper than correct shading, but it buys that by destroying
registration, which is the wrong axis to spend on. Speckle consolidation alone
only reached 91, so unsupported single-pixel noise was NOT the dominant cost;
boundary cells were.

### What replaced it

**Widened brightness ramp.** `SEM_FAR/MID/NEAR` was a three-stop distance ramp,
which is all a flat wall needs. Two interstitial indices (`SEM_FAR_MID`,
`SEM_MID_NEAR`) were added in the two largest gaps, giving a five-stop ramp.
Walls address it in steps of two and the hard-light transform is +2 stops, so
every wall result is bit-identical; only meshes use the finer stops. The 4bpp
shadow alias (`v+7`) still lands inside the palette: ambient 1..7 -> 8..14.

**Smooth per-vertex normals**, so a shade boundary follows the surface
curvature rather than the tessellation.

**Static per-vertex bake.** The hero and its light are both fixed, so light
visibility (self-shadow) and cavity openness are properties of the geometry.
They are solved once per vertex against the object's own full-resolution
triangles and interpolated per pixel. Doing this per pixel per frame would be
billions of ray tests; per vertex it costs a few seconds, once.

**Crease field measured on the SOURCE mesh.** Decimation removes folds before
anything else, so the shell cannot measure its own creases: measured on the
5,184-triangle shell, concavity came out negative (convex) for 89% of the
surface and the crease it drove touched 482 pixels across 2,500 frames.
`tools/glb_rmb/recess.mjs` measures concavity on the welded 500k source and
stencils it onto the shell over a world-space radius matched to the shell's
triangle size. The field is normalized by RANK, not by value: it has a long
tail (90th percentile 0.026 against a maximum 0.125), so a linear remap left
the whole drawn set bunched against zero.

**Occlusion belongs in brightness, not in a post-pass.** A first attempt drew
creases as a dithered darkening after quantization. That clips: creases mostly
occur in already-dark regions, where there are no ramp stops left below them.
Folding the crease into the brightness scalar before quantization lets the ramp
equalization account for it.

**Ramp equalization.** The three shading terms multiply, so their product is
bunched; with plausible constants 88% of the figure landed on the darkest stop.
Thresholds are therefore cut at equal quantiles of the object's own brightness
distribution, so every stop carries a similar share of the surface for any
light rig.

**Incident weight.** At full weight a strongly directional source pushes the lit
side to the top of the ramp and the dark side to the bottom, leaving no tonal
room for occlusion or crease. Lowering it to 0.6 lets those terms carry more of
the ordering, which is how the figure reads solid from every orbit angle.

**Ordered dither** on the fractional ramp position, using the same 4x4 coverage
vocabulary the renderer already uses for one-sided penumbra and cavity
overlays. At 9x zoom it reads as noise; at hardware pitch it resolves to
intermediate tone.

### Rejected, with reasons

- **Hard screen-space silhouette outline.** Derived from the owner buffer, it
  needs no edge adjacency (the shell has ~7,800 edges against `RMB_MAX_EDGES`
  4,096) and it does fix figure/ground. It was rejected on appearance: a
  boundary-value stroke reads as ink laid over the model rather than as shade,
  and the silhouette is legible without it.
- **Character AO at boundary value.** Any occlusion cue allowed to reach
  `SEM_BLACK` stops reading as shade and becomes a permanent-marker line.
  Occlusion stays inside the brightness ramp, as a gradation.
- **Uniform ambient + binary lit bit** (58 budget) is genuinely cheaper and is
  the right shape for a CAST shadow boundary, but as the hero's only lighting it
  collapses the figure to near-uniform. Removed rather than left as a second,
  near-duplicate mesh lighting path.

### Verification

- warning-clean at `-Wall -Wextra -Wpedantic`: PASS
- ASan/UBSan on bundle 11 and on the full catalogue: PASS
- canonical seam `E4F108D3C424CCE3`: unchanged
- full 13-bundle / 31-route catalogue: byte-identical except bundle 11, which is
  the intended change

`ROOM_BUNDLE_CAPTURE_OWNER=<object index>` additionally writes owner-masked and
false-colour recess diagnostic frames, so the detected field can be inspected
directly, including values below the threshold that will not be drawn.
