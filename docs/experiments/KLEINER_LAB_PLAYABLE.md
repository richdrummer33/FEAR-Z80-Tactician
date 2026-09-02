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
- 40 legal positions after obstacle rejection,
- 640 random-access camera states.

The blocked regions match the largest visible masses: the twin teleporter,
server rack, and desk/HEV work area. This is not an automated camera rail.

Controls:
- D-pad up/down: forward/back,
- D-pad left/right: turn,
- button 1 / A: strafe left,
- button 2 / B: strafe right,
- Start: reset to the overview position.

## Why this branch

The older route-bundle renderer can reproduce a prettier authored camera but
its state is sequential. The playable Doomguy branch already solved the harder
interaction property needed here: arbitrary next-state order without corrupting
the persistent tile cache. Kleiner's lab reuses that architecture and spends ROM
on one frozen room instead of on route history.
