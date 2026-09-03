#!/usr/bin/env python3
"""Reusable stable alternating optimizer + LLM-friendly runtime telemetry.

The optimizer deliberately knows nothing about Doomguy, DHC1, tiles, or image
compression.  A problem adapter supplies state/model/proposal operations.

Design goals:
- accepted mainline state is monotonic: never publish a worse best solution;
- model/state alternating passes are explicit, so feedback cannot silently
  oscillate;
- local-minimum escapes run in disposable probes with deterministic annealing;
- a probe may temporarily accept worse candidates, but it can replace mainline
  state only after a full refit/evaluation proves a real improvement;
- progress is continuously exposed as atomic JSON, optional NDJSON history, and
  compact stdout records suitable for polling by a human or LLM.
"""

from dataclasses import dataclass, field
import copy
import json
import math
import os
import random
import time


@dataclass
class Evaluation:
    objective: float
    feasible: bool = True
    metrics: dict = field(default_factory=dict)

    def as_dict(self):
        return {
            "objective": float(self.objective),
            "feasible": bool(self.feasible),
            "metrics": self.metrics,
        }


class AtomicSolverJournal:
    """Continuously publish one fresh snapshot plus an append-only event trace."""

    def __init__(self, status_path=None, trace_path=None, echo=True):
        self.status_path = status_path
        self.trace_path = trace_path
        self.echo = echo
        self.started = time.time()
        self.sequence = 0
        self.history_tail = []

    @staticmethod
    def _atomic_json(path, payload):
        if not path:
            return
        directory = os.path.dirname(path)
        if directory:
            os.makedirs(directory, exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(payload, f, sort_keys=True, indent=2)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)

    def publish(self, phase, iteration, sub_iteration, current_eval, best_eval,
                details=None, config=None, status="running"):
        self.sequence += 1
        event = {
            "schema": "stable-alternating-solver-v1",
            "sequence": self.sequence,
            "elapsed_sec": round(time.time() - self.started, 3),
            "status": status,
            "phase": phase,
            "iteration": int(iteration),
            "sub_iteration": int(sub_iteration),
            "current": current_eval.as_dict() if current_eval else None,
            "best": best_eval.as_dict() if best_eval else None,
            "details": details or {},
        }
        self.history_tail.append({
            "sequence": event["sequence"],
            "phase": phase,
            "iteration": int(iteration),
            "sub_iteration": int(sub_iteration),
            "current_objective": (None if current_eval is None
                                  else float(current_eval.objective)),
            "best_objective": (None if best_eval is None
                               else float(best_eval.objective)),
            "details": details or {},
        })
        self.history_tail = self.history_tail[-12:]
        snapshot = dict(event)
        snapshot["config"] = config or {}
        snapshot["history_tail"] = self.history_tail
        self._atomic_json(self.status_path, snapshot)

        if self.trace_path:
            directory = os.path.dirname(self.trace_path)
            if directory:
                os.makedirs(directory, exist_ok=True)
            with open(self.trace_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(event, sort_keys=True) + "\n")
                f.flush()

        if self.echo:
            # One JSON object per line: intentionally easy to grep/poll.
            print("LLM_SOLVER_STATUS " + json.dumps(event, sort_keys=True),
                  flush=True)


def min_cost_assignment(cost):
    """Hungarian algorithm for a square numeric cost matrix.

    Returns assignment[row] = column and total cost.  O(n^3), deterministic,
    dependency-free, and fast enough for 64x64 tile assignments.
    """
    n = len(cost)
    if n == 0:
        return [], 0.0
    if any(len(row) != n for row in cost):
        raise ValueError("cost matrix must be square")

    # 1-indexed implementation of the shortest augmenting path formulation.
    u = [0.0] * (n + 1)
    v = [0.0] * (n + 1)
    p = [0] * (n + 1)
    way = [0] * (n + 1)

    for i in range(1, n + 1):
        p[0] = i
        j0 = 0
        minv = [float("inf")] * (n + 1)
        used = [False] * (n + 1)
        while True:
            used[j0] = True
            i0 = p[j0]
            delta = float("inf")
            j1 = 0
            row = cost[i0 - 1]
            for j in range(1, n + 1):
                if used[j]:
                    continue
                cur = float(row[j - 1]) - u[i0] - v[j]
                if cur < minv[j]:
                    minv[j] = cur
                    way[j] = j0
                if minv[j] < delta:
                    delta = minv[j]
                    j1 = j
            for j in range(0, n + 1):
                if used[j]:
                    u[p[j]] += delta
                    v[j] -= delta
                else:
                    minv[j] -= delta
            j0 = j1
            if p[j0] == 0:
                break
        while True:
            j1 = way[j0]
            p[j0] = p[j1]
            j0 = j1
            if j0 == 0:
                break

    assignment = [-1] * n
    for j in range(1, n + 1):
        if p[j]:
            assignment[p[j] - 1] = j - 1
    total = sum(float(cost[i][assignment[i]]) for i in range(n))
    return assignment, total


class StableAlternatingSolver:
    """Monotonic mainline + disposable annealed escape probes.

    Problem adapter contract:
      clone_state(state)
      fit_model(state, iteration) -> opaque model
      evaluate(state, model) -> Evaluation
      main_proposals(state, model, iteration) -> iterable proposals
      escape_proposal(state, model, rng, iteration, probe, step) -> proposal/None
      apply_proposal(state, proposal) -> None
      describe_proposal(proposal) -> JSON-able dict
    """

    def __init__(self, problem, journal=None, max_iterations=8, patience=2,
                 epsilon=1e-6, escape_probes=2, escape_steps=16,
                 escape_temperature=24.0, escape_cooling=0.82,
                 escape_refit_every=4, seed=1, status_every=1):
        self.problem = problem
        self.journal = journal or AtomicSolverJournal(echo=False)
        self.max_iterations = max_iterations
        self.patience = patience
        self.epsilon = epsilon
        self.escape_probes = escape_probes
        self.escape_steps = escape_steps
        self.escape_temperature = escape_temperature
        self.escape_cooling = escape_cooling
        self.escape_refit_every = max(1, escape_refit_every)
        self.seed = seed
        self.status_every = max(1, status_every)

    def _better(self, a, b):
        return (a.feasible and
                (not b.feasible or a.objective < b.objective - self.epsilon))

    def solve(self, initial_state):
        cfg = {
            "max_iterations": self.max_iterations,
            "patience": self.patience,
            "epsilon": self.epsilon,
            "escape_probes": self.escape_probes,
            "escape_steps": self.escape_steps,
            "escape_temperature": self.escape_temperature,
            "escape_cooling": self.escape_cooling,
            "escape_refit_every": self.escape_refit_every,
            "seed": self.seed,
        }
        state = self.problem.clone_state(initial_state)
        model = self.problem.fit_model(state, 0)
        current = self.problem.evaluate(state, model)
        best_state = self.problem.clone_state(state)
        best_model = model
        best = current

        self.journal.publish("initial", 0, 0, current, best,
                             self.problem.model_summary(model), cfg)

        stagnant = 0
        for iteration in range(1, self.max_iterations + 1):
            iter_start_best = best.objective
            # Exact model refit first: one side of the alternating minimization.
            model = self.problem.fit_model(state, iteration)
            current = self.problem.evaluate(state, model)
            self.journal.publish(
                "selector_refit", iteration, 0, current, best,
                self.problem.model_summary(model), cfg)

            accepted = 0
            proposals = list(self.problem.main_proposals(
                state, model, iteration))
            for sub, proposal in enumerate(proposals, 1):
                cand = self.problem.clone_state(state)
                self.problem.apply_proposal(cand, proposal)
                ev = self.problem.evaluate(cand, model)
                did_accept = self._better(ev, current)
                if did_accept:
                    state = cand
                    current = ev
                    accepted += 1

                if did_accept or sub % self.status_every == 0:
                    details = self.problem.describe_proposal(proposal)
                    details.update({
                        "accepted": did_accept,
                        "accepted_in_stage": accepted,
                        "proposals_total": len(proposals),
                    })
                    self.journal.publish(
                        "master_update", iteration, sub, current, best,
                        details, cfg)

            # Refit after the pattern-side moves.  If this somehow regresses the
            # full objective, do not let it poison the accepted best state.
            model = self.problem.fit_model(state, iteration)
            current = self.problem.evaluate(state, model)
            if self._better(current, best):
                best_state = self.problem.clone_state(state)
                best_model = model
                best = current
                improved = True
            else:
                improved = False
                if current.objective > best.objective + self.epsilon:
                    state = self.problem.clone_state(best_state)
                    model = best_model
                    current = best

            self.journal.publish(
                "iteration_commit", iteration, len(proposals), current, best,
                {
                    "accepted_mainline": accepted,
                    "improved_global_best": improved,
                    "iteration_start_best": iter_start_best,
                    "iteration_end_best": best.objective,
                }, cfg)

            # Local-minimum escape.  Each probe is disposable.  It can walk
            # uphill according to annealing, but only a fully refitted,
            # feasible improvement is allowed back onto the mainline.
            escaped = False
            if not improved and self.escape_probes and self.escape_steps:
                for probe in range(self.escape_probes):
                    rng = random.Random(
                        self.seed + iteration * 1000003 + probe * 9176)
                    probe_state = self.problem.clone_state(best_state)
                    probe_model = self.problem.fit_model(
                        probe_state, iteration)
                    probe_eval = self.problem.evaluate(
                        probe_state, probe_model)
                    temperature = self.escape_temperature
                    probe_accepts = 0

                    for step in range(1, self.escape_steps + 1):
                        proposal = self.problem.escape_proposal(
                            probe_state, probe_model, rng, iteration,
                            probe, step)
                        if proposal is None:
                            break
                        cand = self.problem.clone_state(probe_state)
                        self.problem.apply_proposal(cand, proposal)
                        ev = self.problem.evaluate(cand, probe_model)
                        delta = ev.objective - probe_eval.objective
                        accept = False
                        if ev.feasible:
                            if delta <= 0.0:
                                accept = True
                            elif temperature > 0.0:
                                # Deterministic for a fixed seed.
                                accept = rng.random() < math.exp(
                                    -delta / temperature)
                        if accept:
                            probe_state = cand
                            probe_eval = ev
                            probe_accepts += 1

                        if step % self.escape_refit_every == 0:
                            probe_model = self.problem.fit_model(
                                probe_state, iteration)
                            probe_eval = self.problem.evaluate(
                                probe_state, probe_model)

                        if accept or step % self.status_every == 0:
                            details = self.problem.describe_proposal(proposal)
                            details.update({
                                "probe": probe,
                                "step": step,
                                "anneal_accept": accept,
                                "delta": delta,
                                "temperature": temperature,
                                "probe_accepts": probe_accepts,
                            })
                            self.journal.publish(
                                "escape_probe", iteration, step,
                                probe_eval, best, details, cfg)
                        temperature *= self.escape_cooling

                    probe_model = self.problem.fit_model(
                        probe_state, iteration)
                    probe_eval = self.problem.evaluate(
                        probe_state, probe_model)
                    promoted = self._better(probe_eval, best)
                    if promoted:
                        state = probe_state
                        model = probe_model
                        current = probe_eval
                        best_state = self.problem.clone_state(probe_state)
                        best_model = probe_model
                        best = probe_eval
                        escaped = True
                    self.journal.publish(
                        "escape_commit", iteration, probe, current, best,
                        {
                            "probe": probe,
                            "probe_accepts": probe_accepts,
                            "promoted_to_mainline": promoted,
                            "probe_objective": probe_eval.objective,
                        }, cfg)
                    if promoted:
                        break

            feedback = {}
            if hasattr(self.problem, "after_iteration"):
                feedback = self.problem.after_iteration(
                    best, improved or escaped, iteration) or {}
                self.journal.publish(
                    "feedback_control", iteration, 0, current, best,
                    feedback, cfg)

            if best.objective < iter_start_best - self.epsilon:
                stagnant = 0
            elif feedback.get("force_continue", False):
                # A bounded controller changed a search parameter.  Give that
                # new setting one complete alternating pass before declaring
                # convergence.
                stagnant = 0
            else:
                stagnant += 1

            if stagnant >= self.patience:
                self.journal.publish(
                    "converged", iteration, 0, current, best,
                    {
                        "reason": "patience_exhausted",
                        "stagnant_iterations": stagnant,
                    }, cfg, status="converged")
                return best_state, best_model, best

        self.journal.publish(
            "complete", self.max_iterations, 0, current, best,
            {"reason": "iteration_limit"}, cfg, status="complete")
        return best_state, best_model, best
