# Kleiner's Lab playable reconstruction

This experiment branches from `experiment/gg-doomguy-playable` at
`98a680689189`, because that branch is the first room-class proof with truly
random-access camera states and D-pad traversal. It does not depend on sequential
route deltas.

## Source reduction

Reference: the canonical Half-Life 2 `d1_trainstation_05` BSP supplied for the
experiment. The iconic final lab crop is approximately Source X -7312..-6352,
Y -1664..-1024. The reconstruction maps that rectangle at roughly one tenth
scale into a 96x64 world-unit Game Gear room.

The intent is semantic reconstruction, not BSP fidelity:

- twin orange teleporter columns -> two eight-sided coloured shells + dark rings,
- teleporter console / server rack / lockers / HEV case -> coarse boxes,
- Kleiner -> six primitive masses with a bright lab-coat value structure,
- desk / monitors -> coarse boxes over one accent rug,
- workshop slats -> three bright horizontal bands,
- brick/plaster and tiled floor -> large value bands and a 4x4 floor checker.

No Source texture is stored. Texture detail is deliberately pushed down to
features that survive 160x144 output and the existing tile codec.

For reproducible measurement, `tools/hl2_bsp_lab_extract.py` can read a local
Source BSP and emit a crop-only OBJ/reference manifest. Running it against the
supplied canonical BSP with the default crop selected 1,565 source faces,
triangulated to 4,672 triangles across 60 texture/material names. The GG scene
does not ship that geometry; those numbers are useful precisely because they
show how much source description is being thrown away before the bake.

## Colour strategy

The five-stop semantic brightness ramp remains intact, but the GG palettes are
changed from neutral greys to a dirty warm industrial ramp. Index 15 is reserved
as one direct authored accent. It is safe because the current mixed-light
encoding reserves 8..14 and leaves 15 unused.

For this first pass, index 15 carries both the orange teleporter shell and the
rug. This is intentionally vocabulary-limited rather than a general material
system.

## Traversal

The runtime uses the Doomguy playable branch's double-buffer random-access
scheme: every legal pose is self-contained, staged into the invisible VRAM pool
over as many VBlanks as needed, and then atomically published.

Grid:
- 9 x 6 candidate cells,
- 10 world-unit step,
- 16 yaw states (22.5 degrees each),
- 37 legal positions after the actual non-rectangular floor plan and large
  obstacle footprints are applied,
- 592 random-access camera states.

The second trace pass no longer treats the lab as a rectangle. The OBJ slice
defines a separate left teleporter chamber, the main lab, the stepped north
perimeter, and the smaller bottom-right HEV suit sub-room. Movement also checks
the wall segment between two legal grid nodes, so two valid destinations on
opposite sides of a partition cannot cross except through the measured doorway.
This is still not an automated camera rail.

Controls:
- D-pad up/down: forward/back,
- D-pad left/right: turn,
- button 1 / A: strafe left,
- button 2 / B: strafe right,
- Start: reset to grid (4,1), yaw 7. This is in the main lab, aimed through the
  real partition opening toward the twin teleporter chamber.

## Why this branch

The older route-bundle renderer can reproduce a prettier authored camera but
its state is sequential. The playable Doomguy branch already solved the harder
interaction property needed here: arbitrary next-state order without corrupting
the persistent tile cache. Kleiner's lab reuses that architecture and spends ROM
on one frozen room instead of on route history.


## Trace pass 2: measured interior topology

The first playable pass proved the random-access renderer but deliberately used
a rectangular shell. The second pass traces the dominant planes from the
extracted OBJ at a low horizontal slice (z=6), where Source door openings are
actually open.

The authoring transform is `world_x = obj_x + 36`,
`world_y = obj_y - 16`. Important measured planes are:

- teleporter/main partition: OBJ x=25.6 -> GG x=61.6,
- teleporter-room south/north: OBJ y=12.8 / 57.6 -> GG y=-3.2 / 41.6,
- tall main north wall: OBJ y=64 -> GG y=48,
- north-wall step: OBJ x=69.6 -> GG x=105.6,
- HEV-room west wall: OBJ x=68.8 -> GG x=104.8,
- east perimeter: OBJ x=96 -> GG x=132.

Measured openings are retained too: the teleporter partition doorway is GG
y=3.6..20.4 and the HEV room doorway is GG x=114.0..124.4. The camera eye is
lowered from z=16 to z=8 so those Source-scale openings are actually viewable
rather than hidden behind their lintels.

The trace also keeps several height cues instead of flattening everything to one
ceiling: the teleporter chamber reaches about z=34.8, the main room z=32, the
north wall steps down on the east side, and broad z=25.6..28.8 truss /
z=16..18.4 duct bands become coarse overhead masses.

`tools/hl2_lab_obj_trace.py` regenerates slice-plane measurements from the
extracted OBJ before future wall changes.
