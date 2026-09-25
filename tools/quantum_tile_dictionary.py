#!/usr/bin/env python3
"""Quantum-assisted shared 8x8 hero tile dictionary experiment.

This tool reuses the existing DHC1 dense hero corpus and perceptual tile metric.
It deliberately leaves the renderer alone.  The pipeline is:

  DHC1 oracle views -> sprite 8x8 demands -> weighted unique target classes
  -> observed candidate pool -> classical controls -> QUBO export
  -> optional quantum selection -> weighted synthetic centroid refinement.

Two QUBO formulations are emitted:

1) ``compact``: one binary variable per candidate tile. Utility rewards tiles
   that improve many weighted targets relative to transparent; pairwise
   redundancy penalizes candidates that buy the same improvements; an exact-K
   quadratic penalty fixes dictionary size. This is a compact search surrogate
   whose samples are always rescored against the exact facility objective.

2) ``assignment``: selection bits plus sparse target->candidate assignment bits.
   It directly encodes weighted facility-location/k-medoids over each target's
   top-L candidate list. It needs many more logical variables, but is the clean
   formulation when qubit count is not the first constraint.

The quantum computer never invents a tile pixel-by-pixel here. After selection,
all targets are assigned to their selected representatives and the CPU computes
an independently optimal 8x8 weighted semantic centroid for each cluster.
"""

from __future__ import annotations

import argparse
import itertools
import json
import os
import pathlib
from collections import defaultdict
from dataclasses import dataclass

from analyze_doomguy_dense_corpus import Corpus
from analyze_resident_lod_dictionary import parse_csv_ints
from analyze_sprite_resident_lod import build_groups
from resident_tile_dictionary import TileWeights, pattern_cost, pixel_cost


@dataclass(frozen=True)
class TargetClass:
    pattern: bytes
    weight: float
    occurrences: int
    views: int


@dataclass
class Qubo:
    variables: list[str]
    linear: dict[int, float]
    quadratic: dict[tuple[int, int], float]
    offset: float = 0.0
    metadata: dict | None = None

    def add_linear(self, i: int, value: float) -> None:
        if value:
            self.linear[i] = self.linear.get(i, 0.0) + float(value)

    def add_quadratic(self, i: int, j: int, value: float) -> None:
        if not value:
            return
        if i == j:
            self.add_linear(i, value)
            return
        if j < i:
            i, j = j, i
        self.quadratic[(i, j)] = self.quadratic.get((i, j), 0.0) + float(value)

    def energy(self, bits) -> float:
        if len(bits) != len(self.variables):
            raise ValueError("bit vector length does not match QUBO")
        e = self.offset
        for i, a in self.linear.items():
            e += a * bits[i]
        for (i, j), b in self.quadratic.items():
            e += b * bits[i] * bits[j]
        return e

    def to_json(self) -> dict:
        return {
            "schema": "hero-tile-qubo-v1",
            "variables": self.variables,
            "linear": [[i, v] for i, v in sorted(self.linear.items()) if abs(v) > 1e-15],
            "quadratic": [
                [i, j, v]
                for (i, j), v in sorted(self.quadratic.items())
                if abs(v) > 1e-15
            ],
            "offset": self.offset,
            "metadata": self.metadata or {},
        }


def atomic_json(path, payload) -> None:
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n")
    os.replace(tmp, path)


def flatten_demands(groups):
    return [d for g in groups for d in g["demands"]]


def build_weighted_targets(groups, views, weight_mode="view"):
    """Collapse identical 8x8 targets while retaining empirical mass."""
    if weight_mode not in ("view", "occurrence"):
        raise ValueError("weight_mode must be 'view' or 'occurrence'")

    demands = flatten_demands(groups)
    per_view_n = defaultdict(int)
    for d in demands:
        per_view_n[d["view_index"]] += 1

    agg = {}
    seen_views = defaultdict(set)
    for d in demands:
        p = bytes(d["pattern"])
        if p not in agg:
            agg[p] = [0.0, 0]
        if weight_mode == "occurrence":
            w = 1.0
        else:
            n = per_view_n[d["view_index"]]
            w = 1.0 / n if n else 0.0
        agg[p][0] += w
        agg[p][1] += 1
        seen_views[p].add(d["view_index"])

    targets = [
        TargetClass(p, weight, occurrences, len(seen_views[p]))
        for p, (weight, occurrences) in agg.items()
    ]
    targets.sort(key=lambda t: (-t.weight, -t.occurrences, t.pattern))
    return targets


def target_stats(targets):
    return {
        "unique_targets": len(targets),
        "weighted_mass": sum(t.weight for t in targets),
        "occurrences": sum(t.occurrences for t in targets),
        "max_target_weight": max((t.weight for t in targets), default=0.0),
    }


def observed_candidates(targets):
    return [t.pattern for t in targets]


def cost_matrix(targets, candidates, weights):
    return [
        [pattern_cost(c, t.pattern, weights) for c in candidates]
        for t in targets
    ]


def exact_dictionary_score(targets, candidates, selected, matrix=None):
    """Weighted exact facility objective for one selected dictionary."""
    if not selected:
        return float("inf")
    if matrix is None:
        raise ValueError("matrix is required")
    total = 0.0
    exact_mass = 0.0
    assignment = []
    for i, t in enumerate(targets):
        best = min((matrix[i][j], j) for j in selected)
        total += t.weight * best[0]
        if best[0] == 0:
            exact_mass += t.weight
        assignment.append(best[1])
    mass = sum(t.weight for t in targets)
    return {
        "weighted_cost": total,
        "mean_cost": total / mass if mass else 0.0,
        "exact_weight_fraction": exact_mass / mass if mass else 1.0,
        "assignment": assignment,
    }


def greedy_facility(targets, candidates, k, matrix):
    if not 1 <= k <= len(candidates):
        raise ValueError("k outside candidate count")
    selected = []
    best_cost = [float("inf")] * len(targets)
    history = []
    remaining = set(range(len(candidates)))
    for step in range(k):
        winner = None
        for j in sorted(remaining):
            total = 0.0
            for i, t in enumerate(targets):
                total += t.weight * min(best_cost[i], matrix[i][j])
            key = (total, j)
            if winner is None or key < winner[0]:
                winner = (key, j)
        j = winner[1]
        selected.append(j)
        remaining.remove(j)
        for i in range(len(targets)):
            best_cost[i] = min(best_cost[i], matrix[i][j])
        history.append({"step": step + 1, "candidate": j,
                        "weighted_cost": winner[0][0]})
    return selected, history


def swap_refine(targets, candidates, selected, matrix, max_passes=8):
    """PAM-style 1-swap local control under the true objective."""
    selected = list(selected)
    current = exact_dictionary_score(targets, candidates, selected, matrix)
    history = [{"pass": 0, "weighted_cost": current["weighted_cost"]}]
    for p in range(1, max_passes + 1):
        chosen = set(selected)
        best = None
        for out_pos, old in enumerate(selected):
            for new in range(len(candidates)):
                if new in chosen:
                    continue
                proposal = list(selected)
                proposal[out_pos] = new
                score = exact_dictionary_score(targets, candidates, proposal, matrix)
                key = (score["weighted_cost"], tuple(sorted(proposal)))
                if best is None or key < best[0]:
                    best = (key, proposal, score, old, new)
        if best is None or best[2]["weighted_cost"] >= current["weighted_cost"] - 1e-12:
            break
        selected = best[1]
        current = best[2]
        history.append({"pass": p, "weighted_cost": current["weighted_cost"],
                        "removed": best[3], "added": best[4]})
    return selected, current, history


def weighted_centroid(patterns, weights_for_patterns, metric):
    """Optimal semantic 8x8 representative for a fixed weighted cluster."""
    if not patterns:
        return bytes(64)
    if len(patterns) != len(weights_for_patterns):
        raise ValueError("centroid patterns/weights length mismatch")
    out = bytearray(64)
    for pos in range(64):
        best = None
        for value in range(9):
            c = 0.0
            for p, w in zip(patterns, weights_for_patterns):
                c += w * pixel_cost(value, p[pos], metric)
            key = (c, value)
            if best is None or key < best[0]:
                best = (key, value)
        out[pos] = best[1]
    return bytes(out)


def centroid_refine(targets, dictionary, metric, iterations=4):
    """Weighted Lloyd refinement; accept only non-increasing iterations."""
    current = [bytes(p) for p in dictionary]
    history = []
    for iteration in range(iterations + 1):
        matrix = cost_matrix(targets, current, metric)
        score = exact_dictionary_score(targets, current, range(len(current)), matrix)
        history.append({"iteration": iteration,
                        "weighted_cost": score["weighted_cost"],
                        "mean_cost": score["mean_cost"]})
        if iteration == iterations:
            break
        buckets = [[] for _ in current]
        bucket_weights = [[] for _ in current]
        for t, j in zip(targets, score["assignment"]):
            buckets[j].append(t.pattern)
            bucket_weights[j].append(t.weight)
        proposal = list(current)
        for j in range(len(current)):
            if buckets[j]:
                proposal[j] = weighted_centroid(buckets[j], bucket_weights[j], metric)
        pmat = cost_matrix(targets, proposal, metric)
        pscore = exact_dictionary_score(targets, proposal, range(len(proposal)), pmat)
        if pscore["weighted_cost"] > score["weighted_cost"] + 1e-12:
            break
        if proposal == current:
            break
        current = proposal
    matrix = cost_matrix(targets, current, metric)
    final = exact_dictionary_score(targets, current, range(len(current)), matrix)
    return current, final, history


def candidate_utility(targets, matrix, metric):
    zero = bytes(64)
    baseline = [pattern_cost(zero, t.pattern, metric) for t in targets]
    gains = []
    utility = []
    for j in range(len(matrix[0]) if matrix else 0):
        g = [max(0.0, baseline[i] - matrix[i][j]) for i in range(len(targets))]
        gains.append(g)
        utility.append(sum(t.weight * g[i] for i, t in enumerate(targets)))
    return baseline, gains, utility


def preprocess_candidates(targets, candidates, matrix, metric, limit=0):
    """Optional deterministic utility prefilter for QC-sized experiments."""
    if not limit or limit >= len(candidates):
        return candidates, matrix, list(range(len(candidates)))
    _, _, utility = candidate_utility(targets, matrix, metric)
    keep = sorted(range(len(candidates)), key=lambda j: (-utility[j], j))[:limit]
    keep.sort()
    return (
        [candidates[j] for j in keep],
        [[row[j] for j in keep] for row in matrix],
        keep,
    )


def add_cardinality_penalty(qubo, variables, target, penalty):
    # penalty * (sum x - target)^2
    qubo.offset += penalty * target * target
    variables = list(variables)
    for i in variables:
        qubo.add_linear(i, penalty * (1 - 2 * target))
    for a, i in enumerate(variables):
        for j in variables[a + 1:]:
            qubo.add_quadratic(i, j, 2 * penalty)


def build_compact_qubo(targets, candidates, k, matrix, metric,
                       redundancy_beta=0.35, cardinality_penalty=None):
    """One-variable-per-candidate utility/redundancy selection QUBO."""
    _, gains, utility = candidate_utility(targets, matrix, metric)
    max_u = max(utility, default=1.0) or 1.0
    utility_n = [u / max_u for u in utility]

    redundancy = {}
    max_r = 0.0
    for j in range(len(candidates)):
        for q in range(j + 1, len(candidates)):
            r = sum(t.weight * min(gains[j][i], gains[q][i])
                    for i, t in enumerate(targets))
            redundancy[(j, q)] = r
            max_r = max(max_r, r)
    max_r = max_r or 1.0

    vars_ = [f"select[{j}]" for j in range(len(candidates))]
    qubo = Qubo(vars_, {}, {}, 0.0, {
        "formulation": "compact-utility-redundancy",
        "k": k,
        "redundancy_beta": redundancy_beta,
        "note": "search surrogate; always rescore samples on exact facility objective",
    })
    for j, u in enumerate(utility_n):
        qubo.add_linear(j, -u)
    for (j, q), r in redundancy.items():
        qubo.add_quadratic(j, q, redundancy_beta * (r / max_r))

    if cardinality_penalty is None:
        cardinality_penalty = 2.0 * (
            1.0 + redundancy_beta * max(0, len(candidates) - 1))
    add_cardinality_penalty(
        qubo, range(len(candidates)), k, cardinality_penalty)
    qubo.metadata["cardinality_penalty"] = cardinality_penalty
    qubo.metadata["utility_scale"] = max_u
    qubo.metadata["redundancy_scale"] = max_r
    return qubo


def build_assignment_qubo(targets, candidates, k, matrix, top_l=8,
                          constraint_penalty=None):
    """Sparse assignment/facility-location QUBO.

    z_j selects a resident candidate. y_i,j assigns target i to one of its
    top-L candidates. Penalties enforce one assignment, y<=z, and exactly K z.
    """
    if top_l <= 0:
        raise ValueError("top_l must be positive")
    n_c = len(candidates)
    names = [f"select[{j}]" for j in range(n_c)]
    assignment_vars = []
    top = []
    for i, row in enumerate(matrix):
        js = sorted(range(n_c), key=lambda j: (row[j], j))[:min(top_l, n_c)]
        top.append(js)
        row_vars = []
        for j in js:
            row_vars.append(len(names))
            names.append(f"assign[{i},{j}]")
        assignment_vars.append(row_vars)

    qubo = Qubo(names, {}, {}, 0.0, {
        "formulation": "sparse-assignment-facility",
        "k": k,
        "top_l": top_l,
        "target_count": len(targets),
        "candidate_count": n_c,
    })

    max_weighted = 0.0
    for i, t in enumerate(targets):
        for j in top[i]:
            max_weighted = max(max_weighted, t.weight * matrix[i][j])
    scale = max_weighted or 1.0
    for i, t in enumerate(targets):
        for local, j in enumerate(top[i]):
            y = assignment_vars[i][local]
            qubo.add_linear(y, (t.weight * matrix[i][j]) / scale)

    if constraint_penalty is None:
        constraint_penalty = max(4.0, 2.0 * len(targets) + 2.0)

    for row_vars in assignment_vars:
        add_cardinality_penalty(qubo, row_vars, 1, constraint_penalty)

    # y_ij <= z_j -> penalty * y_ij * (1-z_j)
    for i, js in enumerate(top):
        for local, j in enumerate(js):
            y = assignment_vars[i][local]
            qubo.add_linear(y, constraint_penalty)
            qubo.add_quadratic(y, j, -constraint_penalty)

    add_cardinality_penalty(qubo, range(n_c), k, constraint_penalty)
    qubo.metadata["constraint_penalty"] = constraint_penalty
    qubo.metadata["assignment_cost_scale"] = scale
    qubo.metadata["top_candidates"] = top
    return qubo


def qubo_to_ising(qubo):
    """Map x=(1-Z)/2. Returns constant + h_i Z_i + J_ij Z_i Z_j."""
    offset = qubo.offset
    h = defaultdict(float)
    jz = defaultdict(float)
    for i, a in qubo.linear.items():
        offset += a / 2.0
        h[i] -= a / 2.0
    for (i, j), b in qubo.quadratic.items():
        offset += b / 4.0
        h[i] -= b / 4.0
        h[j] -= b / 4.0
        jz[(i, j)] += b / 4.0
    return {"offset": offset, "h": dict(h), "J": dict(jz)}


def emit_qaoa_qasm3(qubo, path, reps=1, gammas=None, betas=None):
    """Emit an explicit numeric QAOA circuit in OpenQASM 3."""
    if reps < 1:
        raise ValueError("reps must be >=1")
    gammas = list(gammas or [0.5] * reps)
    betas = list(betas or [0.3] * reps)
    if len(gammas) != reps or len(betas) != reps:
        raise ValueError("gamma/beta count must equal reps")
    ising = qubo_to_ising(qubo)
    n = len(qubo.variables)
    lines = [
        "OPENQASM 3.0;",
        'include "stdgates.inc";',
        f"qubit[{n}] q;",
        f"bit[{n}] c;",
    ]
    for i in range(n):
        lines.append(f"h q[{i}];")
    for layer in range(reps):
        gamma = gammas[layer]
        beta = betas[layer]
        lines.append(f"// QAOA cost layer {layer}")
        for i, coef in sorted(ising["h"].items()):
            if abs(coef) > 1e-15:
                lines.append(f"rz({2.0 * gamma * coef:.17g}) q[{i}];")
        for (i, j), coef in sorted(ising["J"].items()):
            if abs(coef) <= 1e-15:
                continue
            lines.append(f"cx q[{i}], q[{j}];")
            lines.append(f"rz({2.0 * gamma * coef:.17g}) q[{j}];")
            lines.append(f"cx q[{i}], q[{j}];")
        lines.append(f"// QAOA X mixer layer {layer}")
        for i in range(n):
            lines.append(f"rx({2.0 * beta:.17g}) q[{i}];")
    for i in range(n):
        lines.append(f"c[{i}] = measure q[{i}];")
    pathlib.Path(path).write_text("\n".join(lines) + "\n")


def brute_force_qubo(qubo, fixed_ones=None, max_variables=25):
    n = len(qubo.variables)
    if n > max_variables:
        raise ValueError(f"bruteforce disabled above {max_variables} variables")
    best = None
    if fixed_ones is None:
        masks = range(1 << n)
    else:
        masks = (
            sum(1 << i for i in comb)
            for comb in itertools.combinations(range(n), fixed_ones)
        )
    for mask in masks:
        bits = [(mask >> i) & 1 for i in range(n)]
        e = qubo.energy(bits)
        key = (e, mask)
        if best is None or key < best[0]:
            best = (key, bits)
    return {"energy": best[0][0], "bits": best[1]}


def pattern_hex(pattern):
    return bytes(pattern).hex()


def write_dictionary(path, patterns):
    atomic_json(path, {
        "schema": "hero-tile-dictionary-v1",
        "patterns": [pattern_hex(p) for p in patterns],
    })


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("outdir")
    ap.add_argument("--angles", type=parse_csv_ints)
    ap.add_argument("--bands", type=parse_csv_ints, default=parse_csv_ints("2,3"))
    ap.add_argument("--patterns", type=int, default=16)
    ap.add_argument("--weight-mode", choices=("view", "occurrence"), default="view")
    ap.add_argument("--silhouette-weight", type=float, default=12.0)
    ap.add_argument("--shade-weight", type=float, default=1.0)
    ap.add_argument("--candidate-limit", type=int, default=0,
                    help="utility prefilter for compact QC experiments; 0 keeps all")
    ap.add_argument("--redundancy-beta", type=float, default=0.35)
    ap.add_argument("--assignment-top-l", type=int, default=8)
    ap.add_argument("--lloyd-iterations", type=int, default=4)
    ap.add_argument("--emit-assignment-qubo", action="store_true")
    ap.add_argument("--emit-qasm", action="store_true")
    ap.add_argument("--qaoa-reps", type=int, default=1)
    ap.add_argument("--exact-compact-max", type=int, default=20)
    args = ap.parse_args()

    outdir = pathlib.Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    corpus = Corpus(args.corpus)
    angles = args.angles if args.angles is not None else list(range(corpus.angles))
    groups, views = build_groups(corpus, angles, args.bands)
    metric = TileWeights(args.silhouette_weight, args.shade_weight)
    targets = build_weighted_targets(groups, views, args.weight_mode)
    candidates = observed_candidates(targets)
    matrix = cost_matrix(targets, candidates, metric)

    # Full-candidate classical controls happen before QC preprocessing.
    greedy, greedy_hist = greedy_facility(
        targets, candidates, args.patterns, matrix)
    swapped, swapped_score, swap_hist = swap_refine(
        targets, candidates, greedy, matrix)
    seed_dictionary = [candidates[j] for j in swapped]
    centroids, centroid_score, centroid_hist = centroid_refine(
        targets, seed_dictionary, metric, args.lloyd_iterations)

    qc_candidates, qc_matrix, source_indices = preprocess_candidates(
        targets, candidates, matrix, metric, args.candidate_limit)
    if args.patterns > len(qc_candidates):
        raise SystemExit("--patterns exceeds QC candidate count")

    compact = build_compact_qubo(
        targets, qc_candidates, args.patterns, qc_matrix, metric,
        redundancy_beta=args.redundancy_beta)
    atomic_json(outdir / "compact_selection_qubo.json", compact.to_json())
    ising = qubo_to_ising(compact)
    atomic_json(outdir / "compact_selection_ising.json", {
        "schema": "hero-tile-ising-v1",
        "variables": compact.variables,
        "offset": ising["offset"],
        "h": [[i, v] for i, v in sorted(ising["h"].items())],
        "J": [[i, j, v] for (i, j), v in sorted(ising["J"].items())],
        "metadata": compact.metadata,
    })
    if args.emit_qasm:
        emit_qaoa_qasm3(
            compact, outdir / "compact_qaoa_p1.qasm", reps=args.qaoa_reps)

    exact_compact = None
    if len(qc_candidates) <= args.exact_compact_max:
        exact_compact = brute_force_qubo(
            compact, fixed_ones=args.patterns,
            max_variables=args.exact_compact_max)
        chosen_local = [
            i for i, b in enumerate(exact_compact["bits"]) if b]
        chosen_global = [source_indices[i] for i in chosen_local]
        exact_compact["selected_local"] = chosen_local
        exact_compact["selected_global"] = chosen_global
        exact_compact["true_facility_score"] = exact_dictionary_score(
            targets, candidates, chosen_global, matrix)

    assignment_meta = None
    if args.emit_assignment_qubo:
        assignment = build_assignment_qubo(
            targets, qc_candidates, args.patterns, qc_matrix,
            top_l=args.assignment_top_l)
        atomic_json(
            outdir / "assignment_facility_qubo.json", assignment.to_json())
        assignment_meta = {
            "variables": len(assignment.variables),
            "linear_terms": len(assignment.linear),
            "quadratic_terms": len(assignment.quadratic),
            "metadata": assignment.metadata,
        }

    write_dictionary(
        outdir / "classical_centroid_dictionary.json", centroids)
    atomic_json(outdir / "weighted_tile_corpus.json", {
        "schema": "hero-tile-qc-corpus-v1",
        "weight_mode": args.weight_mode,
        "angles": angles,
        "bands": args.bands,
        "view_count": len(views),
        "targets": [
            {
                "pattern": pattern_hex(t.pattern),
                "weight": t.weight,
                "occurrences": t.occurrences,
                "views": t.views,
            }
            for t in targets
        ],
        "qc_candidate_source_indices": source_indices,
    })

    report = {
        "schema": "hero-quantum-dictionary-result-v1",
        "corpus": target_stats(targets),
        "view_count": len(views),
        "angles": angles,
        "bands": args.bands,
        "weight_mode": args.weight_mode,
        "metric": {
            "silhouette": args.silhouette_weight,
            "shade": args.shade_weight,
            "brightness_rank": True,
        },
        "requested_patterns": args.patterns,
        "observed_candidate_count": len(candidates),
        "qc_candidate_count": len(qc_candidates),
        "classical_controls": {
            "greedy_history": greedy_hist,
            "swap_history": swap_hist,
            "swap_score": swapped_score,
            "centroid_history": centroid_hist,
            "centroid_score": centroid_score,
        },
        "compact_qubo": {
            "variables": len(compact.variables),
            "linear_terms": len(compact.linear),
            "quadratic_terms": len(compact.quadratic),
            "metadata": compact.metadata,
            "exact_control": exact_compact,
        },
        "assignment_qubo": assignment_meta,
        "notes": {
            "qc_output_must_be_rescored_on_true_facility_objective": True,
            "synthetic_centroids_are_not_required_to_occur_in_oracle_corpus": True,
            "renderer_modified": False,
        },
    }
    atomic_json(outdir / "result.json", report)
    print("HERO_QUANTUM_DICTIONARY_RESULT " +
          json.dumps(report, sort_keys=True))
    print("HERO_QUANTUM_DICTIONARY_PASS")


if __name__ == "__main__":
    main()
