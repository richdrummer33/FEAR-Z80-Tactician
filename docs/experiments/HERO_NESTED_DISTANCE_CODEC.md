# Nested distance codec lab

Status: **temporary measurement branch; no runtime format chosen.**

This experiment starts from the dense DHC1 oracle corpus on
`temp/hero-codec-v2-dense-corpus` and asks a different question from the
anchor/predictor pass:

> Do independently rendered distance bands actually need independently stored
> hero data, or can a close master representation be deliberately co-designed
> so that cheap deterministic subsets/offsets are good medium/far renderings?

The hypothesis is intentionally stronger than ordinary mipmapping.  The host
baker is allowed to rearrange pixels inside a shared 8x8 visual pattern, subject
to a strong nearest-view error penalty, so that the *same pattern data* becomes
more useful after projective shrinkage.  Distance then selects tiny
pattern-class side information rather than another image.

## First codec model

For one fixed angular view:

1. The nearest DHC1 band is the **master raster**.
2. Each farther DHC1 band is an independently rendered **oracle**.
3. A pinhole map predicts where each far pixel came from in the master.
4. Baseline A samples the mapped master pixel directly.
5. Baseline B fits one global integer phase vector per distance.
6. The proposed model fits one integer phase vector per **shared 8x8 master
   pattern class** and distance.
7. Optional direct-search refinement swaps adjacent pixels inside a pattern.
   Every occurrence of that pattern is changed identically, preserving its
   shade histogram and dictionary identity as a shared primitive.
8. Refinement minimizes nearest + medium + far oracle error.  The optimizer is
   callback-driven and deliberately independent of the decoder so later
   experiments can replace the phase model with ranked masks, quadtree
   selectors, edge primitives, perceptual loss, or ROM/VRAM penalties.

This directly tests the user's "the far representation is latent inside the
near pattern" idea while keeping the runtime-side proposal tiny: a distance
level and a small per-pattern vector table.

## Why pattern-class refinement matters

Mutating individual screen tiles would cheat.  A codec only wins if one shared
pattern can serve many instances.  The optimizer therefore groups identical
8x8 semantic tiles in the original master and applies a proposed pixel swap to
every occurrence of that class simultaneously.

The class identity remains stable during optimization.  This lets the baker
co-design a canonical pattern without silently inventing instance-specific
patterns.

## Metrics

`tools/analyze_nested_distance_codec.py` reports, for every farther band:

- silhouette disagreement against the independent oracle;
- mean absolute shade-class error on overlapping hero pixels;
- number of 8x8 tiles whose pixels differ from the oracle;
- distinct output pattern count;
- phase-table class count and a simple two-byte-per-vector storage estimate;
- nearest-view damage after joint refinement.

The first pass is deliberately not claiming a shipping byte layout.  It is an
upper/lower-bound experiment to decide whether the distance axis factors well
enough to justify a real codec.

## Run

After producing a DHC1 corpus:

    python3 tools/analyze_nested_distance_codec.py \
      build/corpus/host/doomguy_dense_corpus.dhc \
      --angle 0 --refine-passes 1 --refine-candidates 256 \
      --dump-dir build/corpus/nested-distance

Useful follow-up sweeps:

- angles 0, 64, 128, 192 before assuming one pose is representative;
- phase radii 1, 2, 3 to find the side-information/error knee;
- near-weight sweeps to plot the actual multi-distance rate/distortion trade;
- dense radius bands once the chamber/corpus can safely bake them;
- replacing phase vectors with ranked/nested masks and hierarchical selectors.

## Architecture boundary

`tools/nested_lod_core.py` contains no DHC1 or Game Gear knowledge.  It owns
generic raster losses, projective sampling, phase fitting, shared-pattern
classes, and iterative pattern refinement.

`tools/analyze_nested_distance_codec.py` is the DHC1 adapter and experiment
driver.

That separation is deliberate: the iterative refinement machinery is expected
to survive even if this particular phase-vector codec does not.


## First measured pass — angle 0

The first green end-to-end run used the existing four DHC1 radii
24 / 28 / 32 / 36 and kept radius 24 as the unmodified master.

Silhouette disagreement against the independently rendered farther oracle:

| model | R=28 | R=32 | R=36 | selector storage |
|---|---:|---:|---:|---:|
| nearest projected sample | 11.883% | 17.886% | 22.377% | 0 |
| oracle best sample inside true footprint | 10.358% | 13.472% | 17.371% | unbounded / diagnostic only |
| whole-master per-class phase | 5.411% | 8.081% | 11.525% | 136 B / distance |
| strict canonical-tile phase | 10.333% | 14.924% | 18.755% | 136 B / distance |
| fixed Bayer rank, per-class phase | 10.671% | 15.760% | 18.942% | 48 B mask + 136 B / distance |

The whole-master phase result is deliberately labelled an **optimistic
whole-image model**.  Its phase may cross an 8x8 boundary and therefore select
a pixel from the neighbouring master tile.  That is legal if the shipping
representation retains a spatial master image and the decoder can address it,
but it is not evidence that a two-byte selector attached to one isolated
dictionary tile is sufficient.

The strict canonical-tile control cannot cross that boundary.  It is therefore
the honest baseline for a selector that rides beside one reusable resident
pattern.  Its much smaller gain is important.

The oracle-footprint bound is also intentionally harsh and useful: with the
radius-24 image frozen, even an all-knowing selector that picks the best
existing close pixel inside each farther pixel's true projective footprint only
improves the silhouette modestly.  In other words, **plain subsampling is not
the proposed codec**.  The interesting hypothesis requires the baker to
co-design/rearrange the close pattern so the future representative samples are
placed where the farther levels need them.

The first conservative class-wide adjacent-swap pass used a 4x nearest-view
weight and accepted zero swaps.  That is not taken as a negative verdict on
co-design: a greedy one-swap hill climber with a strong nearest penalty cannot
cross a coordinated rearrangement valley.  It does tell us not to confuse
"start from the oracle and make tiny local swaps" with the strongest version of
the idea.

## Optimizer architecture from here

The experiment now keeps three independent axes:

1. **Representation / decoder** — point sample, whole-image phase,
   dictionary-local phase, ranked footprint, or a later hierarchical selector.
2. **Objective** — oracle silhouette/shade error plus optional ROM, pattern
   count, upload churn, and a hard/soft nearest-view quality constraint.
3. **Search strategy** — current deterministic direct swaps, learned rank
   permutation, and later a stronger coordinated optimizer.

The reusable search code remains outside the DHC1 adapter.  That separation is
intentional: if rank masks lose to a quadtree, edge vocabulary, or a small
multi-tile neighbourhood selector, the oracle, losses, and iterative search
machinery survive.

The strongest next co-design experiment should not freeze the nearest image.
Instead it should solve:

> find one shared master pattern family whose nearest reconstruction stays
> inside a chosen quality budget while the same stored samples minimize error
> across all farther radii.

A useful implementation path is alternating optimization:

- fit distance selectors for the current master;
- optimize the shared master pattern under a nearest-view error cap;
- refit selectors;
- repeat until the multi-distance objective stops improving.

For fixed selectors, each shared 8x8 pattern can potentially be treated as a
small constrained assignment problem rather than relying only on greedy
single-pixel swaps.  Preserving the pattern's shade histogram keeps the
lighting vocabulary stable while allowing coordinated relocation of dither and
edge samples.

This is the point at which the experiment becomes the user's proposed
**baked nested-sample co-design** rather than merely a downsampling comparison.


### Learned-rank sanity pass

A one-pass direct search over sixteen adjacent rank swaps was then enabled for
the shared 8x8 Bayer permutation.  It accepted **zero** swaps:
`12537 -> 12537` weighted multi-distance objective.

That does **not** establish that Bayer is globally optimal.  It establishes a
narrower and useful fact: with the master image frozen, the current
pattern-class phase tables fixed during each candidate evaluation, and only
small adjacent changes to the rank ordering allowed, there is no immediate
downhill direction.  The next meaningful search effort belongs on the
**co-design of the master pattern and selector together**, not on polishing a
generic Bayer mask in isolation.


## Stable co-design solver v1

The temporary branch now contains a reusable optimization runtime:

- `tools/iterative_solver.py` — representation-agnostic alternating solver,
  deterministic annealed escape probes, dependency-free Hungarian assignment,
  atomic JSON status, NDJSON trace, and compact live-log records.
- `tools/solve_nested_distance_codec.py` — DHC1 / Doomguy adapter for the
  current whole-master phase-selector experiment.

### Feedback-stable loop

The accepted mainline is monotonic.  A candidate can replace the mainline only
when a full evaluation is feasible and improves the constrained objective.

Each outer iteration is:

1. refit the distance selectors for the current master;
2. freeze those selectors;
3. solve coordinated 64-position pattern reassignment proposals;
4. accept only farther-distance improvements that remain inside the hard
   nearest-view quality budget;
5. refit selectors after the accepted pattern changes;
6. publish the new best only if the full refitted solution really improved;
7. if stalled, run disposable seeded annealing probes which may temporarily
   walk uphill, then promote a probe only if its final fully refitted result is
   better than the global best.

This separates local-minimum exploration from the accepted feedback loop:
exploration can be non-monotonic; the published solver state cannot thrash.

### Coordinated pattern update

With selectors frozen, one shared 8x8 pattern becomes a 64-position assignment
problem.  The current pattern's exact multiset of semantic shade values is kept,
but all 64 positions may be reassigned simultaneously.  A 64x64 Hungarian solve
finds the minimum-cost arrangement for the current distance demand plus a
proposal-only price on nearest-view damage.

This is deliberately stronger than the earlier adjacent-pixel hill climber.
The pattern may jump directly across a local swap valley while preserving its
shade histogram and shared-class identity.

Nearest quality is now an epsilon-style constraint:

> minimize farther-distance oracle error, subject to nearest-view error <=
> the configured budget.

The `near_lambda` parameter does not contaminate the accepted objective.  It is
only a Lagrange price used to generate candidate assignments.  Each pass tries
several prices, including zero.  A bounded dead-band controller can relax or
tighten the central proposal price between iterations without changing the
meaning of the best score.

### LLM / human live telemetry

During a solve the tool atomically rewrites `--status-json` with the freshest
small snapshot.  It also optionally appends every published event to
`--trace-ndjson` and emits the same event as one
`LLM_SOLVER_STATUS {...}` stdout line.

The snapshot contains:

- phase / outer iteration / sub-iteration;
- current and global-best objective;
- feasibility and nearest-budget use;
- per-distance silhouette, shade and changed-tile error;
- master pattern count and selector-byte estimate;
- accepted proposal / class / predicted deltas;
- annealing temperature and escape acceptance;
- controller action;
- the last twelve high-level events.

This is intentionally compact enough to poll during long host solves without
needing to reconstruct state from verbose diagnostic output.

## First coordinated co-design result

CI run `33734716817`, angle zero, source radius 24, hard near budget 192:

| metric | before co-design | after co-design |
|---|---:|---:|
| far weighted objective | 7934 | **7761** |
| R=28 silhouette error | 5.4109% | **5.1203%** |
| R=32 silhouette error | 8.0808% | **7.7703%** |
| R=36 silhouette error | 11.5254% | **11.2999%** |
| nearest silhouette damage | 0% | **0.4318%** |
| nearest weighted damage | 0 / 192 | **162 / 192** |
| nearest changed 8x8 cells | 0 | **1** |
| master pattern classes | 68 | **68** |

The first successful move was one full 64-position Hungarian reassignment of
shared pattern class 56.  It predicted a 173-point farther-distance improvement
for 162 points of nearest-view damage; the measured full objective improved by
exactly 173 after the change.  The selector refit retained that win.

That is a small but important proof: the close pattern was deliberately made
slightly less locally ideal, within a fixed quality envelope, and the same
stored pattern became measurably better at all three farther radii without
adding a pattern class.

The run then stalled because the single accepted class consumed about 84% of
the deliberately small near-error budget.  Other individually promising
pattern reassignments were rejected by the hard budget.  This is desirable
behaviour for the first control test, not evidence of convergence of the full
codec.

The next research axis is therefore the rate/distortion envelope itself:
repeat across several near budgets and angles, then graduate the representation
from the optimistic whole-master phase selector to the hardware-oriented
neighbourhood-to-resident-pattern dictionary synthesis.
