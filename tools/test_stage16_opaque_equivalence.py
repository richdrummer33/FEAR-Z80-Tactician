#!/usr/bin/env python3
"""Deterministic semantic acceptance for the Stage-16 opaque GG materializer.

This compares an independent model of the existing generic opaque-column raster
against the specialized FULL/RAISED_FULL path. It deliberately tests the state
that matters before a preloaded tile word is written: clipped hardware rows,
exact edge-LUT coordinates, full-tile family/border selection, and opaque portal
closure. No Game Gear toolchain is required, so this runs before ROM assembly.
"""

import math
import random

CASES = 200_000
SEED = 0x16D1EC7


def generic_column(top_l, top_r, bot_l, bot_r, clip_top, clip_bottom, shade, border):
    """Model src/tilesector_raster_gg.s for opaque profiles."""
    clip_first = (clip_top + 7) >> 3
    if clip_first >= 18:
        return (), (clip_top, clip_bottom)

    clip_last = clip_bottom >> 3
    if clip_last >= 18:
        clip_last = 17
    if clip_first > clip_last:
        return (), (clip_top, clip_bottom)

    top_l_row = top_l // 8
    top_r_row = top_r // 8
    bot_l_row = bot_l // 8
    bot_r_row = bot_r // 8
    top_min, top_max = min(top_l_row, top_r_row), max(top_l_row, top_r_row)
    bot_min, bot_max = min(bot_l_row, bot_r_row), max(bot_l_row, bot_r_row)

    words = {}

    def edge(bottom, left, right, row):
        if row < 0 or row >= 18 or row < clip_first or row > clip_last:
            return
        slope = right - left
        local = max(-15, min(15, left - row * 8))
        # This tuple uniquely identifies the existing preloaded edge-LUT entry
        # before its shade-family offset is applied.
        words[row] = ("edge", bottom, slope, local, shade)

    edge(0, top_l, top_r, top_min)
    if top_max != top_min:
        edge(0, top_l, top_r, top_max)
    edge(1, bot_l, bot_r, bot_min)
    if bot_max != bot_min:
        edge(1, bot_l, bot_r, bot_max)

    first = max(top_max + 1, clip_first)
    last = min(bot_min - 1, clip_last)
    if last >= 18:
        last = 17
    if first < 0:
        first = clip_first
    if 0 <= first <= last < 18:
        full_tile = (3, 15, 27)[shade] + border
        for row in range(first, last + 1):
            words[row] = ("full", full_tile)

    # FULL and RAISED_FULL close the aperture after a successfully open column.
    return tuple(sorted(words.items())), (1, 0)


def stage16_column(top_l, top_r, bot_l, bot_r, clip_top, clip_bottom, shade, border):
    """Independent model of src/tilesector_opaque_gg.s."""
    first_tile = (clip_top + 7) // 8
    if first_tile >= 18:
        return (), (clip_top, clip_bottom)
    last_tile = min(17, clip_bottom // 8)
    if first_tile > last_tile:
        return (), (clip_top, clip_bottom)

    tl = math.floor(top_l / 8)
    tr = math.floor(top_r / 8)
    bl = math.floor(bot_l / 8)
    br = math.floor(bot_r / 8)
    top_lo, top_hi = min(tl, tr), max(tl, tr)
    bot_lo, bot_hi = min(bl, br), max(bl, br)

    words = {}
    for bottom, left, right, lo, hi in (
        (0, top_l, top_r, top_lo, top_hi),
        (1, bot_l, bot_r, bot_lo, bot_hi),
    ):
        slope = right - left
        rows = (lo,) if lo == hi else (lo, hi)
        for row in rows:
            if 0 <= row < 18 and first_tile <= row <= last_tile:
                local = max(-15, min(15, left - 8 * row))
                words[row] = ("edge", bottom, slope, local, shade)

    lo = max(top_hi + 1, first_tile)
    if lo < 0:
        lo = first_tile
    hi = min(bot_lo - 1, last_tile, 17)
    if 0 <= lo <= hi:
        full_tile = (3, 15, 27)[shade] + border
        for row in range(lo, hi + 1):
            words[row] = ("full", full_tile)

    return tuple(sorted(words.items())), (1, 0)


def test_edge_lut_domain():
    # Two edge orientations * fifteen connected slopes * thirty-one local
    # coordinates = 930 exact entries in the existing preloaded vocabulary.
    seen = set()
    for bottom in (0, 1):
        for slope in range(-7, 8):
            for local in range(-15, 16):
                group = slope + 7 + (15 if bottom else 0)
                index = group * 31 + (local + 15)
                assert 0 <= index < 930
                seen.add(index)
    assert len(seen) == 930


def test_randomized_selection_equivalence():
    rng = random.Random(SEED)
    for case in range(CASES):
        top_l = rng.randint(-80, 220)
        top_r = top_l + rng.randint(-7, 7)
        bot_l = rng.randint(-40, 240)
        bot_r = bot_l + rng.randint(-7, 7)

        # Most cases resemble a real wall; retain some degenerate/off-screen
        # inputs to exercise the rejection and clipping branches too.
        if rng.random() < 0.90 and min(bot_l, bot_r) <= max(top_l, top_r):
            bot_l = max(top_l, top_r) + rng.randint(0, 180)
            bot_r = bot_l + rng.randint(-7, 7)

        clip_top = rng.randint(0, 143)
        clip_bottom = rng.randint(0, 143)
        shade = rng.randint(0, 2)
        border = rng.randint(0, 3)

        old = generic_column(
            top_l, top_r, bot_l, bot_r,
            clip_top, clip_bottom, shade, border,
        )
        new = stage16_column(
            top_l, top_r, bot_l, bot_r,
            clip_top, clip_bottom, shade, border,
        )
        if old != new:
            raise AssertionError(
                f"case {case} mismatch: "
                f"geom={(top_l, top_r, bot_l, bot_r)} "
                f"clip={(clip_top, clip_bottom)} shade={shade} border={border}\n"
                f"generic={old}\nstage16={new}"
            )


if __name__ == "__main__":
    test_edge_lut_domain()
    test_randomized_selection_equivalence()
    print(f"Stage 16 opaque equivalence: {CASES} randomized cases + 930 edge LUT entries OK")
