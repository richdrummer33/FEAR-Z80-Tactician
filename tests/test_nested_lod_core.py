import importlib.util
import sys
import unittest
from pathlib import Path


TOOLS = Path(__file__).parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import nested_lod_core as nlc


class NestedLodCoreTests(unittest.TestCase):
    def test_projective_mapping_expands_far_pixel_into_near_master(self):
        sx, sy = nlc.projective_source_xy(
            84, 70, (80.0, 72.0), (80.0, 72.0), 24.0, 36.0)
        self.assertEqual((sx, sy), (86.0, 69.0))

    def test_pattern_classes_merge_identical_tiles(self):
        r = nlc.Raster.blank(16, 8)
        r.set(0, 0, 3)
        r.set(8, 0, 3)
        coord, classes = nlc.tile_classes(r)
        self.assertEqual(coord[(0, 0)], coord[(1, 0)])
        self.assertEqual(len(classes[coord[(0, 0)]]), 2)

    def test_class_wide_swap_changes_every_occurrence(self):
        r = nlc.Raster.blank(16, 8)
        for x0 in (0, 8):
            r.set(x0, 0, 2)
            r.set(x0 + 1, 0, 5)
        _, classes = nlc.tile_classes(r)
        key = next(k for k in classes if any(k))
        nlc.apply_pattern_swap(r, classes[key], 0, 1)
        self.assertEqual((r.at(0, 0), r.at(1, 0)), (5, 2))
        self.assertEqual((r.at(8, 0), r.at(9, 0)), (5, 2))

    def test_refiner_accepts_histogram_preserving_improvement(self):
        r = nlc.Raster.blank(8, 8)
        r.set(0, 0, 1)
        r.set(1, 0, 2)
        _, classes = nlc.tile_classes(r)
        target = r.copy()
        target.set(0, 0, 2)
        target.set(1, 0, 1)

        def objective(candidate):
            return nlc.compare(
                candidate, target, nlc.LossWeights(12.0, 1.0)).weighted

        history = nlc.refine_by_pattern_swaps(
            r, classes, objective, passes=1, candidate_limit=8)
        self.assertGreaterEqual(history[0]["accepted"], 1)
        self.assertEqual(r.pixels, target.pixels)

    def test_global_phase_finds_shift(self):
        master = nlc.Raster.blank(8, 8)
        master.set(2, 3, 5)
        target = nlc.Raster.blank(8, 8)
        target.set(3, 3, 5)
        phase, loss, _ = nlc.fit_global_phase(
            master, target, (0.0, 0.0), (0.0, 0.0), 1.0, 1.0,
            phases=[(0, 0), (1, 0)])
        self.assertEqual(phase, (1, 0))
        self.assertEqual(loss.silhouette_xor, 0)

    def test_distinct_tiles_ignores_empty_by_default(self):
        r = nlc.Raster.blank(16, 8)
        r.set(0, 0, 1)
        self.assertEqual(nlc.distinct_tile_count(r), 1)
        self.assertEqual(nlc.distinct_tile_count(r, include_empty=True), 2)


if __name__ == "__main__":
    unittest.main()
