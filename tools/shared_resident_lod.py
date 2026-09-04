#!/usr/bin/env python3
"""Shared cross-angle resident LOD vocabulary optimization.

Each angular group owns its own near-distance resident base dictionary. A small
set of additional LOD patterns is shared by every group. This models the
important hardware/ROM case: the close hero vocabulary may still vary with
view, while a common coarse vocabulary can remain resident across angle and
distance changes.

The implementation caches each group's best local-base match once. Repeated
shared-vocabulary scoring then compares demands only against the shared entries,
which keeps 32-angle sweeps practical.
"""

from resident_tile_dictionary import (
    TileWeights, best_match, canonical_pattern, dedupe_patterns, flip_pattern,
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
        "base_score_cache": None,
    }


def _base_score(group, weights, allow_flips):
    cached = group.get("base_score_cache")
    key = (weights.silhouette, weights.shade, bool(allow_flips))
    if cached is not None and cached.get("key") == key:
        return cached["score"]
    score = score_demands(
        group["demands"], group["base"], weights, allow_flips)
    group["base_score_cache"] = {"key": key, "score": score}
    return score


def score_groups(groups, shared, weights=TileWeights(), allow_flips=True):
    """Score angle-local bases plus one shared add-on vocabulary.

    The expensive local base search is cached. For every later solve we begin
    from that local winner and compare only the shared entries.
    """
    total_cost = 0.0
    exact = 0
    demand_count = 0
    group_scores = []

    for gi, group in enumerate(groups):
        base_score = _base_score(group, weights, allow_flips)
        base_count = len(group["base"])
        matches = [dict(m) for m in base_score["matches"]]

        for si, resident in enumerate(shared):
            dictionary_index = base_count + si
            for di, demand in enumerate(group["demands"]):
                candidate = best_match(
                    demand["pattern"], [resident], weights, allow_flips)
                current = matches[di]
                candidate_key = (
                    candidate.cost, dictionary_index,
                    int(candidate.flip_v), int(candidate.flip_h))
                current_key = (
                    current["cost"], current["dictionary_index"],
                    int(current["flip_v"]), int(current["flip_h"]))
                if candidate_key < current_key:
                    matches[di] = {
                        "demand_index": di,
                        "cost": candidate.cost,
                        "dictionary_index": dictionary_index,
                        "flip_h": candidate.flip_h,
                        "flip_v": candidate.flip_v,
                    }

        group_total = sum(m["cost"] for m in matches)
        group_exact = sum(1 for m in matches if m["cost"] == 0)
        n = len(group["demands"])
        total_cost += group_total
        exact += group_exact
        demand_count += n
        group_scores.append({
            "group_index": gi,
            "base_count": base_count,
            "score": {
                "total_cost": group_total,
                "mean_cost": group_total / n if n else 0.0,
                "exact_count": group_exact,
                "exact_fraction": group_exact / n if n else 1.0,
                "matches": matches,
            },
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
    """Grow one shared vocabulary with incremental per-demand updates."""
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

    # Keep the current winning match tables and compare each newly added shared
    # pattern exactly once against each demand.
    current = []
    for gscore in score["groups"]:
        current.append([dict(m) for m in gscore["score"]["matches"]])

    def rebuild_score():
        total = 0.0
        exact_count = 0
        demand_count = 0
        group_scores = []
        for gi, matches in enumerate(current):
            n = len(matches)
            group_total = sum(m["cost"] for m in matches)
            group_exact = sum(1 for m in matches if m["cost"] == 0)
            total += group_total
            exact_count += group_exact
            demand_count += n
            group_scores.append({
                "group_index": gi,
                "base_count": len(groups[gi]["base"]),
                "score": {
                    "total_cost": group_total,
                    "mean_cost": group_total / n if n else 0.0,
                    "exact_count": group_exact,
                    "exact_fraction": group_exact / n if n else 1.0,
                    "matches": matches,
                },
            })
        return {
            "total_cost": total,
            "mean_cost": total / demand_count if demand_count else 0.0,
            "exact_count": exact_count,
            "exact_fraction": exact_count / demand_count if demand_count else 1.0,
            "demand_count": demand_count,
            "groups": group_scores,
        }

    for step in range(1, additions + 1):
        ranked = []
        for gi, matches in enumerate(current):
            for di, match in enumerate(matches):
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
        si = len(shared) - 1

        for group_i, group in enumerate(groups):
            dictionary_index = len(group["base"]) + si
            for demand_i, demand in enumerate(group["demands"]):
                candidate = best_match(
                    demand["pattern"], [pattern], weights, allow_flips)
                old = current[group_i][demand_i]
                candidate_key = (
                    candidate.cost, dictionary_index,
                    int(candidate.flip_v), int(candidate.flip_h))
                old_key = (
                    old["cost"], old["dictionary_index"],
                    int(old["flip_v"]), int(old["flip_h"]))
                if candidate_key < old_key:
                    current[group_i][demand_i] = {
                        "demand_index": demand_i,
                        "cost": candidate.cost,
                        "dictionary_index": dictionary_index,
                        "flip_h": candidate.flip_h,
                        "flip_v": candidate.flip_v,
                    }

        score = rebuild_score()
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

        worst.sort(reverse=True)
        used = set()
        for si in range(len(candidate)):
            if buckets[si]:
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
