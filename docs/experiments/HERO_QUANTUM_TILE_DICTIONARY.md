# Hero Quantum Tile Dictionary — R0

Status: experiment branch `experiment/hero-quantum-dictionary`.

## Goal

Given the complete oracle appearance of the hero over angle and distance, find a
small reusable 8x8 semantic tile vocabulary that minimizes perceptual error.

This experiment intentionally does **not** modify the renderer.  It reuses:

- the DHC1 dense object-view corpus;
- the established 8x8 sprite tiling;
- the corrected perceptual shade ordering;
- silhouette weight 12 / shade-rank weight 1 as the initial metric.

The quantum part replaces only the global combinatorial dictionary-selection
step.  Every quantum result is rescored on the same exact classical
facility-location objective.

## Corpus

Initial R0 corpus:

- 32 yaw angles;
- R=32 and R=36 distance bands (DHC1 bands 2 and 3);
- object-only transparent 8x8 sprite patterns;
- no free H/V flips, matching the existing sprite LOD experiments.

Each sprite occurrence becomes a target pattern.  Exact duplicates are collapsed
to one target class while preserving:

- occurrence count;
- number of views containing the target;
- empirical weight.

Two weight policies are supported:

### view

Every camera view has total weight 1, divided over the tiles in that view.
This prevents a large view from dominating only because it occupies more
8x8 blocks.  This is the default/headline metric.

### occurrence

Every tile occurrence has weight 1.  This measures total displayed-tile error.

## Perceptual cost

For target tile i and candidate tile j:

    D[i,j] = sum over 64 pixels of pixel_cost(candidate, target)

The existing `TileWeights` metric is reused:

- transparent/filled mismatch: strong silhouette penalty;
- filled/filled shade mismatch: distance in true dark-to-light ramp order;
- exact pixel: zero.

The corrected brightness-rank metric is important: raw semantic enum distance is
known to produce gross tonal errors.

## Classical controls

The full observed candidate set is always evaluated classically before any
quantum prefilter.

1. Greedy facility growth:
   repeatedly add the candidate giving the largest exact reduction in weighted
   corpus error.

2. PAM-style one-swap refinement:
   replace one selected observed candidate at a time while the true facility
   objective decreases.

3. Weighted synthetic centroid refinement:
   assign every target to its selected representative, then for each of the 64
   pixel positions choose the semantic value 0..8 minimizing the weighted
   perceptual error of that cluster.  This means the final dictionary is **not**
   restricted to tiles that occurred verbatim in the oracle corpus.

These controls are the baseline the QC path must beat or match.

## Compact selection QUBO

One binary variable per candidate tile:

    z_j = 1  -> candidate j is resident

For each candidate, utility is the weighted improvement over the transparent
baseline.  For each candidate pair, redundancy is the weighted overlap of their
per-target improvements.

The minimization surrogate is:

    - sum_j utility_j z_j
    + beta sum_{j<k} redundancy_jk z_j z_k
    + A (sum_j z_j - K)^2

The exact-K penalty is deliberately explicit.  This QUBO is compact and is the
first QAOA target, but it is a **search surrogate**, not the final visual score.
Every sampled K-tile set is rescored with:

    sum_i w_i min_{j:selected} D[i,j]

The first CI calibration prefilters to 18 high-utility observed candidates,
selects K=6, and brute-forces that compact QUBO exactly.  This gives a classical
ground-truth energy before QAOA is asked to sample it.

## Assignment/facility QUBO

When logical-qubit count is not the first constraint, the more direct
formulation adds target-assignment variables:

    z_j    candidate j is selected
    y_i,j  target i uses candidate j

For tractability the exporter can retain the top-L nearest candidates per
target.  The objective is the actual weighted perceptual assignment cost:

    sum_i sum_j w_i D[i,j] y_i,j

with quadratic penalties enforcing:

    sum_j y_i,j = 1        for every target i
    y_i,j <= z_j
    sum_j z_j = K

This is much closer to the true weighted k-medoids/facility-location problem,
but logical-variable count is approximately:

    candidates + targets * L

The exporter records the exact variable count and sparse target candidate lists.

## QUBO -> Ising -> QAOA circuit

Binary variables are mapped with:

    x_i = (1 - Z_i) / 2

which produces:

    H_C = constant + sum_i h_i Z_i + sum_{i<j} J_i,j Z_i Z_j

The QAOA circuit begins in |+>^n and repeats p times:

    exp(-i gamma H_C)
    exp(-i beta sum_i X_i)

The emitted OpenQASM 3 circuit implements:

- H on every qubit;
- Z field evolution with RZ;
- ZZ evolution with CX-RZ-CX;
- X mixer with RX;
- measurement of every selection/assignment bit.

`tools/quantum_tile_qaoa_qiskit.py` also builds the same ansatz with current
Qiskit `qaoa_ansatz` and can sample it with `StatevectorSampler` when Qiskit
is installed.

## What a QPU sample means

For the compact formulation, one measured bitstring is directly a proposed
resident dictionary:

    001010... -> select candidate 2, 4, ...

Samples are filtered/rescored classically:

1. reject/warn if cardinality is not K;
2. map selected QC-local candidates back to full corpus candidate IDs;
3. compute exact weighted facility score;
4. assign every target to the nearest selected tile;
5. synthesize one weighted optimal centroid per resulting cluster;
6. rescore the synthetic dictionary.

The intended next loop is:

    observed candidates
      -> QC selection
      -> exact assignment
      -> synthetic centroids
      -> add centroids to candidate pool
      -> QC selection again

That is the quantum-assisted analogue of Lloyd/vector-quantization refinement,
with the quantum solver used for the global discrete selection step.

## R0 artifacts

`tools/quantum_tile_dictionary.py` emits:

- `weighted_tile_corpus.json`
- `classical_centroid_dictionary.json`
- `compact_selection_qubo.json`
- `compact_selection_ising.json`
- `compact_qaoa_p1.qasm`
- optional `assignment_facility_qubo.json`
- `result.json`

The CI workflow bakes the same dense corpus used by the later LOD work, runs
unit tests, builds both QUBO formulations, brute-forces the small compact
calibration problem, and uploads all artifacts.

## What is intentionally not claimed yet

- no quantum advantage;
- no real QPU result yet;
- compact utility/redundancy QUBO is not identical to facility location;
- assignment QUBO can become very large;
- no holdout-angle validation yet;
- no angle/distance sweep of K yet;
- no background-tile/HV-flip corpus yet.

Those are follow-up experiments after the first corpus/QUBO run is numerically
validated.
