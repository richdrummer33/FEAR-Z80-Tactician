import sys
import unittest
from pathlib import Path


TOOLS = Path(__file__).parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from sweep_nested_distance_codec import frontier, summarize_angle


class RateDistortionSweepTests(unittest.TestCase):
    def test_frontier_drops_dominated_points(self):
        pts = [
            {"budget": 0, "near_weighted": 0.0, "far_weighted": 100.0,
             "near_silhouette_pct": 0.0},
            {"budget": 64, "near_weighted": 40.0, "far_weighted": 95.0,
             "near_silhouette_pct": 0.1},
            {"budget": 128, "near_weighted": 80.0, "far_weighted": 97.0,
             "near_silhouette_pct": 0.2},
            {"budget": 192, "near_weighted": 100.0, "far_weighted": 88.0,
             "near_silhouette_pct": 0.3},
        ]
        got = frontier(pts)
        self.assertEqual([p["budget"] for p in got], [0, 64, 192])

    def test_angle_summary_reports_best_gain(self):
        pts = [
            {"angle": 0, "budget": 0, "near_weighted": 0.0,
             "near_silhouette_pct": 0.0, "far_weighted": 100.0},
            {"angle": 0, "budget": 64, "near_weighted": 50.0,
             "near_silhouette_pct": 0.2, "far_weighted": 90.0},
        ]
        s = summarize_angle(0, pts)
        self.assertEqual(s["best_improvement_abs"], 10.0)
        self.assertEqual(s["best_improvement_pct"], 10.0)
        self.assertEqual(len(s["frontier"]), 2)


if __name__ == "__main__":
    unittest.main()
