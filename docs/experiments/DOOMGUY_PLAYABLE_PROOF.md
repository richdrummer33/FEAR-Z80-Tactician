# Doomguy playable chamber proof

Status: random-access Game Gear ROM proof. This is a stepping stone toward Hero
View Codec v1, not a replacement architecture for it.

## What this proves

The host baker produces 608 self-contained display states: 38 legal camera
positions times 16 yaw headings. A state may be requested in any order. It owns
an absolute 20x18 visible name table and a contiguous block of dynamic tile
patterns, so runtime movement never depends on replaying an authored route.

The room, Doomguy, plinth, window light, occlusion and cast shadow are baked
together. This is intentionally useful for the first playable ROM because it
proves the camera envelope and actual target-side data transport without first
solving object/world composition.

It is not yet the final reusable Doomguy codec. The finished architecture still
separates world rendering from the object's angle/distance appearance data and
keeps the far-wall cast shadow in the world bake.

## Quality and movement envelope

The old 22-world-unit radius remains the first quality limit. The generator now
also checks every candidate position against actual room segments using a
2-world-unit player radius, renders the nearest aimed yaw, and rejects any
position where Doomguy touches a screen edge.

Two near-quarter positions fail that framing test and are removed:

- grid (6,5), world (98,36);
- grid (2,6), world (66,44).

The remaining 38-position graph is connected under the runtime's eight-neighbour
movement. The measured nearest surviving distance is 23.324 world units. A
future authored plinth or velvet-rope barrier can follow this irregular quality
envelope rather than exposing an invisible circle.

For evaluation, every aimed state exports both an exact binary ownership mask
and an isolated Doomguy view over a saturated magenta background. This makes
silhouette loss unambiguous even where the real room and lit figure share
colours. The synthetic background never enters the ROM or changes the baked
world-relative lighting.

## Atomic target-side publication

VRAM below 0x3000 is divided into two 186-pattern dynamic pools plus the eight
resident hero patterns at tile IDs 376..383. VRAM then contains two complete
name tables at 0x3000 and 0x3800.

While one page is visible, the next state's patterns are uploaded to the other
pool. Its name table is uploaded invisibly in two nine-row VBlank slices. A
single VDP R2 register write then reveals the completed page. The visible image
can therefore never reference an uninitialized pattern or a half-written map.

The pack currently measures 145 peak dynamic patterns and about 70.20 on
average. Pattern uploads remain capped at 48 per scheduling phase. The compact
balanced ROM-bank dispatcher resolves one of 177 data banks in about
eight comparisons rather than walking three duplicated linear chains; this
also fixes the former 21,644-byte dispatcher overflow in a 16 KiB bank.

## Deliberately not claimed

- Position changes still jump in 8-world-unit increments.
- Yaw changes still jump by 22.5 degrees.
- Height is fixed and there is no pitch or roll.
- The bake stores the whole chamber, so its ROM cost is room-specific.
- It does not yet interpolate, warp or progressively refine between anchors.
- The eight-pattern dictionary was trained from the established hero routes;
  retraining and rating it over a denser object-relative angle/distance corpus
  remains part of the reusable codec work.
- Dynamic lighting is not a v1 requirement. Doomguy's light is world-relative
  and pre-baked; the rear-wall shadow remains ordinary world content.

The next architectural stage is to use this proof's quality envelope,
random-access selector, resident vocabulary and atomic page scheduler while
extracting Doomguy's appearance state from the room-wide frames. The general
Polar room renderer can then remain independent and Doomguy can become reusable
across rooms.
