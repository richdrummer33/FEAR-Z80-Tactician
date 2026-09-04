# Hierarchical transparent hero LOD

Status: temporary experiment branch. Not merged.

> **Correction, later pass.** Every cost figure below was produced with a shade
> distance that measured semantic indices in enum order rather than brightness
> order, which priced the darkest-to-brightest swap as the cheapest possible
> move. That is what made the rendered far views read as speckle. See
> `HERO_LOD_SHADE_METRIC.md`. The architecture and the sprite/VRAM/runtime
> figures below all survive; the absolute error numbers were re-measured after
> the fix and are restated here:
>
> | | before (broken metric) | after (corrected) |
> |---|---:|---:|
> | far core 16, R=36 mean cost | 109.967 | 111.989 |
> | nested 16+16, mid/far mean cost | 100.763 | 99.611 |
> | hierarchy penalty at 32 total | 2.085% | 3.531% |
> | hierarchy penalty at 64 total | 3.071% | 1.796% |
> | far sprite refs mean / max | 28.81 / 36 | 28.0 / 35 |
> | mid/far sprite refs mean / max | 32.97 / 47 | 32.6 / 45 |
>
> The costs are in different units before and after, so only the penalties and
> the reference counts are directly comparable. The separate finding that the
> 16-pattern core is undersized relative to the measured room headroom is in
> the same document.

## Executive result

The distance codec has converged on a hardware-oriented hybrid rather than a
single representation for every range.

- Near: keep the high-detail/co-designed hero path.
- Mid (measured R=32): transparent 8x8 sprite representation using a shared
  resident vocabulary.
- Far (measured R=36): the same sprite representation, but only a 16-pattern
  core vocabulary is required.
- Approaching from far to mid enables an additional refinement prefix; changing
  viewing angle does not require replacing pattern graphics.
- The lit room remains an independently rendered background plane.

The transparent sprite plane is important. An object-only background tile has
no alpha, so its zero pixels would overwrite the live room behind Doomguy.
Sprite colour zero is transparent and therefore makes the object-only shared
vocabulary composable with the room without generating hero+room boundary
patterns.

## Measured hierarchical codebook

32 angular views, R=32/R=36 corpus.

Far core:

- 16 patterns = 512 bytes pattern graphics.
- R=36 mean weighted pattern error: 109.967.
- Equivalent upper-bound interpretation if all error were silhouette:
  9.16 mismatched pixels per used 8x8 pattern. Real error is a mixture of
  silhouette and shade error, so this is only a scale indicator.
- Mean 28.81 sprite references per view, max 36.
- Mean SAT payload ~87.4 bytes per view, max 109.
- Naive 32-angle map table: 2,798 bytes.

Far core + 16 mid refinements:

- 32 total patterns = 1,024 bytes pattern graphics.
- Nested mid/far mean weighted error: 100.763.
- Best independently trained flat 32-pattern dictionary: 98.705.
- Hierarchy penalty: only 2.085%.
- Mean 32.97 sprite references per view, max 47.
- Mean SAT payload ~99.9 bytes per view, max 142.
- Naive 64-view R32/R36 map table: 6,394 bytes.

Far core + 48 mid refinements:

- 64 total patterns = 2,048 bytes pattern graphics.
- Nested mid/far mean weighted error: 91.133.
- Flat 64-pattern dictionary: 88.418.
- Hierarchy penalty: 3.071%.

This is strong evidence that a prefix/nested vocabulary is practical: the far
representation can keep only a small core while the medium representation adds
more visual information, with very little loss versus optimizing each total
vocabulary independently.

## Sprite footprint

Best phase is chosen offline per view.

8x8 sprite coverage:

- R=24: does not fit reliably (15.6% of views fit); mean 73.8 sprites, max 95,
  max 10 on a scanline.
- R=28: 87.5% of views fit; mean 53.6, max 67, max 8 on a scanline.
- R=32: all 32 views fit; mean 41.2, max 51, max 7 on a scanline.
- R=36: all 32 views fit; mean 33.0, max 40, max 6 on a scanline.

8x16 sprite coverage lowers total sprite count but not scanline pressure:

- R=28: all views fit; mean 29.9 sprites, max 38, max 8/scanline.
- R=32: all views fit; mean 23.0, max 28, max 7/scanline.
- R=36: all views fit; mean 18.4, max 22, max 6/scanline.

R=32 is therefore the clean first 8x8 sprite handoff. R=28 remains an
interesting later 8x16 experiment but consumes the complete eight-sprite
scanline allowance in the worst views.

## VRAM impact on the independently lit room

The room compositor was rebaked with reduced effective background tile capacity
while preserving all other room/content logic. The ordinary one-name-table
capacity is 448 patterns.

Two-route total pattern loads and scheduled peak:

| room limit | reserved vs 448 | total loads | scheduled peak |
|---:|---:|---:|---:|
| 448 | 0 | 5,820 | 21 |
| 416 | 32 / 1 KiB | 5,870 | 22 |
| 384 | 64 / 2 KiB | 5,920 | 23 |
| 352 | 96 / 3 KiB | 6,001 | 24 |
| 320 | 128 / 4 KiB | 6,060 | 26 |
| 288 | 160 / 5 KiB | 6,137 | 28 |

Name-table changed words remained 10,560 in all variants. Even the 288-pattern
room cache remains well below the roughly 48 pattern-upload/VBlank target in
this room.

For the proposed single-page sprite LOD mode, the interesting point is 416:
a permanently resident 32-pattern hero dictionary raises total room pattern
loads by only ~0.86% and the route peak from 21 to 22.

A 16-pattern far-only core would reserve only 512 bytes and should sit between
the 448 and 416 cases; it has not yet required a separate cache-pressure run.

## Comparison with the earlier per-angle 32-pattern result

Earlier experiment:

- 32 extra patterns (1 KiB) tailored to one angle.
- Approximately 43% average reduction in far-tile error across four tested
  angles compared with using only that angle's near vocabulary.
- Strong local quality, but each angular sector wants its own dictionary.

Consequences if naively scaled:

- 32 angles x 1 KiB = 32 KiB of pattern dictionaries in ROM.
- 256 angles x 1 KiB = 256 KiB.
- Only 1 KiB need be in VRAM at once, but changing angular sectors can require
  replacing up to 32 patterns (1,024 bytes).
- A 32-pattern hero swap plus a busy ~21-pattern room frame exceeds the rough
  48-pattern combined target, so it needs prefetching/scheduling or multiple
  frames.

Current hierarchical shared approach:

- Far graphics vocabulary is 512 bytes once for all 32 tested angles.
- Mid/far graphics vocabulary is 1 KiB once for all 32 tested angles.
- Angle changes require no pattern uploads once the current distance prefix is
  resident.
- R=36 -> R=32 requires only the new 16-pattern / 512-byte refinement prefix.
  This transition is distance-driven and can be prefetched over approaching
  frames rather than paid on every turn.
- Per-view state becomes sprite references/positions. Current naive tables are
  ~2.8 KiB for 32 far views and ~6.4 KiB for 64 R32/R36 views before delta or
  angular thinning compression.
- Runtime hero work is roughly 90-105 bytes of sprite-attribute state per view,
  not kilobytes of pattern pixels.

The trade is image fidelity: a global shared vocabulary cannot match a
separately tailored per-angle codebook at the same pattern count. The measured
advantage is instead ROM scaling, virtually zero angle-dependent pattern
bandwidth, and clean compositing with a live room.

## Z80 / VDP interpretation

After loading the active prefix:

- No image reconstruction, filtering, wavelet decode, or pixel synthesis runs
  on the Z80.
- Angle change is lookup + sprite attribute updates.
- Far view averages ~87 bytes of sprite attribute payload.
- Mid/far view averages ~100 bytes in the 32-pattern hierarchical case.
- Pattern data is loaded only at LOD threshold transitions.
- The expensive solver, VQ, phase selection and nested dictionary design remain
  PC-side baking work.

The first R=32 refinement load is 16 patterns / 512 bytes. The measured room at
a 416-pattern cache peaks around 22 pattern uploads per route frame, so sixteen
additional hero pattern uploads still fit beneath the rough 48-pattern count
budget in isolation (22 + 16 = 38). A shipping implementation should still
schedule the refinement load during VBlank or prefetch it across several
frames rather than call a bulk VRAM upload at an arbitrary display time.

## Lighting / room integration

The room remains the background renderer and keeps its baked/coarse-lattice
lighting, cast shadows and dirty name-table/tile updates.

The current hero sprite corpus contains the hero's authored/baked semantic
shading; it does not yet dynamically inherit arbitrary room light direction.
The immediate integration options are:

1. preserve the hero's baked/ambient semantic shading while room lighting moves;
2. palette-transform the hero for cheap global brightness changes;
3. later train direction-class or light-class versions of the small resident
   dictionary if directional relighting is worth the extra vocabulary.

World-space floor/cast shadow should remain a room/background effect. It is
view-independent and should not be multiplied through the hero sprite
vocabulary.

Foreground occlusion is a separate integration constraint. Sprites naturally
compose over the background, but room surfaces which must pass in front of
Doomguy need priority/clipping handling. The first proof deliberately places
Doomguy in an open room region; general occlusion is not yet claimed solved.

## Proposed runtime policy

A practical first hierarchy is:

- R < ~32: high-detail background/dynamic hero path.
- R ~= 32: load/enable 16 refinement patterns, 32-pattern transparent sprite
  vocabulary total.
- R ~= 36 and beyond: use only the 16-pattern / 512-byte far core.
- Angle changes at R>=32: sprite map changes only; no pattern graphics swap.
- As the player approaches the R32 threshold: prefetch refinement patterns.
- As the player recedes: refinement slots may eventually be released back to
  the room cache if/when the runtime allocator supports that dynamic handoff.

This is structurally the original nested-distance idea at the hardware level:
coarse information persists, and additional visual vocabulary is layered in as
projected detail becomes useful.
