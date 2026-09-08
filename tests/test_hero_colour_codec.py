"""Guards for the material-family encoding.

The properties that matter here are not the measured numbers -- those change
whenever the ramp or the asset changes -- but the invariants the encoding has
to satisfy for the measurement to mean anything at all.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

from analyze_hero_colour_codec import (RAMP_LEN, NON_RAMP_CODES, family_rank,
                                       ramp_positions, Recoded)
from resident_tile_dictionary import SHADE_ORDER, TileWeights, pixel_cost


def test_ramp_positions_come_from_the_shade_order():
    pos = ramp_positions()
    assert len(pos) == RAMP_LEN
    # Dark to light, and none of the three non-ramp plane roles.
    assert list(pos) == [c for c in SHADE_ORDER if c not in NON_RAMP_CODES]
    assert sorted(pos.values()) == list(range(RAMP_LEN))


def test_non_ramp_codes_are_black_ceiling_floor():
    # Corpus codes are semantic+1, so SEM_BLACK/CEILING/FLOOR are 1/2/3.
    # A hero pixel can never be one of these, which is why they are excluded.
    assert NON_RAMP_CODES == (1, 2, 3)


class _FakeSample:
    """Minimal stand-in: Recoded only ever calls at() and family_at()."""
    def __init__(self, shade, family):
        self._shade, self._family = shade, family
        self.x0 = self.y0 = 0
        self.x1 = self.y1 = 0

    def at(self, sx, sy):
        return self._shade

    def family_at(self, sx, sy):
        return self._family


def test_encoding_packs_family_and_shade_without_collision():
    pos = ramp_positions()
    seen = {}
    for code in pos:
        for fam in range(3):
            v = Recoded(_FakeSample(code, fam), pos, 3).at(0, 0)
            assert 1 <= v <= 15, v
            assert (fam, code) not in seen
            assert v not in seen.values(), f"{(fam, code)} collides"
            seen[(fam, code)] = v
    assert len(set(seen.values())) == 3 * RAMP_LEN


def test_empty_pixels_stay_empty_under_the_encoding():
    pos = ramp_positions()
    # 0 means "not the hero" in both planes and must survive re-encoding, or
    # the silhouette itself would move.
    assert Recoded(_FakeSample(0, 0), pos, 3).at(0, 0) == 0
    assert Recoded(_FakeSample(0, 2), pos, 3).at(0, 0) == 0


def test_one_family_is_the_identity():
    pos = ramp_positions()
    for code in list(pos) + [0]:
        assert Recoded(_FakeSample(code, 0), pos, 1).at(0, 0) == code


def test_swapping_material_costs_more_than_a_neighbouring_shade():
    """The quantizer must not be able to buy tone by spending material.

    A rank table that let a family swap look cheap would produce exactly the
    failure this project already hit once with the shade ordering: a codebook
    that optimises a number while looking wrong.
    """
    rank = family_rank(3, [0, 1, 2])
    w = TileWeights(12.0, 1.0, rank)
    same_family_adjacent = pixel_cost(1, 2, w)
    other_family_same_shade = pixel_cost(1, 1 + RAMP_LEN, w)
    assert other_family_same_shade > same_family_adjacent


def test_hue_order_decides_which_material_confusions_are_cheap():
    """Families are ordered by area, so the rank table has to reorder them by
    hue -- otherwise the cost of confusing two materials would depend on how
    much of the model they happened to cover."""
    # Family 2 is the odd hue out; 0 and 1 are neighbours.
    rank = family_rank(3, [0, 1, 2])
    w = TileWeights(12.0, 1.0, rank)
    near = pixel_cost(1, 1 + RAMP_LEN, w)          # family 0 -> 1
    far = pixel_cost(1, 1 + 2 * RAMP_LEN, w)       # family 0 -> 2
    assert far > near
    # Re-order so family 2 sits between 0 and 1 and the prices must follow.
    rank2 = family_rank(3, [0, 2, 1])
    w2 = TileWeights(12.0, 1.0, rank2)
    assert pixel_cost(1, 1 + 2 * RAMP_LEN, w2) < pixel_cost(1, 1 + RAMP_LEN, w2)
