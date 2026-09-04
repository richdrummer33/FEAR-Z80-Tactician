#!/usr/bin/env python3
"""Shared cross-angle resident LOD vocabulary optimization.

Each angular group owns its own near-distance resident base dictionary.  A
small set of additional LOD patterns is shared by every group.  This models the
important hardware/ROM case: the close hero vocabulary may still vary with
view, while a common coarse vocabulary can remain resident across angle and
distance changes.

The module provides:
- greedy shared dictionary growth from the globally worst represented demand;
- monotonic Lloyd-style refinement of only the shared entries;
- free H/V flips, matching Mode-4 name-table capabilities.
"""

from resident_tile_dictionary import (
    TileWeights, canonical_pattern, dedupe_patterns, flip_pattern,
    score_demands,
)
from resident_tile_lloyd import _optimal_value


def prepare_group(base_dictionary, demands, allow_flips=True):
    base = dedupe_patterns(base_dictionary, modulo_flips=allow_flips)
    zero = bytes(64)
    if zero not in base:
        base.insert(0, zero)
    return {
        "base": base,
        "demands": demands,
    }


def score_groups(groups, shared, weights=TileWeights(), allow_flips=True):
    total_cost = 0.0
    exact = 0
    demand_count = 0
    group_scores = []
    for gi, group in enumerate(groups):
        dictionary = group["base"] + list(shared)
        score = score_demands(
            group["demands"], dictionary, weights, allow_flips)
        total_cost += score["total_cost"]
        exact += score["exact_count"]
        demand_count += len(group["demands"])
        group_scores.append({
            "group_index": gi,
            "base_count": len(group["base"]),
            "score": score,
        })
    return {
        "total_cost": total_cost,
        "mean_cost": total_cost / demand_count if demand_count else 0.0,
        "exact_count": exact,
        "exact_fraction": exact / demand_count if demand_count else 1.0,
        "demand_count": demand_count,
        "groups": group_scores,
    }


def greedy_shared_growth(groups, additions, weights=TileWeights(),
                         allow_flips=True):
    shared = []
    known = set()
    score = score_groups(groups, shared, weights, allow_flips)
    history = [{
        "additions": 0,
        "total_cost": score["total_cost"],
        "mean_cost": score["mean_cost"],
        "exact_fraction": score["exact_fraction"],
        "added_group": None,
        "added_demand": None,
    }]

    for step in range(1, additions + 1):
        ranked = []
        for gscore in score["groups"]:
            gi = gscore["group_index"]
            for di, match in enumerate(gscore["score"]["matches"]):
                ranked.append((match["cost"], gi, di))
        ranked.sort(reverse=True)

        chosen = None
        for cost, gi, di in ranked:
            if cost <= 0:
                break
            raw = bytes(groups[gi]["demands"][di]["pattern"])
            key = canonical_pattern(raw) if allow_flips else raw
            if key in known:
                continue
            chosen = (gi, di, key)
            break
        if chosen is None:
            break

        gi, di, pattern = chosen
        shared.append(pattern)
        known.add(pattern)
        score = score_groups(groups, shared, weights, allow_flips)
        history.append({
            "additions": step,
            "total_cost": score["total_cost"],
            "mean_cost": score["mean_cost"],
            "exact_fraction": score["exact_fraction"],
            "added_group": gi,
            "added_demand": di,
        })
        if score["total_cost"] <= 0:
            break

    return {
        "shared": shared,
        "history": history,
        "final_score": score,
    }


def refine_shared_dictionary(groups, shared, iterations=4,
                             weights=TileWeights(), allow_flips=True):
    current = [bytes(p) for p in shared]
    current_score = score_groups(groups, current, weights, allow_flips)
    current_cost = current_score["total_cost"]
    history = [{
        "iteration": 0,
        "total_cost": current_cost,
        "mean_cost": current_score["mean_cost"],
        "exact_fraction": current_score["exact_fraction"],
        "accepted": True,
        "changed_entries": 0,
    }]

    for iteration in range(1, iterations + 1):
        buckets = {i: [] for i in range(len(current))}
        worst = []

        for gscore in current_score["groups"]:
            gi = gscore["group_index"]
            base_count = gscore["base_count"]
            demands = groups[gi]["demands"]
            for di, (demand, match) in enumerate(
                    zip(demands, gscore["score"]["matches"])):
                worst.append((match["cost"], gi, di))
                if match["dictionary_index"] < base_count:
                    continue
                si = match["dictionary_index"] - base_count
                if si < 0 or si >= len(current):
                    continue
                aligned = flip_pattern(
                    demand["pattern"], match["flip_h"], match["flip_v"])
                buckets[si].append(aligned)

        candidate = list(current)
        changed = 0

        # Dead shared entries are reseeded from globally difficult demands.
        worst.sort(reverse=True)
        used = set()
        for si in range(len(candidate)):
            assigned = buckets[si]
            if assigned:
                continue
            seed = None
            for cost, gi, di in worst:
                if cost <= 0:
                    break
                raw = bytes(groups[gi]["demands"][di]["pattern"])
                key = canonical_pattern(raw) if allow_flips else raw
                if key in used:
                    continue
                used.add(key)
                seed = key
                break
            if seed is not None and seed != candidate[si]:
                candidate[si] = seed
                changed += 1

        for si in range(len(candidate)):
            assigned = buckets[si]
            if not assigned:
                continue
            out = bytearray(64)
            for pos in range(64):
                out[pos] = _optimal_value(
                    [p[pos] for p in assigned], weights)
            pattern = bytes(out)
            if allow_flips:
                pattern = canonical_pattern(pattern)
            if pattern != candidate[si]:
                candidate[si] = pattern
                changed += 1

        candidate_score = score_groups(
            groups, candidate, weights, allow_flips)
        accepted = candidate_score["total_cost"] <= current_cost + 1e-9
        if accepted:
            current = candidate
            current_score = candidate_score
            current_cost = candidate_score["total_cost"]

        history.append({
            "iteration": iteration,
            "total_cost": current_cost,
            "mean_cost": current_score["mean_cost"],
            "exact_fraction": current_score["exact_fraction"],
            "accepted": accepted,
            "changed_entries": changed,
        })

        if not accepted or changed == 0:
            break
        if abs(history[-2]["total_cost"] - history[-1]["total_cost"]) < 1e-9:
            break

    return {
        "shared": current,
        "history": history,
        "final_score": current_score,
    }
