import sys
import unittest
from pathlib import Path


TOOLS = Path(__file__).parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from shared_resident_lod import (
    greedy_shared_growth, prepare_group, refine_shared_dictionary,
    score_groups,
)


class SharedResidentLodTests(unittest.TestCase):
    def test_shared_tile_can_help_two_groups(self):
        zero = bytes(64)
        wanted = bytearray(64)
        wanted[9] = 3
        groups = [
            prepare_group([zero], [{"pattern": bytes(wanted)}]),
            prepare_group([zero], [{"pattern": bytes(wanted)}]),
        ]
        before = score_groups(groups, [])
        grown = greedy_shared_growth(groups, additions=1)
        after = grown["final_score"]
        self.assertGreater(before["total_cost"], after["total_cost"])
        self.assertEqual(after["total_cost"], 0.0)
        self.assertEqual(len(grown["shared"]), 1)

    def test_lloyd_shared_refinement_is_monotonic(self):
        zero = bytes(64)
        d1 = bytearray(64)
        d1[1] = 2
        d2 = bytearray(64)
        d2[1] = 3
        seed = bytearray(64)
        seed[0] = 2
        groups = [
            prepare_group([zero], [{"pattern": bytes(d1)}]),
            prepare_group([zero], [{"pattern": bytes(d2)}]),
        ]
        before = score_groups(groups, [bytes(seed)])
        result = refine_shared_dictionary(groups, [bytes(seed)], iterations=4)
        self.assertLessEqual(
            result["final_score"]["total_cost"], before["total_cost"])


if __name__ == "__main__":
    unittest.main()
