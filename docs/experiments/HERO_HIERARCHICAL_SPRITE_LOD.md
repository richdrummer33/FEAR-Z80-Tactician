# Hierarchical transparent hero LOD

Status: temporary experiment branch. Not merged.

> **Correction, later pass.** Every cost figure in the original version of this
> document was produced with a shade distance that measured semantic indices
> in enum order rather than brightness order, which priced the darkest-to-
> brightest swap as the cheapest possible move. That is what made the
> rendered far views read as speckle. See `HERO_LOD_SHADE_METRIC.md`.

> **Second pass: the architecture changed.** The originally proposed small
> nested core (16 patterns) plus a mid-band refinement prefix was measured
> against the room's actual VRAM headroom and found badly undersized -- the
> room stays safe reserving up to 288 patterns (scheduled peak 36 of the
> roughly 48 ceiling), so there was no reason to keep the hero vocabulary at
> 16. Growing it exposed a second, more useful finding: **one vocabulary
> trained jointly on mid+far demands beats two independently-sized
> dictionaries of the same combined budget, at every size tested.** The mid
> and far views of the same object share enough structure that a shared
> dictionary amortizes it; splitting the budget re-learns that structure
> twice. The shipped policy is therefore a single flat vocabulary, not a
> nested core/refinement pair. The nested scheme is kept as a measured
> comparison point, not deleted.

## Executive result

- Near (R<32): keep the high-detail/co-designed hero path, unchanged.
- Mid (R=32) and far (R=36): **one shared 192-pattern transparent-sprite
  vocabulary**, trained jointly on both bands, resident once at boot.
- Angle changes AND mid/far distance changes are both sprite-attribute
  rewrites only. There is no second load stage, because there is nothing
  left to load.
- 192 patterns (6,144 bytes) is not an arbitrary choice: it is the largest
  vocabulary this VRAM layout can hold. Sprite tile id N reads unified
  pattern tile 256+N (sprite pattern base 0x2000), and the background name
  table begins at unified tile 448, so tile 191 is the last sprite id that
  does not read into it.
- The lit room remains an independently rendered background plane, now
  capped at 256 background patterns (448 - 192 reserved). Measured scheduled
  peak: 33 pattern uploads per route frame, against the roughly 48 ceiling.

The transparent sprite plane is still the reason this composes with the room
at all: an object-only background tile has no alpha, so its zero pixels
would overwrite the live room behind Doomguy. Sprite colour zero is
transparent.

## Shared vs. dedicated: the measurement that picked the architecture

Before committing to "one big shared vocabulary", the alternative was tested
directly: two independent dictionaries, one trained only on far (R=36)
demands and one only on mid (R=32) demands, sized so their combined bytes
matched the shared vocabulary at the same total budget. All four numbers
below are on the full 32-angle corpus.

| total bytes | shared far sil.% | dedicated far sil.% | shared mid sil.% | dedicated mid sil.% |
|---:|---:|---:|---:|---:|
| 2,048 | 12.27 | 14.34 | 11.17 | 12.38 |
| 3,072 | 10.96 | 13.39 | 9.89 | 11.38 |
| 4,096 | 10.10 | 12.03 | 8.88 | 10.47 |
| 5,120 | 9.36 | 11.29 | 8.24 | 9.68 |
| 6,144 | 8.64 | 10.25 | 7.55 | 9.04 |

Shared wins outright at every budget, on both bands. Splitting the budget
does not buy specialization worth having; it just throws away the structure
mid and far have in common.

This also means the ORIGINAL "hierarchy penalty" measurement -- how much a
small nested core costs versus a same-size flat dictionary -- was measuring
the wrong tradeoff. The relevant comparison was never "nested vs flat at a
fixed small size", it was "one shared dictionary vs two split dictionaries at
the size the hardware can actually afford". Once asked that way, the answer
was one-sided.

## Measured hierarchical codebook: what a small core costs when kept small

Retained for reference, since the finding above only makes sense next to it.
Nesting a small far-only core inside a larger mid/far dictionary costs a
penalty that tracks the ABSOLUTE size of the frozen core, not its share of
the total budget (measured, corrected metric, full 32-angle corpus):

| far core | total patterns | penalty vs. best flat dictionary of the same total size |
|---:|---:|---:|
| 16 | 32 | 3.53% |
| 16 | 64 | 1.80% |
| 32 | 64 | 7.59% |
| 32 | 128 | 7.18% |
| 64 | 128 | 11.87% |

A small core (16) nests almost for free. A core large enough to be useful on
its own (32-64) pays 7-12% to stay nested. Since the hardware can afford one
large shared vocabulary outright, this tradeoff no longer needs to be paid at
all -- it is documented here so a future change that reintroduces a small
resident "always on" core (for a use case nesting genuinely serves, such as a
third, even-farther band) has the real cost in front of it.

## Shipped vocabulary: quality vs. size

Shared vocabulary, trained jointly on R=32+R=36 demands, scored separately per
band, full 32-angle corpus, corrected (brightness-order) shade metric:

| patterns | bytes | room scheduled peak | far silhouette err% | far gross tonal err | mid silhouette err% |
|---:|---:|---:|---:|---:|---:|
| 16 | 512 | 22 | 17.22 | 86 | -- |
| 64 | 2,048 | 23 | 12.27 | 273 | 11.65 |
| 96 | 3,072 | 24 | 10.96 | 448 | 10.35 |
| 128 | 4,096 | 26 | 10.10 | 416 | 9.41 |
| 160 | 5,120 | 27 | 9.36 | 389 | 8.73 |
| **192** | **6,144** | **29** | **8.64** | **421** | **8.02** |
| 224\* | 7,168 | 31 | 8.11 | 308 | 7.49 |
| 256\* | 8,192 | 33 | 7.68 | 311 | 7.13 |

\* Beyond the current 192-pattern hardware ceiling; would need a different
sprite-base/name-table layout. Diminishing returns are already visible by
192 (each +32 patterns buys roughly 0.5-0.7 points), so this was not pursued.

192 was shipped: it is the largest the current VRAM layout can hold, room
impact stays comfortably safe (scheduled peak 29 of ~48), and the visual
result at that size resolves the specific defect that motivated this pass --
see below.

## The visual proof

The original small (16-pattern) far core produced blocky, rectangular
extrusions on the worst-error views: the raised-arm/rifle silhouette at one
extreme angle, and a filled-in gap between the legs at another. Both are
angles where a fine limb crosses open space, which is exactly where a tiny
shared vocabulary runs out of shapes to represent it with.

Reconstructed at 16 / 64 / 192 shared patterns, same four worst-offender
angles, against the independently-rendered oracle:

```
oracle -> 16p/512B -> 64p/2048B -> 192p/6144B
```

At 16 patterns the arm reads as a stub and the leg gap is filled solid. At 64
both are recognizable but soft. At 192 the silhouette is close to the oracle
and both defects are gone. Diagnostic images are generated by
`tools/diagnose_lod_shade_metric.py` and the worst-offender ladders in
`build/hier/diag/` during development; the CI workflow uploads equivalent
artifacts from a fresh bake.

## Sprite footprint

Best phase is chosen offline per view. This is unaffected by vocabulary size
-- footprint is how the FIGURE tiles, not how many distinct patterns back it.

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
scanline allowance in the worst views. The measured max of 8 sprites on a
scanline at R=24/R=28 is already at the GG/SMS hardware limit; this is a
pre-existing constraint of the object's own silhouette, not something the
vocabulary size changes.

## VRAM impact on the independently lit room

The room compositor was rebaked with reduced effective background tile
capacity while preserving all other room/content logic. The ordinary
one-name-table capacity is 448 patterns.

Two-route total pattern loads and scheduled peak:

| room limit | reserved vs 448 | total loads | scheduled peak |
|---:|---:|---:|---:|
| 448 | 0 | 5,820 | 21 |
| 416 | 32 / 1 KiB | 5,870 | 22 |
| 384 | 64 / 2 KiB | 5,920 | 23 |
| 352 | 96 / 3 KiB | 6,001 | 24 |
| 320 | 128 / 4 KiB | 6,060 | 26 |
| 288 | 160 / 5 KiB | 6,137 | 28 |
| **256** | **192 / 6 KiB** | **~6,200** | **33** |

(256/192 row measured locally with the same harness as the workflow; see
`hero-room-vram-impact-fast.yml`, which now sweeps this point in CI.)

Name-table changed words remain constant across all variants. Even reserving
the full 192-pattern shipped vocabulary, the room stays well below the
roughly 48 pattern-upload/VBlank target -- nineteen pattern-loads of margin
remain at the measured peak.

## Comparison with the earlier per-angle 32-pattern result

Earlier experiment (superseded, kept for scale contrast):

- 32 extra patterns (1 KiB) tailored to one angle.
- Approximately 43% average reduction in far-tile error across four tested
  angles compared with using only that angle's near vocabulary.
- Strong local quality, but each angular sector wants its own dictionary --
  32 angles x 1 KiB = 32 KiB of pattern dictionaries in ROM, and changing
  angular sectors could require replacing up to 32 patterns live.

Shipped shared approach:

- One 192-pattern / 6 KiB vocabulary covers every tested angle at both R=32
  and R=36.
- Angle changes AND mid/far distance changes require zero pattern uploads.
- Per-view state is sprite references/positions only: mean SAT payload
  ~93.7-117.2 bytes per view (far/mid), well inside a single VBlank's
  transfer budget.

The trade is the same as before, just at a size that no longer bites: a
shared vocabulary cannot match a codebook retrained per angle at equal
pattern count. The measured advantage is ROM scaling, zero angle- or
distance-dependent pattern bandwidth, and clean compositing with a live room.

## Z80 / VDP interpretation

- No image reconstruction, filtering, wavelet decode, or pixel synthesis runs
  on the Z80.
- Angle change AND mid/far distance change are both lookup + sprite attribute
  updates -- `hero_lod_apply_view(band, angle)`, one call, no branch on
  "do I need to load anything".
- The entire 192-pattern / 6,144-byte vocabulary loads once, at boot, with
  the display blanked (`hero_lod_load_vocabulary()`), so it costs zero
  visible-frame bandwidth.
- The expensive solver, VQ, and vocabulary training remain PC-side baking
  work; see `tools/analyze_hierarchical_sprite_lod.py` and
  `tools/build_hierarchical_sprite_lod_pack.py --mode flat`.

## Lighting / room integration

Unchanged from the nested-scheme analysis:

The room remains the background renderer and keeps its baked/coarse-lattice
lighting, cast shadows and dirty name-table/tile updates.

The hero sprite corpus contains the hero's authored/baked semantic shading;
it does not yet dynamically inherit arbitrary room light direction. The
immediate integration options are:

1. preserve the hero's baked/ambient semantic shading while room lighting
   moves;
2. palette-transform the hero for cheap global brightness changes;
3. later train direction-class or light-class versions of the shared
   dictionary if directional relighting is worth the extra vocabulary.

World-space floor/cast shadow should remain a room/background effect. It is
view-independent and should not be multiplied through the hero sprite
vocabulary.

Foreground occlusion is a separate integration constraint. Sprites naturally
compose over the background, but room surfaces which must pass in front of
Doomguy need priority/clipping handling. The proof deliberately places
Doomguy in an open room region; general occlusion is not yet claimed solved.

## Shipped runtime policy

- R < ~32: high-detail background/dynamic hero path, unchanged.
- R >= ~32 (both mid and far bands): the same 192-pattern / 6,144-byte
  transparent sprite vocabulary, loaded once at boot.
- Angle changes: sprite map/reference changes only.
- Mid/far distance changes: sprite map/reference changes only -- there is no
  "approach the boundary, prefetch a refinement layer" step, because nothing
  needs to be prefetched.
- World-space floor/cast shadow stays a room effect, independent of hero
  view.

Runtime source: `src/main_hero_sprite_lod_flat_room_gg.c`. The original
nested-scheme proof (`src/main_hero_sprite_lod_room_gg.c`, 16+16 patterns)
is kept unmodified as a comparison ROM; both build and run in CI.

## Future work, not attempted here

- **Beyond 192 patterns** needs a different VRAM layout (moving the name
  table, or reworking how the background renderer's own 448-tile budget is
  numbered). The quality curve above shows real but shrinking returns past
  this point (224: 8.11%, 256: 7.68%); worth revisiting only if a specific
  view is still found wanting after 192 ships.
- **A third, farther band** (R>36) is exactly the case where a small nested
  "always resident" core would pay for itself again, since the penalty table
  above shows a 16-pattern core nests almost for free. Not measured yet
  because the current chamber's orbit tops out at R=36.
