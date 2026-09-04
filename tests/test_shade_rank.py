import importlib.util
import re
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import resident_tile_dictionary as rtd  # noqa: E402

SPEC = importlib.util.spec_from_file_location(
    "analyze_doomguy_dense_corpus", TOOLS / "analyze_doomguy_dense_corpus.py")
reader = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(reader)


def luminance(rgb):
    r, g, b = rgb
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


class ShadeRankTests(unittest.TestCase):
    """The shade ordering must stay tied to what the renderer actually draws.

    The compositor's semantic indices are in historical order, not brightness
    order. Every consumer that measures "how wrong is this shade" has to sort
    them first. Deriving the expected order here from the preview colours means
    a future ramp change breaks this test instead of silently degrading the
    quantizer.
    """

    def test_order_matches_preview_luminance(self):
        # SHADE_RGB[0] is the not-the-hero marker; codes start at 1.
        codes = list(range(1, len(reader.SHADE_RGB)))
        expected = tuple(
            sorted(codes, key=lambda c: luminance(reader.SHADE_RGB[c])))
        self.assertEqual(rtd.SHADE_ORDER, expected)

    def test_order_covers_every_semantic_code(self):
        self.assertEqual(sorted(rtd.SHADE_ORDER),
                         list(range(1, len(reader.SHADE_RGB))))

    def test_compositor_enum_still_has_the_interstitial_stops_appended(self):
        """If the C enum is ever renumbered, this table has to be revisited."""
        src = (TOOLS / "polar_baked_composite.c").read_text()
        wanted = {"SEM_FAR": 3, "SEM_MID": 4, "SEM_NEAR": 5,
                  "SEM_FAR_MID": 6, "SEM_MID_NEAR": 7}
        for name, value in wanted.items():
            m = re.search(rf"\b{name}\s*=\s*(\d+)u", src)
            self.assertIsNotNone(m, f"{name} missing from the compositor enum")
            self.assertEqual(int(m.group(1)), value,
                             f"{name} moved; SHADE_ORDER must be re-derived")

    def test_rank_prices_the_worst_swap_highest(self):
        w = rtd.TileWeights()
        # Second-darkest against brightest: three ramp stops.
        self.assertEqual(rtd.pixel_cost(7, 6, w), 3.0)
        # Genuinely adjacent stops.
        self.assertEqual(rtd.pixel_cost(4, 7, w), 1.0)
        self.assertEqual(rtd.pixel_cost(5, 8, w), 1.0)
        # The bug being guarded against: raw indices invert both of these.
        raw = rtd.TileWeights(12.0, 1.0, None)
        self.assertEqual(rtd.pixel_cost(7, 6, raw), 1.0)
        self.assertEqual(rtd.pixel_cost(4, 7, raw), 3.0)

    def test_coverage_still_dominates_shade(self):
        w = rtd.TileWeights()
        worst_shade = max(
            rtd.pixel_cost(a, b, w)
            for a in rtd.SHADE_ORDER for b in rtd.SHADE_ORDER)
        self.assertLess(worst_shade, rtd.pixel_cost(0, 5, w))


if __name__ == "__main__":
    unittest.main()
