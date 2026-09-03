#!/usr/bin/env python3
"""Alternating co-design solver for the nested Doomguy distance experiment.

This adapter deliberately sits on top of tools/iterative_solver.py.  The generic
solver owns feedback stability, local-minimum escape, and telemetry.  This file
owns the current experiment: shared 8x8 semantic pattern classes, whole-master
phase selectors, DHC1 distance oracles, and histogram-preserving coordinated
pattern reassignment.

The coordinated update is NOT a one-pixel hill climb.  With selectors frozen,
each shared 8x8 pattern is treated as a 64-position assignment problem.  The
existing multiset of shade values is reassigned across all 64 positions in one
shot using a dependency-free Hungarian solve.  Every occurrence of that shared
pattern changes identically.

That makes the current update conservative in colour/shade inventory while
allowing large spatial rearrangements that a greedy adjacent swap cannot reach.
"""

import argparse
import json
import pathlib

from analyze_doomguy_dense_corpus import Corpus, write_ppm
from iterative_solver import (
    AtomicSolverJournal, Evaluation, StableAlternatingSolver,
    min_cost_assignment,
)
from nested_lod_core import (
    LossWeights, Raster, compare, decode_with_pattern_phases,
    distinct_tile_count, fit_pattern_phases, phase_grid,
    projective_source_xy, tile_classes, tile_signature,
)


def sample_raster(corpus, sample):
    out = Raster.blank(corpus.screen_w, corpus.screen_h)
    for y in range(sample.y0, sample.y1 + 1):
        for x in range(sample.x0, sample.x1 + 1):
            out.set(x, y, sample.at(x, y))
    return out


def pixel_cost(got, wanted, weights):
    if bool(got) != bool(wanted):
        return weights.silhouette
    if got:
        return weights.shade * abs(int(got) - int(wanted))
    return 0.0


def apply_pattern_values(raster, origins, pattern, tile_size=8):
    if len(pattern) != tile_size * tile_size:
        raise ValueError("pattern size mismatch")
    for tx, ty in origins:
        x0 = tx * tile_size
        y0 = ty * tile_size
        p = 0
        for y in range(tile_size):
            for x in range(tile_size):
                raster.set(x0 + x, y0 + y, pattern[p])
                p += 1


class NestedDistanceProblem:
    def __init__(self, corpus, angle=0, phase_radius=2,
                 silhouette_weight=12.0, shade_weight=1.0,
                 near_budget=192.0, near_lambda=4.0,
                 near_objective_weight=0.0,
                 pattern_penalty=0.0, selector_byte_penalty=0.0,
                 proposal_limit=48, proposal_class_limit=32):
        self.c = corpus
        self.angle = angle
        self.weights = LossWeights(silhouette_weight, shade_weight)
        self.phases = phase_grid(phase_radius)
        self.near_budget = float(near_budget)
        self.near_lambda = float(near_lambda)
        self.near_objective_weight = float(near_objective_weight)
        self.pattern_penalty = float(pattern_penalty)
        self.selector_byte_penalty = float(selector_byte_penalty)
        self.proposal_limit = int(proposal_limit)
        self.proposal_class_limit = max(
            self.proposal_limit, int(proposal_class_limit))

        if angle < 0 or angle >= corpus.angles:
            raise SystemExit(f"angle {angle} outside 0..{corpus.angles - 1}")
        if corpus.bands < 2:
            raise SystemExit("solver needs at least two distance bands")

        s0 = corpus.band(0)[angle]
        self.master_oracle = sample_raster(corpus, s0)
        self.source_anchor = (s0.anchor_x, s0.anchor_y)
        self.source_radius = corpus.radii[0]

        self.class_map, self.class_origins = tile_classes(self.master_oracle)
        self.zero_key = bytes(64)
        self.class_keys = [
            key for key in sorted(self.class_origins) if any(key)
        ]
        self.class_ids = {key: i for i, key in enumerate(self.class_keys)}
        self.original_patterns = {
            key: tile_signature(
                self.master_oracle,
                self.class_origins[key][0][0],
                self.class_origins[key][0][1])
            for key in self.class_keys
        }

        self.levels = []
        for band in range(1, corpus.bands):
            s = corpus.band(band)[angle]
            self.levels.append({
                "band": band,
                "radius": corpus.radii[band],
                "anchor": (s.anchor_x, s.anchor_y),
                "target": sample_raster(corpus, s),
            })

    def clone_state(self, state):
        return state.copy()

    def fit_model(self, state, iteration):
        tables = []
        for level in self.levels:
            table, loss, _ = fit_pattern_phases(
                state, level["target"], self.source_anchor, level["anchor"],
                self.source_radius, level["radius"], self.class_map,
                self.phases, self.weights)
            tables.append({
                "band": level["band"],
                "radius": level["radius"],
                "table": table,
                "fit_weighted": loss.weighted,
            })
        return {
            "iteration": iteration,
            "tables": tables,
        }

    def _level_by_band(self, band):
        for level in self.levels:
            if level["band"] == band:
                return level
        raise KeyError(band)

    def evaluate(self, state, model):
        near = compare(state, self.master_oracle, self.weights)
        far_weighted = 0.0
        levels = []
        selector_bytes = 0
        for entry in model["tables"]:
            level = self._level_by_band(entry["band"])
            pred = decode_with_pattern_phases(
                state, level["target"], self.source_anchor, level["anchor"],
                self.source_radius, level["radius"], self.class_map,
                entry["table"])
            loss = compare(pred, level["target"], self.weights)
            far_weighted += loss.weighted
            classes = sum(1 for key in entry["table"] if any(key))
            selector_bytes += classes * 2
            levels.append({
                "band": level["band"],
                "radius": level["radius"],
                "silhouette_pct": round(loss.silhouette_pct, 4),
                "shade_abs_mean": round(loss.shade_mean, 4),
                "changed_tiles": loss.changed_tiles,
                "weighted": float(loss.weighted),
            })

        patterns = distinct_tile_count(state)
        objective = (
            far_weighted
            + self.near_objective_weight * near.weighted
            + self.pattern_penalty * patterns
            + self.selector_byte_penalty * selector_bytes
        )
        feasible = near.weighted <= self.near_budget + 1e-9
        metrics = {
            "far_weighted": float(far_weighted),
            "near_weighted": float(near.weighted),
            "near_budget": self.near_budget,
            "near_budget_fraction": (
                round(near.weighted / self.near_budget, 4)
                if self.near_budget > 0 else None
            ),
            "near_silhouette_pct": round(near.silhouette_pct, 4),
            "near_shade_abs_mean": round(near.shade_mean, 4),
            "near_changed_tiles": near.changed_tiles,
            "distinct_master_patterns": patterns,
            "selector_bytes_estimate": selector_bytes,
            "levels": levels,
        }
        return Evaluation(objective, feasible, metrics)

    def model_summary(self, model):
        return {
            "selector_model": "whole-master-pattern-phase",
            "phase_candidates": len(self.phases),
            "proposal_near_lambda": self.near_lambda,
            "near_objective_weight": self.near_objective_weight,
            "distance_tables": [
                {
                    "band": e["band"],
                    "radius": e["radius"],
                    "fit_weighted": float(e["fit_weighted"]),
                    "nonempty_classes": sum(
                        1 for key in e["table"] if any(key)),
                }
                for e in model["tables"]
            ],
        }

    def _sample_class_pos(self, sx, sy):
        ix = int(round(sx))
        iy = int(round(sy))
        if ix < 0 or iy < 0:
            return None, None
        tx, ty = ix // 8, iy // 8
        key = self.class_map.get((tx, ty))
        if key is None or not any(key):
            return None, None
        lx, ly = ix & 7, iy & 7
        return key, ly * 8 + lx

    def _build_far_demand(self, model):
        # demand[class][local_pos][wanted_shade] = use count.
        demand = {
            key: [[0] * 16 for _ in range(64)]
            for key in self.class_keys
        }
        for entry in model["tables"]:
            level = self._level_by_band(entry["band"])
            table = entry["table"]
            for y in range(level["target"].height):
                for x in range(level["target"].width):
                    sx, sy = projective_source_xy(
                        x, y, self.source_anchor, level["anchor"],
                        self.source_radius, level["radius"])
                    center_tx = int(round(sx)) // 8
                    center_ty = int(round(sy)) // 8
                    selector_key = self.class_map.get(
                        (center_tx, center_ty), self.zero_key)
                    dx, dy = table.get(selector_key, (0, 0))
                    key, pos = self._sample_class_pos(sx + dx, sy + dy)
                    if key is None:
                        continue
                    wanted = int(level["target"].at(x, y))
                    if wanted >= len(demand[key][pos]):
                        # Semantic corpus currently stays well below this, but
                        # make the failure loud if that contract ever changes.
                        raise ValueError("semantic shade index exceeds demand bins")
                    demand[key][pos][wanted] += 1
        return demand

    def _pattern_at(self, state, key):
        tx, ty = self.class_origins[key][0]
        return tile_signature(state, tx, ty)

    def _assignment_proposal(self, state, key, demand, proposal_lambda):
        current = self._pattern_at(state, key)
        original = self.original_patterns[key]
        tokens = list(current)  # preserve exact current shade histogram
        occurrence_count = len(self.class_origins[key])

        cost = []
        current_far = 0.0
        current_near = 0.0
        for pos in range(64):
            row = []
            current_value = current[pos]
            far_now = sum(
                count * pixel_cost(current_value, wanted, self.weights)
                for wanted, count in enumerate(demand[key][pos])
            )
            near_now = (
                occurrence_count
                * pixel_cost(current_value, original[pos], self.weights)
            )
            current_far += far_now
            current_near += near_now

            for token_value in tokens:
                far = sum(
                    count * pixel_cost(token_value, wanted, self.weights)
                    for wanted, count in enumerate(demand[key][pos])
                )
                near = (
                    occurrence_count
                    * pixel_cost(token_value, original[pos], self.weights)
                )
                row.append(far + proposal_lambda * near)
            cost.append(row)

        assignment, _ = min_cost_assignment(cost)
        new_pattern = bytes(tokens[assignment[pos]] for pos in range(64))
        if new_pattern == current:
            return None

        new_far = 0.0
        new_near = 0.0
        for pos, value in enumerate(new_pattern):
            new_far += sum(
                count * pixel_cost(value, wanted, self.weights)
                for wanted, count in enumerate(demand[key][pos])
            )
            new_near += (
                occurrence_count
                * pixel_cost(value, original[pos], self.weights)
            )

        far_delta = new_far - current_far
        near_delta = new_near - current_near
        # Mainline proposals exist to improve the actual constrained objective:
        # farther-distance error.  Near quality is handled by the hard budget.
        if far_delta >= -1e-9:
            return None
        return {
            "kind": "hungarian_pattern_reassignment",
            "key": key,
            "class_id": self.class_ids[key],
            "pattern": new_pattern,
            "proposal_lambda": float(proposal_lambda),
            "predicted_far_delta": float(far_delta),
            "predicted_near_delta": float(near_delta),
            "occurrences": occurrence_count,
        }

    def _class_far_upper_bound(self, state, key, demand):
        """Cheap bound: current far cost is the most this class can possibly save."""
        current = self._pattern_at(state, key)
        total = 0.0
        for pos, value in enumerate(current):
            total += sum(
                count * pixel_cost(value, wanted, self.weights)
                for wanted, count in enumerate(demand[key][pos])
            )
        return total

    def main_proposals(self, state, model, iteration):
        demand = self._build_far_demand(model)

        # Hungarian assignment is O(64^3) in Python.  Do not solve it for every
        # class blindly.  Rank classes by a rigorous cheap upper bound on their
        # possible far improvement (their entire current far error), scan the
        # strongest subset first, and only broaden if that fails to fill the
        # requested proposal set.
        ranked_keys = sorted(
            self.class_keys,
            key=lambda key: (
                -self._class_far_upper_bound(state, key, demand),
                self.class_ids[key]))

        lambdas = []
        for value in (
                self.near_lambda,
                self.near_lambda * 0.5,
                self.near_lambda * 0.25,
                0.0):
            value = max(0.0, float(value))
            if not any(abs(value - old) < 1e-12 for old in lambdas):
                lambdas.append(value)

        proposals = []
        seen = set()
        scanned = 0
        target_scan = min(len(ranked_keys), self.proposal_class_limit)

        while scanned < len(ranked_keys):
            stop = min(len(ranked_keys), max(target_scan, scanned + 1))
            for key in ranked_keys[scanned:stop]:
                for proposal_lambda in lambdas:
                    p = self._assignment_proposal(
                        state, key, demand, proposal_lambda)
                    if p is None:
                        continue
                    sig = (p["class_id"], p["pattern"])
                    if sig in seen:
                        continue
                    seen.add(sig)
                    proposals.append(p)
            scanned = stop

            if len(proposals) >= self.proposal_limit:
                break
            # The initial high-opportunity set was unusually barren. Broaden
            # deterministically rather than silently missing lower-cost classes.
            if scanned < len(ranked_keys):
                target_scan = min(
                    len(ranked_keys),
                    max(scanned + 8, scanned * 2))

        proposals.sort(key=lambda p: (
            p["predicted_far_delta"],
            p["predicted_near_delta"],
            p["class_id"],
            -p["proposal_lambda"]))
        self._last_proposal_scan = {
            "classes_total": len(ranked_keys),
            "classes_scanned": scanned,
            "proposals_generated": len(proposals),
        }
        return proposals[:self.proposal_limit]

    def after_iteration(self, best_eval, improved, iteration):
        """Bounded dead-band controller for proposal generation only.

        The accepted objective never depends on this lambda.  It merely changes
        how aggressively the next Hungarian pass is willing to rearrange the
        close pattern while searching for a farther-distance win.
        """
        old = self.near_lambda
        frac = best_eval.metrics.get("near_budget_fraction")
        action = "hold"

        if not improved and (frac is None or frac < 0.35) and old > 0.125:
            self.near_lambda = max(0.125, old * 0.5)
            action = "relax_near_price"
        elif frac is not None and frac > 0.85 and old < 32.0:
            self.near_lambda = min(32.0, old * 1.5)
            action = "tighten_near_price"

        changed = abs(self.near_lambda - old) > 1e-12
        return {
            "controller": "near-budget-deadband-v1",
            "action": action,
            "old_proposal_near_lambda": old,
            "new_proposal_near_lambda": self.near_lambda,
            "near_budget_fraction": frac,
            "force_continue": changed,
        }

    def escape_proposal(self, state, model, rng, iteration, probe, step):
        # A coordinated histogram-preserving shake, not a one-pixel mutation.
        # Two to four swaps can cross a local assignment valley inside one tile.
        keys = self.class_keys
        if not keys:
            return None
        key = keys[rng.randrange(len(keys))]
        pattern = list(self._pattern_at(state, key))
        swaps = 2 + rng.randrange(3)
        changed = False
        chosen = []
        for _ in range(swaps):
            a = rng.randrange(64)
            b = rng.randrange(64)
            if a == b:
                b = (b + 17) & 63
            if pattern[a] != pattern[b]:
                changed = True
            pattern[a], pattern[b] = pattern[b], pattern[a]
            chosen.append((a, b))
        if not changed:
            # Try one deterministic longer jump before giving up this step.
            a = (step * 13 + probe * 7) & 63
            b = (a + 31) & 63
            pattern[a], pattern[b] = pattern[b], pattern[a]
            chosen.append((a, b))
        return {
            "kind": "annealed_multi_swap",
            "key": key,
            "class_id": self.class_ids[key],
            "pattern": bytes(pattern),
            "swaps": chosen,
            "occurrences": len(self.class_origins[key]),
        }

    def apply_proposal(self, state, proposal):
        apply_pattern_values(
            state, self.class_origins[proposal["key"]],
            proposal["pattern"])

    def describe_proposal(self, proposal):
        out = {
            "kind": proposal["kind"],
            "class_id": proposal["class_id"],
            "occurrences": proposal.get("occurrences", 0),
        }
        if "predicted_far_delta" in proposal:
            out["predicted_far_delta"] = round(
                proposal["predicted_far_delta"], 4)
            out["predicted_near_delta"] = round(
                proposal["predicted_near_delta"], 4)
            out["proposal_lambda"] = proposal["proposal_lambda"]
        if "swaps" in proposal:
            out["swap_count"] = len(proposal["swaps"])
            out["swaps"] = proposal["swaps"]
        return out

    def dump_solution(self, state, model, outdir):
        out = pathlib.Path(outdir)
        out.mkdir(parents=True, exist_ok=True)
        write_ppm(
            out / f"a{self.angle:03d}-master-solved.ppm",
            state.width, state.height, state.pixels)
        for entry in model["tables"]:
            level = self._level_by_band(entry["band"])
            pred = decode_with_pattern_phases(
                state, level["target"], self.source_anchor, level["anchor"],
                self.source_radius, level["radius"], self.class_map,
                entry["table"])
            write_ppm(
                out / f"a{self.angle:03d}-b{level['band']}-solved.ppm",
                pred.width, pred.height, pred.pixels)
            write_ppm(
                out / f"a{self.angle:03d}-b{level['band']}-oracle.ppm",
                level["target"].width, level["target"].height,
                level["target"].pixels)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--angle", type=int, default=0)
    ap.add_argument("--phase-radius", type=int, default=2)
    ap.add_argument("--silhouette-weight", type=float, default=12.0)
    ap.add_argument("--shade-weight", type=float, default=1.0)
    ap.add_argument("--near-budget", type=float, default=192.0)
    ap.add_argument("--near-lambda", type=float, default=4.0,
                    help="proposal-only Lagrange price on near-view damage")
    ap.add_argument("--near-objective-weight", type=float, default=0.0,
                    help="optional soft near penalty; hard near-budget still applies")
    ap.add_argument("--pattern-penalty", type=float, default=0.0)
    ap.add_argument("--selector-byte-penalty", type=float, default=0.0)
    ap.add_argument("--proposal-limit", type=int, default=48)
    ap.add_argument("--proposal-class-limit", type=int, default=32,
                    help="initial high-opportunity pattern classes to Hungarian-scan")
    ap.add_argument("--iterations", type=int, default=6)
    ap.add_argument("--patience", type=int, default=2)
    ap.add_argument("--escape-probes", type=int, default=2)
    ap.add_argument("--escape-steps", type=int, default=12)
    ap.add_argument("--escape-temperature", type=float, default=24.0)
    ap.add_argument("--escape-cooling", type=float, default=0.82)
    ap.add_argument("--escape-refit-every", type=int, default=4)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--status-every", type=int, default=8)
    ap.add_argument("--status-json")
    ap.add_argument("--trace-ndjson")
    ap.add_argument("--dump-dir")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    problem = NestedDistanceProblem(
        c, args.angle, args.phase_radius,
        args.silhouette_weight, args.shade_weight,
        args.near_budget, args.near_lambda, args.near_objective_weight,
        args.pattern_penalty, args.selector_byte_penalty,
        args.proposal_limit, args.proposal_class_limit)

    journal = AtomicSolverJournal(
        args.status_json, args.trace_ndjson, echo=True)
    solver = StableAlternatingSolver(
        problem, journal,
        max_iterations=args.iterations,
        patience=args.patience,
        escape_probes=args.escape_probes,
        escape_steps=args.escape_steps,
        escape_temperature=args.escape_temperature,
        escape_cooling=args.escape_cooling,
        escape_refit_every=args.escape_refit_every,
        seed=args.seed,
        status_every=args.status_every)

    best_state, best_model, best_eval = solver.solve(
        problem.master_oracle.copy())

    if args.dump_dir:
        problem.dump_solution(best_state, best_model, args.dump_dir)

    summary = {
        "schema": "nested-distance-solver-summary-v1",
        "angle": args.angle,
        "source_radius": problem.source_radius,
        "radii": [level["radius"] for level in problem.levels],
        "objective": best_eval.objective,
        "feasible": best_eval.feasible,
        "metrics": best_eval.metrics,
    }
    print("NESTED_DISTANCE_SOLVER_SUMMARY " +
          json.dumps(summary, sort_keys=True))
    print("NESTED_DISTANCE_SOLVER_PASS")


if __name__ == "__main__":
    main()
