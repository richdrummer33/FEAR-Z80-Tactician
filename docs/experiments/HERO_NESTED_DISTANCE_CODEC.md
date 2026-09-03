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
