# Doomguy floor-mount, 2x scale

Status: temporary experiment branch. Not merged. Opt-in via
`-DROOM_BUNDLE_DOOMGUY_FLOOR_MOUNT=1`; every other Doomguy proof is
unaffected (`ROOM_BUNDLE_DOOMGUY_FLOOR_MOUNT` defaults to 0).

## Missing input for this pass

The requested asset, `hunyuan_diorama_vertical_112-optimized.glb`, was
described in the handoff but never actually reached this session -- no file,
no system attachment, nothing on disk or in the uploads directory. Everything
below was built and measured against the placeholder already in the repo,
`assets/gg-hero/FullDoomguyclassic-single-notex.glb` (imported at
`--height 19 --up z`, the same parameters every existing Doomguy CI workflow
uses), so the mechanism could be proven and shipped without waiting.

**The exact numeric results here (mesh height, min-clearance, tile-transport
cost) are specific to this placeholder's proportions and must be re-measured
once the real diorama is provided.** `ROOM_BUNDLE_MESH_BOUNDS_PROBE=1` (see
below) makes that a one-command re-derivation, not a re-investigation.

## What changed

Three things were requested: remove the plinth, ground the statue to the
floor with no air gap, and scale it 2x. All three are one flag now,
`ROOM_BUNDLE_DOOMGUY_FLOOR_MOUNT=1`:

- `make_doomguy_hero_chamber` no longer adds the 10x9x1.5 plinth box when the
  flag is set.
- `add_doomguy_proxy_mesh`'s transform Z-translate changes from 3.0 (plinth
  top) to `ROOM_BUNDLE_DOOMGUY_FLOOR_Z` (0.0, the floor). `apply_xf` scales
  before it translates, and the importer already anchors the mesh's own
  minimum bound at local Z=0, so this is flush by construction at any scale
  -- not a number that has to be re-tuned if the scale changes.
- The room-placement scale goes from the fixed 1.35 to
  `ROOM_BUNDLE_DOOMGUY_SCALE` (2.70 default -- literally double the previous
  art-direction factor, since "2x" was requested as a further pass over the
  existing placement, not a replacement of it).

Measured (`ROOM_BUNDLE_MESH_BOUNDS_PROBE=1`, placeholder asset):

    DOOMGUY_MESH_BOUNDS min=(59.99,7.77,0.03) max=(96.01,40.22,51.30)
    scale=2.7000 floor_z=0.0000 floor_mount=1

`min.z=0.03` is Q8 vertex quantization noise (roughly 0.01 units in the
mesh's own local space), not an air gap -- three orders of magnitude below
anything a 160x144 screen can show. The room-bake CI step asserts `min.z <
0.1` so this stays a guarantee, not an observation.

## A real bug, found by this change

Removing the plinth doesn't just remove a box -- it renumbers every mesh
object that comes after it. `rmb_render` owners are `0x80 + object creation
index`; the plinth was object 0, so the visual mesh was object 1, owner
`0x81`. Eight call sites across three functions
(`train_doomguy_codec`, `doom_play_position_framed`,
`bake_doomguy_playable`'s capture code, `HERO_CORPUS_OWNER`) hardcoded that
`0x81` directly. With the plinth gone the visual mesh becomes object 0,
owner `0x80` -- every one of those sites was silently looking at an owner ID
nothing painted to anymore.

The symptom was total silence, not a crash: `tsp_host_composite_owner_pixel_count(0x81u)`
returned 0 everywhere, including from camera positions standing well inside
the room looking straight at the statue, which is what made this findable --
a real framing bug would show SOME views working. `train_doomguy_codec` was
the canary (`fatal: hero codec training found no enclosed cells`, from
`tools/polar_baked_composite.c:1480`).

Fixed with one named constant, `ROOM_BUNDLE_DOOMGUY_VISUAL_OWNER`, derived
next to the object-creation code it depends on and used at all eight sites
instead of the literal. It resolves to `0x81u` in plinth mode (unchanged)
and `0x80u` in floor-mount mode. A future reordering only needs to update
one derivation, not grep the file for `0x81`.

## Min distance: "tippy top at the top of the screen"

`project()` in `room_mesh_bake.c` puts screen_y at
`RMB_CY - (worldZ - camZ) * RMB_FOCAL / d` (72 and 80). Setting screen_y to 0
(the top row) and solving for d:

    d = (topZ - eyeZ) * 80 / 72

With the measured topZ=51.30 and the eye height already used throughout this
codebase for Doomguy (16.0, matching `HERO_CORPUS_EYE_Z` and the playable
grid's `p.z`): d = 39.22, rounded to **39.0** for a hair of margin.

This one constant, `ROOM_BUNDLE_DOOMGUY_MIN_CLEARANCE`, now drives every
place in the file that used to hardcode a "how close can the camera get"
number for bundle 11:

- `doom_play_position_valid` -- the playable grid's inclusion radius (was a
  literal 22.0, unrelated to any framing derivation).
- `doomguy_quality_clamp` -- pushes any route-bake pose outside this radius.
- `showcase_detail_pose`'s bundle-11 case -- now an exact **circle** at this
  radius (was an ellipse, rx=22/ry=27, sized by feel) so the review orbit's
  closest approach is the same distance the math was solved for, at every
  angle.
- `train_doomguy_codec`'s three training bands -- now `MIN_CLEARANCE + 6*band`
  (was a fixed `26.0 + 6*band`, tuned for the old, much smaller statue).

All four are gated on `ROOM_BUNDLE_DOOMGUY_FLOOR_MOUNT`; the plinth-mode
literals (22.0, the 22/27 ellipse, `26.0 + 6*band`) are untouched.

## Known limitation: the playable grid doesn't fit this placeholder at 2x

The route bake and its review video (below) both pass. The **strict**
`DOOM_PLAY_*` walkable-lattice pack (`ROOM_BUNDLE_PLAYABLE=1`) does not --
`doom_play_grid_connected` fails.

Root cause, not a tuning miss: `doom_play_position_framed` rejects a
position independently of `doom_play_position_valid`'s distance check --
raising `MIN_CLEARANCE` only trims the near side of the grid, it cannot
rescue a position that clips because the object is simply too tall from
that angle. On the original 8x8 lattice, at this placeholder's 2x-scaled
height, an entire row near the room's south pillars clips regardless of
clearance, and disconnects the remaining ring. This was verified not to be
a min-clearance-value problem by sweeping 22 through 46 and by widening the
grid to 10x10 and 12x12 (both still failed, the wider grid additionally
colliding with the corner pillars). The room itself -- 80 units wide,
36..116 in X -- is the binding constraint: even the exact framing distance
(39.22) is within 2 units of the room's east-west half-width, leaving no
margin for the off-axis views that don't look dead-on at the statue.

This is very likely a placeholder-asset artifact, not a fundamental
conflict: the actual target's import height (12.2, per the prior tri-run
doc) is roughly 64% of this placeholder's 19, so its 2x-scaled height would
be proportionally smaller too, and may clear the room's own limits without
any further change. Re-run `ROOM_BUNDLE_MESH_BOUNDS_PROBE=1` once the real
asset is imported before deciding whether the room needs widening.

## Route bake / review video

`ROOM_BUNDLE_ONLY=11 ROOM_BUNDLE_CAPTURE_REVIEW=1` (with
`ROOM_BUNDLE_SCHEDULER_MAX_UPLOADS=512` -- see Performance below) produces
120 frames of the closest-approach orbit plus the authored traversal route.
Visually: floor-flush with no plinth and no gap, consistent framing with the
top close to the top of the screen across the whole orbit.

Screenshots and a 120-frame GIF (this environment could not install ffmpeg
locally -- an apt mirror 404 -- so the CI workflow below does the MP4 encode
instead) were sent alongside this doc.

## Performance: this is the actual cost of 2x

|  | plinth, 1.35x (unchanged) | floor-mount, 2.70x |
|---|---:|---:|
| scheduled_peak (0->1) | 55\* | 67 |
| scheduled_peak (1->0) | 56\* | 68 |
| tile_loads (0->1) | 5340\* | 5497 |
| changed_words | 15395\* | 13970 |

\*From the prior material-form tri-run doc, same placeholder mesh, 1.35x
scale -- not independently re-measured here since that path is intentionally
untouched.

Both configurations need `ROOM_BUNDLE_SCHEDULER_MAX_UPLOADS=512` to bake at
all with the real (non-fixture) asset -- confirmed pre-existing by building
the unmodified `da8f987` source against the same asset, so this is not a
consequence of floor-mount or the scale change, just of using the real mesh
instead of the tiny synthetic fixture some older CI jobs import.

scheduled_peak of 67-68 is roughly 40% above the ~48-pattern/VBlank ceiling
this whole project has budgeted against everywhere else. Doubling the linear
scale roughly quadruples projected screen area, and tile churn scales with
it. This is real and worth knowing before committing to 2x for anything
that has to hit that ceiling; it does not block the host bake, video, or
screenshots, all of which are unconditional host-side work with no VBlank
budget of their own.

## Reproduce

    node tools/glb_rmb/convert.mjs assets/gg-hero/FullDoomguyclassic-single-notex.glb \
      tools/generated/doomguy_mesh.inc --name doomguy --height 19 --up z \
      --visual-tris 5200 --shadow-tris 200 --recess-radius 0.5

    cc -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror \
      -DROOM_BUNDLE_DOOMGUY_GENERATED -DROOM_BUNDLE_DOOMGUY_FLOOR_MOUNT=1 \
      -Isrc -Itools tools/polar_baked_composite.c tools/room_mesh_bake.c \
      tools/room_bundle_poc_gen.c -lm -o build/bake

    ROOM_BUNDLE_MESH_BOUNDS_PROBE=1 build/bake build/out   # bounds + derivation inputs
    ROOM_BUNDLE_ONLY=11 ROOM_BUNDLE_SCHEDULER_MAX_UPLOADS=512 \
      ROOM_BUNDLE_CAPTURE_REVIEW=1 build/bake build/out    # route bake + 120 review frames
