# Doomguy floor-mount, 1.8x scale

Status: temporary experiment branch. Not merged. Opt-in via
`-DROOM_BUNDLE_DOOMGUY_FLOOR_MOUNT=1`; every other Doomguy proof is
unaffected (`ROOM_BUNDLE_DOOMGUY_FLOOR_MOUNT` defaults to 0).

File name kept from the initial pass (originally a 2x request); the shipped
scale is 1.8x, see below.

## The asset

`assets/gg-hero/hunyuan_diorama_vertical_112-optimized.glb` -- a flowing,
wide-skirted figure on a flared base, 185365 vertices / 248608 triangles,
with a base-colour texture, normal map, and metallic-roughness texture (no
dedicated occlusion map), matching the prior material-form tri-run's
description. An earlier pass on this branch had to proceed without this
file (it never actually reached that session despite being referenced) and
used the older `FullDoomguyclassic-single-notex.glb` as a stand-in; that
placeholder's numbers are gone from this document now that the real asset
is in hand. It is committed at `assets/gg-hero/` alongside the existing
Doomguy/Purple-Prism assets so CI can import it directly, the same
convention every other Doomguy proof already uses.

Import: `--up y --height 12.2 --visual-tris 5200 --shadow-tris 200
--recess-radius 0.5` (unchanged from the prior material-form tri-run).
Shell: 2560 vertices / 5194 triangles visual, 90/188 shadow.

## Bottom-face check

Requested directly: verify no downward-facing geometry sits below the
floor. Measured on the actual emitted Q8 vertex data (not just the overall
bounding box, which by construction cannot be negative since it defines the
anchor):

| shell | min Z (local, pre-scale) |
|---|---:|
| visual | 0.0117 |
| lighting (unused at these renderer settings) | 0.4453 |
| shadow | 0.0547 |

All three are at or fractionally above zero -- nothing dips below the
anchor. This mattered to check rather than assume: the importer anchors
`z0` to the RAW source mesh's minimum (`tools/glb_rmb/convert.mjs`, `norm.z0
= b.min[2]`, computed before decimation), and each of the visual/lighting/
shadow shells is independently decimated afterward from a fresh read of the
same input. Quadric-based mesh simplification is not guaranteed to stay
within the original silhouette -- an edge collapse can in principle move a
vertex past the original extremum, which would show up as exactly the
"some faces below the floor" the request was checking for. It happens not
to occur here, but that is a property of this decimation run, not a
guarantee `convert.mjs` enforces; if a future asset trips it, clamp each
shell's own minimum into `norm.z0` (`min(raw_min, min(all shell mins))`)
rather than trusting the raw source's bound alone.

World-space, after placement (`ROOM_BUNDLE_MESH_BOUNDS_PROBE=1`):

    DOOMGUY_MESH_BOUNDS min=(61.89,10.56,0.03) max=(94.14,37.36,29.55)
    scale=2.4300 floor_z=0.0000 floor_mount=1

`min.z=0.03` is that same 0.0117-unit visual-shell residual times the 2.43
scale -- three orders of magnitude below anything a 160x144 screen can
show. The CI workflow asserts `min.z < 0.1` so this stays a guarantee.

## Scale: 1.8x, not 2x

`ROOM_BUNDLE_DOOMGUY_SCALE` is `1.35 * 1.8 = 2.43` (revised down from an
initial 2x request, applied as a further pass over the existing 1.35
art-direction factor, same as the original request).

## Min distance, the hard way

The closed-form distance for "tippy top at screen_y=0" is still
`d = (topZ - eyeZ) * 80 / 72` (see `project()` / `RMB_CY=72` /
`RMB_FOCAL=80` in `room_mesh_bake.c`). With the measured topZ=29.55 and
eyeZ=16.0 that is d=15.05 -- and it is wrong for this asset. This is a
wide, flared diorama, not a narrow upright figure: at d=15.05 the camera
sits inside the piece's own silhouette. Confirmed by rendering it, not
just by comparing to the mesh's XY half-extent (which itself
underestimated the real requirement -- a naive bounding-box radius put a
safe distance around 21, and 22 still rendered as clearly inside the
geometry).

The actual value was found by rendering candidates and looking at them:
15, 17, 19, 22, 24, 26, 28, 30, 40, 50 world units, at multiple angles
around the orbit. 15 through 26 all still clipped through geometry at one
angle or another. 50 broke the route bake outright (`route terminal seam
name table != canonical seam` -- `doomguy_quality_clamp` pushing an
ordinary traversal pose that far out corrupts the shared seam, not just
the orbit). **28** was the smallest value clear of the geometry at every
tested angle (0/15/30/45/60/75/90 degrees): the closest approach lands the
tippy top just short of the top row, matching "give or take," and the rest
of the orbit has comfortable headroom.

`ROOM_BUNDLE_DOOMGUY_MIN_CLEARANCE=28.0` drives, as one constant, what used
to be independently hand-tuned numbers: the playable grid's inclusion
radius (`doom_play_position_valid`), the route bake's closest-approach
clamp (`doomguy_quality_clamp`), the review orbit's radius
(`showcase_detail_pose`, now an exact circle at this distance instead of a
by-feel ellipse), and the codec's three training bands
(`train_doomguy_codec`, now `MIN_CLEARANCE + 6*band`).

## A real bug, found removing the plinth (unchanged from the first pass)

Removing the plinth renumbers every mesh object created after it --
`rmb_render` owners are `0x80 + creation index`. The plinth was object 0,
so the visual mesh was object 1, owner `0x81`; with the plinth gone the
visual mesh becomes object 0, owner `0x80`. Eight hardcoded `0x81` sites
across three functions (`train_doomguy_codec`, `doom_play_position_framed`,
`bake_doomguy_playable`'s capture code, `HERO_CORPUS_OWNER`) were left
pointing at nothing. Symptom was total silence -- `owner_pixel_count`
returning 0 from every camera angle, including ones standing well inside
the room -- caught by the codec trainer's "no enclosed cells" die().

Fixed with one named constant, `ROOM_BUNDLE_DOOMGUY_VISUAL_OWNER`, derived
next to the object-creation code it depends on (`0x81u` in plinth mode,
unchanged; `0x80u` in floor-mount mode) and used at all eight sites instead
of the literal.

## Known limitation: the playable grid, for a different reason than before

The route bake and its review video both pass. The strict `DOOM_PLAY_*`
walkable-lattice pack (`ROOM_BUNDLE_PLAYABLE=1`) does not --
`doom_play_grid_connected` fails, same as with the placeholder asset, but
for a different underlying reason now that the real asset is in hand.

The placeholder was too TALL and clipped the top of the screen. This asset
clips the BOTTOM (`y1=143`) at many off-axis grid positions -- its flared
base is wide enough that being far enough away on the min-clearance circle
is not sufficient at an angle that does not look straight at the piece.
Confirmed this is a base-width problem, not a distance problem, by sweeping
`MIN_CLEARANCE` from 22 through 40: the same style of exclusion recurs at
every value tested, because raising clearance moves the camera further
along a ray that still does not point at the object, rather than fixing
the angle. `doom_play_position_valid` only checks distance from the
statue's center; it has no per-position framing check of its own (that is
`doom_play_position_framed`'s job, and it is already doing exactly what it
should -- rejecting the bad positions). Fixing the grid would need either a
wider room or teaching `doom_play_position_valid` to reason about framing,
not just distance; not attempted here.

The route bake and review video are unaffected: `doomguy_quality_clamp`
pushes a pose outward along its existing direction rather than excluding
it, so it cannot disconnect a ring that does not exist for that code path.

## Route bake / review video

`ROOM_BUNDLE_ONLY=11 ROOM_BUNDLE_SCHEDULER_MAX_UPLOADS=512
ROOM_BUNDLE_CAPTURE_REVIEW=1` produces 120 frames of the closest-approach
orbit plus the authored traversal route.

    ROOM_BUNDLE_STATS bundle=11 route=0->1 frames=192 patch_bytes=43403
      tile_bytes=196472 tile_loads=5756 raw_peak=93 scheduled_peak=58
      scheduled_budget=58 atomic_unsafe_peak=58 changed_words=15388
    ROOM_BUNDLE_STATS bundle=11 route=1->0 ... scheduled_peak=58 ...
    ROOM_BUNDLE_POC_PASS bundles=1 ordinary_routes=2 split_routes=6
      stair_routes=2 frames_per_route=192 canonical=E4F108D3C424CCE3

`ROOM_BUNDLE_SCHEDULER_MAX_UPLOADS=512` is required regardless of
floor-mount or scale -- confirmed pre-existing by building the unmodified
pre-floor-mount source against this same real (non-fixture) asset: the
default search budget is sized for the tiny synthetic fixture some older
CI jobs import, not for a ~5200-triangle real mesh.

scheduled_peak=58 is roughly 20% above the ~48-pattern/VBlank ceiling this
project budgets against elsewhere, and lower than the placeholder-asset
pass's 67-68 despite the larger, more detailed mesh -- the shorter overall
height (29.55 vs 51.30 for the placeholder at its own scale) means less of
the screen churns per frame even though the silhouette itself is more
complex. Real, worth knowing, does not block the host bake, video, or
screenshots (all unconditional host-side work with no VBlank budget of
their own).

## Reproduce

    node tools/glb_rmb/convert.mjs \
      assets/gg-hero/hunyuan_diorama_vertical_112-optimized.glb \
      tools/generated/doomguy_mesh.inc --name doomguy --up y --height 12.2 \
      --visual-tris 5200 --shadow-tris 200 --recess-radius 0.5

    cc -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror \
      -DROOM_BUNDLE_DOOMGUY_GENERATED -DROOM_BUNDLE_DOOMGUY_FLOOR_MOUNT=1 \
      -Isrc -Itools tools/polar_baked_composite.c tools/room_mesh_bake.c \
      tools/room_bundle_poc_gen.c -lm -o build/bake

    ROOM_BUNDLE_MESH_BOUNDS_PROBE=1 build/bake build/out   # bounds + derivation inputs
    ROOM_BUNDLE_ONLY=11 ROOM_BUNDLE_SCHEDULER_MAX_UPLOADS=512 \
      ROOM_BUNDLE_CAPTURE_REVIEW=1 build/bake build/out    # route bake + 120 review frames
