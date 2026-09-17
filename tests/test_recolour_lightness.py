"""Guards for the lightness checks the colour build is gated on.

The original gate was symmetric: no (material, shade) pair may sit more than
one ramp step from where the greyscale design put it, in either direction. It
was the right gate while colour was promising to be "the greyscale image with
hue added", and it is the wrong gate now that the hero band deliberately
reaches further down -- because it cannot distinguish the two things it exists
to catch. A stop that moved UP, or that crossed its neighbour, scrambles the
tonal ordering the tile vocabulary was trained against. A stop that moved DOWN,
monotonically, with its order intact, is the extra contrast that was asked for,
and bounding it bounds the feature.

So these test the replacement: a signed, per-role split, plus the ordering
invariant that was what the symmetric bound was really protecting.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

from recolour_semantic_frames import (lightness_signed, lightness_delta,
                                      oklab_L, ramp_order_intact)

# Five stops at the compositor's brightness-order semantic values, dark to
# light, as the design JSON carries them.
BRIGHTNESS_ORDER = [3, 6, 4, 7, 5]
GREY = {3: (60, 60, 60), 6: (100, 100, 100), 4: (140, 140, 140),
        7: (180, 180, 180), 5: (225, 225, 225)}


def design(families, order=None):
    return {
        "brightnessOrder": order or BRIGHTNESS_ORDER,
        "polychrome": len(families) > 1,
        "spriteIndex": [[1 + f * 5 + i for i in range(5)]
                        for f in range(len(families))],
        "heroFamilies": [
            {"family": f, "stops": [{"srgb8": list(c)} for c in stops]}
            for f, stops in enumerate(families)
        ],
    }


def tables(families):
    """Flat sprite table indexed the way hero_index() addresses it."""
    sprite = [(0, 0, 0)] * 16
    for f, stops in enumerate(families):
        for i, c in enumerate(stops):
            sprite[1 + f * 5 + i] = tuple(c)
    return sprite


RAMP = [(60, 60, 60), (100, 100, 100), (140, 140, 140),
        (180, 180, 180), (225, 225, 225)]
DARKER = [(40, 40, 40), (80, 80, 80), (120, 120, 120),
          (160, 160, 160), (205, 205, 205)]
BRIGHTER = [(80, 80, 80), (120, 120, 120), (160, 160, 160),
            (200, 200, 200), (245, 245, 245)]


def _counts(sems, role=1, fam=0):
    return {(role, fam, v): 1 for v in sems}


def test_an_unchanged_palette_moves_nothing_in_either_direction():
    d = design([RAMP])
    bg = [(0, 0, 0)] * 16
    for v, c in GREY.items():
        bg[v] = c
    sp = tables([RAMP])
    sg = lightness_signed(bg, [GREY.get(i, (0, 0, 0)) for i in range(16)],
                          bg, sp, _counts(BRIGHTNESS_ORDER), d)
    up, _, down, _ = sg["hero"]
    assert up == 0.0 and down == 0.0


def test_a_darkened_ramp_registers_as_down_and_not_as_up():
    """The whole point: the gate must see 'darker' as a different thing from
    'scrambled', and today's wider hero band is exactly the darker case."""
    d = design([DARKER])
    bg = [(0, 0, 0)] * 16
    ref_sprite = [GREY.get(i, (0, 0, 0)) for i in range(16)]
    sg = lightness_signed(bg, ref_sprite, bg, tables([DARKER]),
                          _counts(BRIGHTNESS_ORDER), d)
    up, _, down, _ = sg["hero"]
    assert up == 0.0, f"a uniformly darker ramp reported an upward move of {up}"
    assert down > 0.02, down


def test_a_brightened_ramp_registers_as_up():
    d = design([BRIGHTER])
    bg = [(0, 0, 0)] * 16
    ref_sprite = [GREY.get(i, (0, 0, 0)) for i in range(16)]
    sg = lightness_signed(bg, ref_sprite, bg, tables([BRIGHTER]),
                          _counts(BRIGHTNESS_ORDER), d)
    up, _, down, _ = sg["hero"]
    assert up > 0.02, up
    assert down == 0.0


def test_the_symmetric_bound_cannot_tell_the_two_apart():
    """Why the gate had to change, stated as a test rather than as a comment:
    the old number is the same for the safe case and the unsafe one."""
    bg = [(0, 0, 0)] * 16
    for v, c in GREY.items():
        bg[v] = c
    ref_sprite = [GREY.get(i, (0, 0, 0)) for i in range(16)]
    counts = _counts(BRIGHTNESS_ORDER)
    dark = lightness_delta(bg, ref_sprite, bg, tables([DARKER]), counts,
                           design([DARKER]))[1][0]
    bright = lightness_delta(bg, ref_sprite, bg, tables([BRIGHTER]), counts,
                             design([BRIGHTER]))[1][0]
    assert abs(dark - bright) < 0.02, (dark, bright)


def test_the_room_role_is_reported_separately_from_the_hero():
    """The room keeps the symmetric bound, so the split has to actually
    separate them rather than reporting the worst of the two twice."""
    d = design([RAMP])
    bg = [(0, 0, 0)] * 16
    moved_bg = [(0, 0, 0)] * 16
    for v, c in GREY.items():
        bg[v] = c
        moved_bg[v] = tuple(min(255, ch + 40) for ch in c)
    ref_sprite = [GREY.get(i, (0, 0, 0)) for i in range(16)]
    counts = {(0, 0, v): 1 for v in BRIGHTNESS_ORDER}
    counts.update(_counts(BRIGHTNESS_ORDER))
    sg = lightness_signed(bg, ref_sprite, moved_bg, tables([RAMP]), counts, d)
    assert sg["room"][0] > 0.05, sg["room"]
    assert sg["hero"][0] == 0.0, sg["hero"]


def test_an_ascending_ramp_has_no_order_violations():
    assert ramp_order_intact(design([RAMP])) == []


def test_two_stops_swapped_is_an_order_violation():
    swapped = list(RAMP)
    swapped[1], swapped[2] = swapped[2], swapped[1]
    bad = ramp_order_intact(design([swapped]))
    assert len(bad) == 1
    assert bad[0][0] == 0


def test_two_stops_that_collide_are_an_order_violation():
    """Equal is as bad as backwards: two shade values that draw the same
    colour cost a ramp stop and the figure loses a level of form."""
    flat = list(RAMP)
    flat[3] = flat[2]
    assert len(ramp_order_intact(design([flat]))) == 1


def test_every_family_is_checked_for_ordering_not_just_the_first():
    """A per-family albedo offset makes this a real risk: family 0 can be
    perfect while family 1 has been shifted into a clamp that flattened it."""
    flat = list(RAMP)
    flat[1] = flat[0]
    bad = ramp_order_intact(design([RAMP, flat]))
    assert len(bad) == 1 and bad[0][0] == 1


def test_a_darkened_family_stays_ordered():
    """Darkening a whole family by a constant is the supported operation and
    it must not, by itself, produce an ordering violation."""
    assert ramp_order_intact(design([RAMP, DARKER])) == []


def test_a_darkened_family_is_below_the_base_family_at_every_stop():
    """What the offset is for. Checked here on the palette rather than on a
    render, because a render can hide it behind the shading."""
    for base, dark in zip(RAMP, DARKER):
        assert oklab_L(dark) < oklab_L(base), (base, dark)
