import json
import os
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from iterative_solver import (
    AtomicSolverJournal, Evaluation, StableAlternatingSolver,
    min_cost_assignment,
)


class AssignmentTests(unittest.TestCase):
    def test_hungarian_finds_known_minimum(self):
        cost = [
            [4, 1, 3],
            [2, 0, 5],
            [3, 2, 2],
        ]
        assignment, total = min_cost_assignment(cost)
        self.assertEqual(total, 5.0)
        self.assertEqual(sorted(assignment), [0, 1, 2])

    def test_hungarian_preserves_duplicate_tokens_by_column(self):
        cost = [
            [0, 0, 8],
            [7, 7, 0],
            [1, 1, 9],
        ]
        assignment, total = min_cost_assignment(cost)
        self.assertEqual(sorted(assignment), [0, 1, 2])
        self.assertEqual(total, 1.0)


class JournalTests(unittest.TestCase):
    def test_atomic_status_and_trace_are_machine_readable(self):
        with tempfile.TemporaryDirectory() as td:
            status = os.path.join(td, "status.json")
            trace = os.path.join(td, "trace.ndjson")
            j = AtomicSolverJournal(status, trace, echo=False)
            ev = Evaluation(12.5, True, {"near": 3})
            j.publish("test", 2, 7, ev, ev, {"accepted": True},
                      {"seed": 1})
            payload = json.loads(Path(status).read_text())
            self.assertEqual(payload["schema"], "stable-alternating-solver-v1")
            self.assertEqual(payload["phase"], "test")
            self.assertEqual(payload["current"]["objective"], 12.5)
            lines = Path(trace).read_text().splitlines()
            self.assertEqual(len(lines), 1)
            self.assertEqual(json.loads(lines[0])["sub_iteration"], 7)


class _ToyProblem:
    """1-D problem with a local minimum that escape can cross."""

    def clone_state(self, state):
        return list(state)

    def fit_model(self, state, iteration):
        return {"iteration": iteration}

    def evaluate(self, state, model):
        x = state[0]
        # x=0 local basin, x=3 global basin.
        table = {0: 4.0, 1: 5.0, 2: 4.5, 3: 0.0}
        return Evaluation(table[x], True, {"x": x})

    def model_summary(self, model):
        return dict(model)

    def main_proposals(self, state, model, iteration):
        # From zero, direct move to one is worse so monotonic mainline stalls.
        if state[0] == 0:
            return [{"kind": "step", "x": 1}]
        return []

    def escape_proposal(self, state, model, rng, iteration, probe, step):
        x = state[0]
        if x >= 3:
            return None
        return {"kind": "step", "x": x + 1}

    def apply_proposal(self, state, proposal):
        state[0] = proposal["x"]

    def describe_proposal(self, proposal):
        return dict(proposal)


class StableSolverTests(unittest.TestCase):
    def test_escape_probe_may_walk_uphill_but_only_promotes_better_final(self):
        problem = _ToyProblem()
        solver = StableAlternatingSolver(
            problem,
            AtomicSolverJournal(echo=False),
            max_iterations=2,
            patience=2,
            escape_probes=1,
            escape_steps=3,
            escape_temperature=1000.0,
            escape_cooling=1.0,
            escape_refit_every=1,
            seed=2,
            status_every=1,
        )
        state, model, ev = solver.solve([0])
        self.assertEqual(state[0], 3)
        self.assertEqual(ev.objective, 0.0)


if __name__ == "__main__":
    unittest.main()
