# Why the far sprite LOD looked like speckle

Status: temporary experiment branch. Not merged.

## Summary

The hierarchical sprite vocabulary was sound. The distance measure it was
optimizing was not.

The compositor's semantic shade indices are in the order they were added to the
enum, not in order of brightness. The two interstitial stops were appended after
the original three, so `SEM_FAR_MID` and `SEM_MID_NEAR` sit at indices 6 and 7
while belonging visually between 3/4 and 4/5. Every LOD tool measured "how wrong
is this shade" with raw index arithmetic, which measures distance in the wrong
space.

The consequence is not a small inaccuracy. In corpus codes the hero ramp runs
dark to light as 4, 7, 5, 8, 6. Raw index arithmetic therefore prices:

| pair | ramp stops apart | priced at |
|---|---:|---:|
| code 7 vs code 6 | 3 | **1** |
| code 4 vs code 5 | 2 | **1** |
| code 5 vs code 6 | 2 | **1** |
| code 4 vs code 7 | 1 | **3** |
| code 5 vs code 8 | 1 | **3** |

Codes 7 and 6 are the second-darkest and the brightest stop. They are 48% of all
hero pixels, three stops apart, and the quantizer was told that swapping one for
the other is the cheapest move available -- while being told that genuinely
adjacent stops are the most expensive thing it could touch.

A vector quantizer given that price list does exactly what it was asked to do:
it preserves distinctions that do not matter and trades dark for bright wherever
it can. That is the speckle.

## Measurement

Far band R=36, 8 angles, 16 shared patterns, identical pipeline, only the shade
distance changed. Both variants are scored in the corrected space so the
comparison is fair.

| trained with | silhouette error | shade exact | off by 1 | off by 2 | off by 3+ | mean stops wrong |
|---|---:|---:|---:|---:|---:|---:|
| raw index | 17.27% | 3044 | 2031 | 2230 | **1798** | 1.3205 |
| perceptual rank | 17.22% | 3368 | 4165 | 1539 | **86** | 0.8216 |

Gross tonal errors -- three or more ramp stops wrong, which is what the eye reads
as noise -- drop by 21x. Mean tonal error drops 38%. Silhouette accuracy is
unchanged, so this is not a trade: it is the same shape with the tone put back.

R=32 with 32 patterns behaves the same way: off-by-3-or-more falls from 2155 to
168, mean stops wrong from 1.2438 to 0.6385.

Cost of the fix: zero ROM, zero VRAM, zero runtime. The dictionary is the same
size, the sprite counts are the same, the resident bytes are the same. Only the
offline objective changed.

## The fix

`TileWeights` now carries a `rank` table mapping semantic code to its position
on the actual dark-to-light ramp, and `pixel_cost` measures distance there.
`_optimal_value` in the Lloyd centroid update now defers to `pixel_cost` instead
of duplicating the arithmetic, which is how the two drifted apart in the first
place. The shared-vocabulary score cache key includes the rank, or scores leak
between metrics.

`tests/test_shade_rank.py` re-derives the expected ordering from the
compositor's own preview colours and asserts the C enum still has the values the
table assumes, so a future ramp change breaks a test rather than silently
degrading the quantizer.

Raw-index behaviour is still reachable as `TileWeights(12.0, 1.0, None)`, purely
so the regression can be reproduced. `tools/diagnose_lod_shade_metric.py` runs
both and emits oracle / reconstruction / error sheets.

## What was left afterwards

With the metric corrected the reconstruction is structurally faithful but flat.
That is a genuine capacity limit, not a bug, and it is measurable.

R=36, 8 angles, silhouette weight 12:

| patterns | resident bytes | silhouette error | mean stops wrong |
|---:|---:|---:|---:|
| 16 | 512 | 17.22% | 0.822 |
| 24 | 768 | 14.65% | 0.776 |
| 32 | 1024 | 12.19% | 0.735 |
| 48 | 1536 | 10.25% | 0.672 |
| 64 | 2048 | 8.64% | 0.603 |
| 96 | 3072 | 5.69% | 0.413 |
| 128 | 4096 | 3.86% | 0.300 |

At 128 patterns the far reconstruction is close to indistinguishable from the
oracle. The already-measured room cache table says reserving 128 hero patterns
leaves the room at a scheduled peak of 26 against the roughly 48 per-VBlank
ceiling. **The far core at 16 patterns is undersized by around 8x relative to
the headroom that was already measured.**

Raising the silhouette weight is not the lever. At 16 patterns, doubling it from
12 to 24 makes silhouette accuracy slightly worse (17.22% to 17.85%) and tone
clearly worse. At 32 and 48 patterns it buys about 0.3 points of silhouette for
a 10% tonal regression. Leave it at 12.

Removing the ordered dither from the far bands is also not the lever, which is
worth recording because it is an obvious thing to try. Retraining on an
undithered corpus changes mean stops wrong from 0.7637 to 0.7520 -- about 1.5%,
i.e. nothing. At this codebook size the capacity is consumed by shape, not by
dither phase. The dither stays on, consistent with the earlier near-path finding.

## What this means for the nesting constraint

The hierarchy penalty was measured at several core and total sizes, all with the
corrected metric:

| far core | total | penalty vs best flat dictionary of the same total size |
|---:|---:|---:|
| 16 | 32 | 3.53% |
| 16 | 64 | **1.80%** |
| 32 | 64 | 7.59% |
| 32 | 128 | 7.18% |
| 64 | 128 | 11.87% |

The penalty tracks the absolute size of the frozen core, not the fraction of the
budget it occupies. A 16-pattern core has to stay generic, and generic patterns
happen to serve the mid band well. A 64-pattern core specializes to R=36 shapes
that the mid band does not want.

So the nesting constraint is cheap only while the core is small, and far quality
wants the core to be large. Those pull in opposite directions and the question
should be settled on what nesting actually buys: it avoids pattern uploads when
the player crosses a distance band. Turning already costs zero uploads whether or
not the vocabulary is nested. Distance crossings are rare and gradual, and a
non-nested 64-pattern far set is a 2 KiB upload, roughly 1.5 VBlanks, easily
hidden behind band hysteresis.

Recommendation: keep the prefix while the core is 16 to 32 patterns, and do not
let it cap far quality. If the far band needs 64 or more patterns to read
correctly, pay a rare 2 KiB transition rather than a permanent 12% quality tax.

## Reproduce

    ROOM_BUNDLE_HERO_CORPUS=1 HERO_CORPUS_ANGLES=32 \
    HERO_CORPUS_BANDS=24,28,32,36 HERO_CORPUS_PREVIEW_EVERY=0 \
      build/hier/corpus-bake build/hier/host

    python3 tools/diagnose_lod_shade_metric.py \
      build/hier/host/doomguy_dense_corpus.dhc build/hier/diag \
      --band 3 --patterns 16 --angles 8
