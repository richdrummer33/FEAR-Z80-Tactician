#!/usr/bin/env python3
"""Monotonic Lloyd-style refinement for resident semantic 8x8 dictionaries.

The first N dictionary entries are fixed (typically implicit empty + the
near-distance resident vocabulary). Remaining entries are learnable LOD
patterns.

Each iteration:
1. assign every far demand to its cheapest dictionary tile + free H/V flip;
2. inverse-flip assigned demands into each learned tile's canonical frame;
3. choose the independently optimal semantic shade at every one of 64 pixels;
4. reassign and accept the whole iteration only if total demand cost does not
   increase.

This is discrete vector quantization tailored to the Game Gear's actual 8x8
pattern atom. Runtime remains a name-table tile reference plus flip bits.
"""

from resident_tile_dictionary import (
    TileWeights, best_match, canonical_pattern, flip_pattern,
    pattern_cost, pixel_cost, score_demands,
)


def _optimal_value(values, weights):
    """Cheapest single shade code for one pixel across every assigned demand.

    Candidates are the observed codes plus transparent. The cost has to be
    measured in the same space the matcher uses, so this defers to pixel_cost
    rather than re-deriving the arithmetic -- an earlier version duplicated it
    and silently kept raw-index distance after the matcher moved to ranks.
    """
    if not values:
        return 0
    candidates = sorted(set([0] + [int(v) for v in values]))
    best = None
    for value in candidates:
        cost = sum(pixel_cost(value, wanted, weights) for wanted in values)
        key = (cost, value)
        if best is None or key < best[0]:
            best = (key, value)
    return best[1]


def refine_learned_dictionary(demands, dictionary, fixed_count,
                              iterations=4, weights=TileWeights(),
                              allow_flips=True):
    """Refine mutable dictionary entries while keeping fixed resident seeds."""
    if fixed_count < 0 or fixed_count > len(dictionary):
        raise ValueError("fixed_count outside dictionary")
    current = [bytes(p) for p in dictionary]
    initial = score_demands(demands, current, weights, allow_flips)
    current_cost = initial["total_cost"]
    history = [{
        "iteration": 0,
        "total_cost": current_cost,
        "mean_cost": initial["mean_cost"],
        "exact_fraction": initial["exact_fraction"],
        "accepted": True,
        "changed_entries": 0,
    }]

    for iteration in range(1, iterations + 1):
        score = score_demands(demands, current, weights, allow_flips)
        buckets = {i: [] for i in range(fixed_count, len(current))}

        for demand, match in zip(demands, score["matches"]):
            j = match["dictionary_index"]
            if j < fixed_count:
                continue
            # A flip is its own inverse. Bring the target pattern into the
            # resident entry's canonical coordinate frame before centroiding.
            aligned = flip_pattern(
                demand["pattern"], match["flip_h"], match["flip_v"])
            buckets[j].append(aligned)

        candidate = list(current)
        changed = 0

        # Classic dead-cluster repair. A learned slot can initially be worse
        # than every fixed near/empty entry and therefore receive no demands.
        # Reseed such slots from the currently worst represented demands so
        # they get a chance to become useful on the next assignment pass.
        empty_entries = [
            j for j in range(fixed_count, len(candidate))
            if not buckets[j]
        ]
        ranked_demands = sorted(
            range(len(demands)),
            key=lambda i: (score["matches"][i]["cost"], i),
            reverse=True)
        used_seeds = set()
        for j in empty_entries:
            seed = None
            for i in ranked_demands:
                if score["matches"][i]["cost"] <= 0:
                    break
                pattern = bytes(demands[i]["pattern"])
                if allow_flips:
                    pattern = canonical_pattern(pattern)
                if pattern in used_seeds:
                    continue
                used_seeds.add(pattern)
                seed = pattern
                break
            if seed is not None and seed != candidate[j]:
                candidate[j] = seed
                changed += 1

        for j in range(fixed_count, len(candidate)):
            assigned = buckets[j]
            if not assigned:
                continue
            out = bytearray(64)
            for pos in range(64):
                out[pos] = _optimal_value(
                    [p[pos] for p in assigned], weights)
            pattern = bytes(out)
            if allow_flips:
                pattern = canonical_pattern(pattern)
            if pattern != candidate[j]:
                candidate[j] = pattern
                changed += 1

        candidate_score = score_demands(
            demands, candidate, weights, allow_flips)
        accepted = candidate_score["total_cost"] <= current_cost + 1e-9
        if accepted:
            current = candidate
            current_cost = candidate_score["total_cost"]
            final_score = candidate_score
        else:
            final_score = score

        history.append({
            "iteration": iteration,
            "total_cost": (
                candidate_score["total_cost"] if accepted else current_cost),
            "mean_cost": (
                candidate_score["mean_cost"] if accepted
                else score["mean_cost"]),
            "exact_fraction": (
                candidate_score["exact_fraction"] if accepted
                else score["exact_fraction"]),
            "accepted": accepted,
            "changed_entries": changed,
        })

        if not accepted or changed == 0:
            break
        if len(history) >= 2 and abs(
                history[-2]["total_cost"] -
                history[-1]["total_cost"]) < 1e-9:
            break

    final_score = score_demands(
        demands, current, weights, allow_flips)
    return {
        "dictionary": current,
        "fixed_count": fixed_count,
        "history": history,
        "final_score": final_score,
    }
