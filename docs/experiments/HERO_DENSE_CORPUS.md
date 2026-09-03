# Hero dense view corpus and anchor analysis

Status: measurement pass. No runtime codec layout is chosen here. The point of
this pass was to find out which of the Hero View Codec v2 ideas survive contact
with real numbers, before any of them is built into a ROM.

Short version: **two of the four proposed mechanisms are dead, one is alive but
belongs somewhere else than proposed, and the real constraint turned out to be
distance, not angle.**

## Why a new corpus

The playable pack stores whole chamber states -- "camera state 173 in the
room". Every measured difference between two neighbouring states mixes three
unrelated causes:

1. the hero genuinely turned (appearance),
2. the camera stood somewhere else on the 8-unit walkable lattice (placement),
3. the aim yaw was quantized to 16 steps, so the hero slid up to 11 degrees off
   centre (framing).

Causes 2 and 3 are artefacts of the walkable grid. The v1 orbit analyzer dealt
with them by searching for a best x-shift after the fact, which is exactly the
kind of measurement that flatters whichever predictor happens to resemble the
search.

`ROOM_BUNDLE_HERO_CORPUS=1` on the room-bundle baker removes both analytically
instead:

* the camera orbits the authored hero pivot on an exact circle, so the pivot's
  forward depth equals the band radius for every sample;
* the angle count divides 256, so the aim yaw lands exactly on a hardware yaw
  step and the pivot projects to screen x = 80.0 with zero residual;
* therefore the placement anchor is known in closed form. Normalizing is an
  integer translation, not a registration search, and every pixel difference
  that remains is appearance.

Each record is object-only: the owner-resolved crop of the hero (owner 0x81),
its bounding box, and its anchor. Room pixels cannot contaminate it. The tile
codec is disabled while capturing -- the corpus is the appearance signal a codec
has to compress, and quantizing it first would measure last year's dictionary
instead of this year's geometry.

Default corpus: 256 angles (1.4062 degrees per step) x 4 distance bands
(radius 24, 28, 32, 36), eye height 16, 1024 samples, 4.4 MB.

    ROOM_BUNDLE_HERO_CORPUS=1 build/corpus/corpus-bake OUTDIR
    build/corpus/corpus-analyze OUTDIR/doomguy_dense_corpus.dhc
    python3 tools/analyze_doomguy_dense_corpus.py OUTDIR/doomguy_dense_corpus.dhc
    python3 tools/render_dense_corpus_sheets.py OUTDIR/doomguy_dense_corpus.dhc SHEETDIR

## Framing limit: 24 units

A full 360-degree orbit at radius 20, 21, 22 or 23 pushes some part of the
figure off a screen edge; 24 is the tightest radius that never does. The
playable bake's existing 22-unit rule is not wrong, it is just weaker than it
looks: it survives because the walkable lattice happens not to place a camera at
the worst azimuth, and its nearest legal point is 23.32 units out.

## The finding that actually matters: distance, not angle

Cost of one 1.4-degree rotation step, measured as 8x8 screen tiles that need a
new pattern upload, against the ~48 patterns per VBlank the streaming PoC
sustains:

| radius | changed tiles (mean) | max | steps over 48 |
|-------:|---------------------:|----:|--------------:|
| 24 | 74.1 | 96 | 256/256 |
| 26 | 62.1 | 81 | 256/256 |
| 28 | 53.3 | 69 | 170/256 |
| 30 | 46.9 | 60 |  84/256 |
| 32 | 41.7 | 52 |  29/256 |
| 33 | 39.2 | 50 |   4/256 |
| 34 | 37.3 | 48 |   0/256 |
| 35 | 35.1 | 46 |   0/256 |
| 36 | 33.4 | 44 |   0/256 |

**Radius 34 is the crossover.** At 34 units and beyond, every rotation step in
the whole orbit fits in one VBlank, so the hero can be orbited at full frame
rate. Inside that, rotation has to spend two VBlanks per step -- which is not a
crash, it is half-rate turning, and the existing double-buffered page flip
already has the machinery for it. At radius 24 it is two VBlanks for every
single step, everywhere on the orbit.

This is the answer to "how close should the player be allowed to get". It is a
frame-rate decision, not a ROM-size decision, and it is set by how much of the
160x144 screen the figure covers -- not by how fast it changes.

## Predictor comparison

Five predictors, scored on the hardest case a codec faces (predict the next
1.4-degree view from the current one), at radius 24:

| predictor | silhouette err mean/max | shade err mean | residual tiles | side info |
|---|---|---|---|---|
| P0 nearest anchor      | 3.226 / 4.449 | 12.29 | 74.14 | 0 B |
| P1 global shift        | 3.213 / 4.449 | 12.29 | 74.10 | 2 B |
| P2 row-span morph      | **1.435** / **2.965** | 21.50 | 71.97 | ~211 B |
| P3 two-region shift    | 3.213 / 4.449 | 12.34 | 74.11 | 4 B |
| P4 four-region shift   | 3.154 / 4.164 | 13.58 | 74.09 | 8 B |

Three conclusions, in order of how much they change the plan.

**1. Motion compensation over angle-as-time is dead.** P1, P3 and P4 improve
the silhouette by 0.01 to 0.07 percentage points. That is not a tuning failure;
it is the corpus doing its job. Motion compensation exists to recover a global
translation, and this corpus has no global translation left to recover -- the
anchor already removed it exactly. Everything the earlier x-shift search was
"finding" was camera placement, not the object. Drop P1/P3/P4.

**2. No predictor reduces the VDP upload cost.** Residual tiles land between 72
and 78 for every predictor at every anchor budget, against 74.14 for the
trivial "just keep the previous frame" baseline. A tile is dirty if any of its
64 pixels differs, and the interior re-shades faster than the outline moves: of
the 74 dirty tiles at radius 24, 44 are dirty because the silhouette moved and
the rest are pure interior. P2 cuts silhouette-dirty tiles by 60 percent (44.0
to 17.8) and still barely moves the total. Whatever a view codec is for, it is
not for buying VBlank bandwidth.

**3. P2 is real, but it is a shape channel, not a frame predictor.** Per-row
span morphing halves the silhouette error and is the only predictor that does
anything at all. It also makes shade *worse* (12.3 to 21.5), because resampling
a row horizontally smears the interior. That is the signature of a mechanism
that belongs on its own channel: code the outline with row spans, code the
interior separately, do not ask one resample to do both.

## Adaptive anchors buy nothing over uniform anchors

Greedy adaptive selection was the headline idea. It is not better than putting
anchors at even angles. Radius 24, P0, mean silhouette error:

| anchors | greedy | uniform |
|--------:|-------:|--------:|
|   4 | 31.41 | 31.98 |
|   8 | 20.12 | 20.06 |
|  16 | 11.92 | 11.90 |
|  32 |  6.86 |  6.83 |
|  64 |  4.11 |  4.14 |
|  96 |  3.36 |  3.20 |
| 128 |  3.01 |  3.07 |

The two curves are the same curve. Greedy wins slightly on the worst view at
large budgets (3.70 vs 4.05 at 128) and loses slightly on the mean at small
ones. The reason is visible in the contact sheets: the hero's appearance changes
at a near-uniform rate all the way around the orbit. There are no flat stretches
for an adaptive selector to skip and no sharp events for it to bracket. Use
uniform spacing; it is free, and it makes the runtime index arithmetic trivial.

Note also where the curve gets you. To match the quality of a single 1.4-degree
step (3.23 mean, 4.45 max) you need roughly 96 of the 256 views stored outright.
Storing 37 percent of a corpus is not compression.

## The ordered ramp dither is not the churn source

Suspecting the screen-space 4x4 Bayer dither of causing interior churn, the
whole corpus was rebaked with `ROOM_BUNDLE_DOOMGUY_DITHER=0`. Turning it off
makes things slightly *worse*: shade error 12.29 -> 13.79, changed tiles
74.14 -> 74.70. The dither is keyed to screen coordinates, but within a band the
anchor never moves, so the dither is already object-locked; and feathering the
band boundaries means fewer pixels cross a ramp stop as the angle changes.
Leave it on. This is recorded so nobody optimizes it away on a hunch later.

## What this means for the codec

* Drop sparse adaptive anchors. Drop motion compensation.
* Keep distance banding -- it is the only mechanism measured here that changes
  the frame-rate outcome. The chamber caps the orbit at 36 units, so a wider
  band spread needs a bigger room, which is now a design question rather than a
  guess.
* If a shape channel is wanted, per-row spans are the primitive: ~2 bytes per
  occupied row, 112 to 244 bytes per view depending on band, halving silhouette
  error. Pair it with a separate interior channel.
* The interior remains the tile vocabulary's problem, which is where the
  existing VQ dictionary already works.
* Set the player's minimum approach from the table above, not from the 22-unit
  quality rule. 34 units buys full-rate orbiting; anything closer is a
  deliberate trade of turn rate for size on screen.

## Tools

| file | role |
|---|---|
| `tools/room_bundle_poc_gen.c` | `bake_hero_dense_corpus` writes the DHC1 corpus |
| `tools/hero_corpus_analyze.c` | predictors, anchor selection, report card |
| `tools/analyze_doomguy_dense_corpus.py` | dependency-free reader, structural summary, normalized dumps |
| `tools/render_dense_corpus_sheets.py` | orbit / step-delta / anchor-damage contact sheets |
| `tests/test_dense_corpus_reader.py` | reader unit tests |
| `.github/workflows/hero-dense-corpus.yml` | warning-clean and sanitized builds, full bake, report card, artifacts |

Corpus knobs: `HERO_CORPUS_ANGLES` (must divide 256), `HERO_CORPUS_BANDS`
(comma-separated radii, 8 to 36), `HERO_CORPUS_PREVIEW_EVERY`.
