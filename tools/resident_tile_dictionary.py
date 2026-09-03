#!/usr/bin/env python3
"""Hardware-oriented resident 8x8 dictionary primitives.

All patterns are semantic 8x8 rasters represented as 64-byte sequences.
Horizontal/vertical flips are treated as free because Mode-4 name-table entries
can select them without duplicating pattern bytes.

The greedy growth routine starts from a resident base vocabulary and adds exact
oracle demand patterns one at a time.  It is intentionally a measurement bound,
not the final learned codec: if a small number of additions collapses demand
error, later co-design can try to make those additions latent in/reused by the
near vocabulary.
"""

from dataclasses import dataclass


TILE = 8
PIXELS = 64


@dataclass(frozen=True)
class TileWeights:
    silhouette: float = 12.0
    shade: float = 1.0


@dataclass
class Match:
    cost: float
    dictionary_index: int
    flip_h: bool
    flip_v: bool


def flip_pattern(pattern, flip_h=False, flip_v=False):
    if len(pattern) != PIXELS:
        raise ValueError("pattern must contain 64 samples")
    out = bytearray(PIXELS)
    for y in range(TILE):
        sy = TILE - 1 - y if flip_v else y
        for x in range(TILE):
            sx = TILE - 1 - x if flip_h else x
            out[y * TILE + x] = pattern[sy * TILE + sx]
    return bytes(out)


def transforms(pattern):
    return [
        (False, False, bytes(pattern)),
        (True, False, flip_pattern(pattern, True, False)),
        (False, True, flip_pattern(pattern, False, True)),
        (True, True, flip_pattern(pattern, True, True)),
    ]


def canonical_pattern(pattern):
    """Canonicalize exact storage identity modulo free H/V flips."""
    return min(t[2] for t in transforms(pattern))


def pixel_cost(got, wanted, weights=TileWeights()):
    if bool(got) != bool(wanted):
        return weights.silhouette
    if got:
        return weights.shade * abs(int(got) - int(wanted))
    return 0.0


def pattern_cost(got, wanted, weights=TileWeights()):
    if len(got) != PIXELS or len(wanted) != PIXELS:
        raise ValueError("patterns must contain 64 samples")
    return sum(pixel_cost(a, b, weights) for a, b in zip(got, wanted))


def best_match(pattern, dictionary, weights=TileWeights(), allow_flips=True):
    """Find the lowest-cost resident pattern/reference for one output tile."""
    if not dictionary:
        raise ValueError("dictionary must not be empty")
    best = None
    for i, resident in enumerate(dictionary):
        variants = transforms(resident) if allow_flips else [
            (False, False, bytes(resident))]
        for fh, fv, rendered in variants:
            cost = pattern_cost(rendered, pattern, weights)
            key = (cost, i, int(fv), int(fh))
            if best is None or key < best[0]:
                best = (key, Match(cost, i, fh, fv))
    return best[1]


def dedupe_patterns(patterns, modulo_flips=True):
    """Stable exact dedupe, optionally treating hardware flips as equivalent."""
    seen = set()
    out = []
    for pattern in patterns:
        raw = bytes(pattern)
        key = canonical_pattern(raw) if modulo_flips else raw
        if key in seen:
            continue
        seen.add(key)
        # Store the canonical orientation so results are deterministic.
        out.append(key if modulo_flips else raw)
    return out


def score_demands(demands, dictionary, weights=TileWeights(),
                  allow_flips=True):
    """Score demand dictionaries carrying a 64-byte 'pattern' field."""
    total = 0.0
    exact = 0
    worst = None
    matches = []
    for i, demand in enumerate(demands):
        match = best_match(
            demand["pattern"], dictionary, weights, allow_flips)
        total += match.cost
        if match.cost == 0:
            exact += 1
        item = {
            "demand_index": i,
            "cost": match.cost,
            "dictionary_index": match.dictionary_index,
            "flip_h": match.flip_h,
            "flip_v": match.flip_v,
        }
        matches.append(item)
        key = (match.cost, i)
        if worst is None or key > worst[0]:
            worst = (key, item)
    return {
        "total_cost": total,
        "mean_cost": total / len(demands) if demands else 0.0,
        "exact_count": exact,
        "exact_fraction": exact / len(demands) if demands else 1.0,
        "worst": worst[1] if worst else None,
        "matches": matches,
    }


def greedy_dictionary_growth(demands, base_dictionary, additions,
                             weights=TileWeights(), allow_flips=True):
    """Add the currently worst represented oracle pattern per step.

    Matching is updated incrementally: after the initial base-vocabulary pass,
    each new resident candidate is compared once against every demand rather
    than rescoring the whole dictionary. This keeps larger offline sweeps cheap.
    """
    dictionary = dedupe_patterns(base_dictionary, modulo_flips=allow_flips)
    zero = bytes(PIXELS)
    if zero not in dictionary:
        dictionary.insert(0, zero)
    base_count = len(dictionary)

    score = score_demands(demands, dictionary, weights, allow_flips)
    current_matches = list(score["matches"])

    def snapshot(step, added_from):
        total = sum(m["cost"] for m in current_matches)
        exact = sum(1 for m in current_matches if m["cost"] == 0)
        return {
            "additions": step,
            "dictionary_count": len(dictionary),
            "total_cost": total,
            "mean_cost": total / len(demands) if demands else 0.0,
            "exact_fraction": exact / len(demands) if demands else 1.0,
            "added_from_demand": added_from,
        }

    history = [snapshot(0, None)]
    known = {
        canonical_pattern(p) if allow_flips else bytes(p)
        for p in dictionary
    }

    for step in range(1, additions + 1):
        ranked = sorted(
            enumerate(current_matches),
            key=lambda pair: (pair[1]["cost"], pair[0]),
            reverse=True)
        chosen = None
        for demand_i, match in ranked:
            pattern = bytes(demands[demand_i]["pattern"])
            key = canonical_pattern(pattern) if allow_flips else pattern
            if key not in known:
                chosen = (demand_i, key)
                break
        if chosen is None:
            break

        demand_i, pattern = chosen
        dictionary.append(pattern)
        known.add(pattern)
        new_index = len(dictionary) - 1
        variants = transforms(pattern) if allow_flips else [
            (False, False, pattern)]

        for i, demand in enumerate(demands):
            best = current_matches[i]
            for fh, fv, rendered in variants:
                cost = pattern_cost(rendered, demand["pattern"], weights)
                key = (cost, new_index, int(fv), int(fh))
                old_key = (
                    best["cost"], best["dictionary_index"],
                    int(best["flip_v"]), int(best["flip_h"]))
                if key < old_key:
                    best = {
                        "demand_index": i,
                        "cost": cost,
                        "dictionary_index": new_index,
                        "flip_h": fh,
                        "flip_v": fv,
                    }
            current_matches[i] = best

        history.append(snapshot(step, demand_i))
        if history[-1]["total_cost"] <= 0.0:
            break

    total = sum(m["cost"] for m in current_matches)
    exact = sum(1 for m in current_matches if m["cost"] == 0)
    final_score = {
        "total_cost": total,
        "mean_cost": total / len(demands) if demands else 0.0,
        "exact_count": exact,
        "exact_fraction": exact / len(demands) if demands else 1.0,
        "worst": (
            max(current_matches, key=lambda m: (m["cost"], m["demand_index"]))
            if current_matches else None),
        "matches": current_matches,
    }
    return {
        "base_count_with_implicit_empty": base_count,
        "dictionary": dictionary,
        "history": history,
        "final_score": final_score,
    }
