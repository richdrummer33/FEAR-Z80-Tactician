import sys
import unittest
from pathlib import Path


TOOLS = Path(__file__).parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import resident_tile_dictionary as rtd


class ResidentTileDictionaryTests(unittest.TestCase):
    def test_flip_canonicalization_merges_free_hardware_variants(self):
        p = bytearray(64)
        p[1] = 3
        q = rtd.flip_pattern(bytes(p), flip_h=True)
        got = rtd.dedupe_patterns([bytes(p), q], modulo_flips=True)
        self.assertEqual(len(got), 1)

    def test_best_match_reports_flip(self):
        p = bytearray(64)
        p[1] = 4
        demand = rtd.flip_pattern(bytes(p), flip_h=True)
        m = rtd.best_match(demand, [bytes(p)])
        self.assertEqual(m.cost, 0.0)
        self.assertTrue(m.flip_h)

    def test_greedy_growth_monotonically_reduces_cost(self):
        base = bytearray(64)
        base[0] = 1
        d1 = bytearray(64)
        d1[10] = 2
        d2 = bytearray(64)
        d2[20] = 3
        demands = [{"pattern": bytes(d1)}, {"pattern": bytes(d2)}]
        result = rtd.greedy_dictionary_growth(
            demands, [bytes(base)], additions=2)
        costs = [h["total_cost"] for h in result["history"]]
        self.assertEqual(costs, sorted(costs, reverse=True))
        self.assertEqual(costs[-1], 0.0)

    def test_empty_pattern_is_implicit_candidate(self):
        demand = bytearray(64)
        demand[0] = 1
        result = rtd.greedy_dictionary_growth(
            [{"pattern": bytes(demand)}], [], additions=0)
        self.assertEqual(result["base_count_with_implicit_empty"], 1)


if __name__ == "__main__":
    unittest.main()
