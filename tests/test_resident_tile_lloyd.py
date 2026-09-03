import sys
import unittest
from pathlib import Path


TOOLS = Path(__file__).parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from resident_tile_dictionary import score_demands
from resident_tile_lloyd import refine_learned_dictionary


class ResidentTileLloydTests(unittest.TestCase):
    def test_refinement_is_monotonic_and_keeps_fixed_seed(self):
        fixed = bytes(64)
        learned = bytearray(64)
        learned[0] = 1
        d1 = bytearray(64)
        d1[0] = 3
        d1[1] = 2
        d2 = bytearray(64)
        d2[0] = 3
        d2[1] = 2
        demands = [{"pattern": bytes(d1)}, {"pattern": bytes(d2)}]
        before = score_demands(demands, [fixed, bytes(learned)])
        result = refine_learned_dictionary(
            demands, [fixed, bytes(learned)], fixed_count=1, iterations=4)
        after = result["final_score"]
        self.assertLessEqual(after["total_cost"], before["total_cost"])
        self.assertEqual(result["dictionary"][0], fixed)

    def test_exact_cluster_can_be_learned(self):
        fixed = bytes(64)
        seed = bytearray(64)
        seed[0] = 1
        wanted = bytearray(64)
        wanted[9] = 4
        demands = [{"pattern": bytes(wanted)} for _ in range(3)]
        result = refine_learned_dictionary(
            demands, [fixed, bytes(seed)], fixed_count=1, iterations=4)
        self.assertEqual(result["final_score"]["total_cost"], 0.0)


if __name__ == "__main__":
    unittest.main()
