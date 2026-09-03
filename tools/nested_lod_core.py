#!/usr/bin/env python3
"""Generic primitives for nested, distance-scalable raster experiments.

This module deliberately knows nothing about Doomguy, DHC1, or the Game Gear.
It provides the reusable pieces needed by different offline codec experiments:

* discrete semantic rasters;
* projective target->master coordinate mapping;
* cheap global or pattern-class phase selection;
* silhouette/shade/tile error metrics;
* deterministic class-wide pixel-swap refinement.

The important constraint is that a pattern class is mutated identically at every
occurrence.  Refinement therefore changes a shared visual primitive, not one
screen instance, which keeps the experiment honest about dictionary reuse.
"""

from dataclasses import dataclass


@dataclass
class Raster:
    width: int
    height: int
    pixels: bytearray

    @classmethod
    def blank(cls, width, height):
        return cls(width, height, bytearray(width * height))

    def copy(self):
        return Raster(self.width, self.height, bytearray(self.pixels))

    def at(self, x, y):
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return 0
        return self.pixels[y * self.width + x]

    def set(self, x, y, value):
        self.pixels[y * self.width + x] = value


@dataclass(frozen=True)
class LossWeights:
    silhouette: float = 12.0
    shade: float = 1.0


@dataclass
class Loss:
    silhouette_xor: int
    silhouette_union: int
    shade_abs: int
    overlap: int
    changed_tiles: int
    weighted: float

    @property
    def silhouette_pct(self):
        return (100.0 * self.silhouette_xor / self.silhouette_union
                if self.silhouette_union else 0.0)

    @property
    def shade_mean(self):
        return self.shade_abs / self.overlap if self.overlap else 0.0


def _check_same_shape(a, b):
    if a.width != b.width or a.height != b.height:
        raise ValueError("rasters must have the same dimensions")


def compare(a, b, weights=LossWeights(), tile_size=8):
    """Score two semantic rasters.

    Zero means background.  Non-zero values are ordered shade classes.  A
    silhouette disagreement is intentionally much more expensive than a shade
    disagreement, matching the dense-corpus analyzer's design priority.
    """
    _check_same_shape(a, b)
    sx = su = shade = overlap = 0
    dirty = set()
    for i, (av, bv) in enumerate(zip(a.pixels, b.pixels)):
        aa, bb = bool(av), bool(bv)
        if aa or bb:
            su += 1
        if aa != bb:
            sx += 1
        elif aa:
            overlap += 1
            shade += abs(int(av) - int(bv))
        if av != bv:
            x = i % a.width
            y = i // a.width
            dirty.add((x // tile_size, y // tile_size))
    weighted = weights.silhouette * sx + weights.shade * shade
    return Loss(sx, su, shade, overlap, len(dirty), weighted)


def tile_signature(raster, tx, ty, tile_size=8):
    """Return one padded tile as bytes, suitable as a stable class key."""
    out = bytearray(tile_size * tile_size)
    x0, y0 = tx * tile_size, ty * tile_size
    p = 0
    for y in range(y0, y0 + tile_size):
        for x in range(x0, x0 + tile_size):
            out[p] = raster.at(x, y)
            p += 1
    return bytes(out)


def tile_classes(raster, tile_size=8):
    """Return (coord->class, class->origins) for the raster's tile vocabulary."""
    coord_to_class = {}
    class_to_origins = {}
    cols = (raster.width + tile_size - 1) // tile_size
    rows = (raster.height + tile_size - 1) // tile_size
    for ty in range(rows):
        for tx in range(cols):
            key = tile_signature(raster, tx, ty, tile_size)
            coord_to_class[(tx, ty)] = key
            class_to_origins.setdefault(key, []).append((tx, ty))
    return coord_to_class, class_to_origins


def distinct_tile_count(raster, tile_size=8, include_empty=False):
    _, classes = tile_classes(raster, tile_size)
    if include_empty:
        return len(classes)
    zero = bytes(tile_size * tile_size)
    return len(classes) - (1 if zero in classes else 0)


def projective_source_xy(tx, ty, source_anchor, target_anchor,
                         source_radius, target_radius):
    """Approximate which source pixel projects to target pixel (tx, ty).

    The mapping intentionally models only uniform pinhole shrink about the
    authored pivot.  Any residual against an independently rendered target is
    the thing the distance codec has to explain: depth relief, occlusion, shade
    change, and quantization.
    """
    if source_radius <= 0 or target_radius <= 0:
        raise ValueError("radii must be positive")
    ratio = float(target_radius) / float(source_radius)
    sx = source_anchor[0] + (tx - target_anchor[0]) * ratio
    sy = source_anchor[1] + (ty - target_anchor[1]) * ratio
    return sx, sy


def phase_grid(radius=2):
    """Deterministic integer phase candidates, center first."""
    out = [(0, 0)]
    for d in range(1, radius + 1):
        for dy in range(-d, d + 1):
            for dx in range(-d, d + 1):
                if max(abs(dx), abs(dy)) != d:
                    continue
                out.append((dx, dy))
    return out


def decode_with_global_phase(master, target_shape, source_anchor, target_anchor,
                             source_radius, target_radius, phase=(0, 0)):
    w, h = target_shape
    out = Raster.blank(w, h)
    dx, dy = phase
    for y in range(h):
        for x in range(w):
            sx, sy = projective_source_xy(
                x, y, source_anchor, target_anchor, source_radius, target_radius)
            out.set(x, y, master.at(round(sx + dx), round(sy + dy)))
    return out


def fit_global_phase(master, target, source_anchor, target_anchor,
                     source_radius, target_radius, phases=None,
                     weights=LossWeights()):
    phases = phases or phase_grid()
    best_phase = None
    best_loss = None
    best_raster = None
    for phase in phases:
        pred = decode_with_global_phase(
            master, (target.width, target.height), source_anchor, target_anchor,
            source_radius, target_radius, phase)
        loss = compare(pred, target, weights)
        if best_loss is None or loss.weighted < best_loss.weighted:
            best_phase, best_loss, best_raster = phase, loss, pred
    return best_phase, best_loss, best_raster


def _class_for_source_xy(class_map, sx, sy, tile_size):
    tx = int(round(sx)) // tile_size
    ty = int(round(sy)) // tile_size
    return class_map.get((tx, ty), bytes(tile_size * tile_size))


def fit_pattern_phases(master, target, source_anchor, target_anchor,
                       source_radius, target_radius, class_map, phases=None,
                       weights=LossWeights(), tile_size=8):
    """Fit one integer (dx,dy) per shared master tile class.

    The class identity is supplied by the caller and can remain fixed while the
    master pattern itself is refined.  This models a tiny distance-dependent
    vector table associated with a resident/canonical pattern, rather than
    per-instance side data.
    """
    phases = phases or phase_grid()
    groups = {}
    for y in range(target.height):
        for x in range(target.width):
            sx, sy = projective_source_xy(
                x, y, source_anchor, target_anchor, source_radius, target_radius)
            key = _class_for_source_xy(class_map, sx, sy, tile_size)
            groups.setdefault(key, []).append((x, y, sx, sy, target.at(x, y)))

    table = {}
    for key, items in groups.items():
        best_phase = phases[0]
        best = None
        for dx, dy in phases:
            cost = 0.0
            for _, _, sx, sy, wanted in items:
                got = master.at(round(sx + dx), round(sy + dy))
                if bool(got) != bool(wanted):
                    cost += weights.silhouette
                elif got:
                    cost += weights.shade * abs(int(got) - int(wanted))
            if best is None or cost < best:
                best = cost
                best_phase = (dx, dy)
        table[key] = best_phase

    pred = decode_with_pattern_phases(
        master, target, source_anchor, target_anchor, source_radius,
        target_radius, class_map, table, tile_size)
    return table, compare(pred, target, weights), pred


def decode_with_pattern_phases(master, target_or_shape, source_anchor,
                               target_anchor, source_radius, target_radius,
                               class_map, table, tile_size=8):
    if isinstance(target_or_shape, Raster):
        w, h = target_or_shape.width, target_or_shape.height
    else:
        w, h = target_or_shape
    out = Raster.blank(w, h)
    zero_key = bytes(tile_size * tile_size)
    for y in range(h):
        for x in range(w):
            sx, sy = projective_source_xy(
                x, y, source_anchor, target_anchor, source_radius, target_radius)
            key = _class_for_source_xy(class_map, sx, sy, tile_size)
            dx, dy = table.get(key, table.get(zero_key, (0, 0)))
            out.set(x, y, master.at(round(sx + dx), round(sy + dy)))
    return out


def pattern_swap_candidates(master, class_to_origins, tile_size=8, limit=256):
    """Propose class-wide adjacent swaps, preserving each pattern histogram."""
    out = []
    for key, origins in sorted(class_to_origins.items(),
                               key=lambda kv: kv[0]):
        if not any(key):
            continue
        tx, ty = origins[0]
        x0, y0 = tx * tile_size, ty * tile_size
        for py in range(tile_size):
            for px in range(tile_size):
                a = py * tile_size + px
                av = master.at(x0 + px, y0 + py)
                if px + 1 < tile_size:
                    b = a + 1
                    bv = master.at(x0 + px + 1, y0 + py)
                    if av != bv:
                        out.append((key, a, b))
                if py + 1 < tile_size:
                    b = a + tile_size
                    bv = master.at(x0 + px, y0 + py + 1)
                    if av != bv:
                        out.append((key, a, b))
                if len(out) >= limit:
                    return out
    return out


def apply_pattern_swap(master, origins, a, b, tile_size=8):
    """Apply the same two-pixel swap to every occurrence of one class."""
    ax, ay = a % tile_size, a // tile_size
    bx, by = b % tile_size, b // tile_size
    for tx, ty in origins:
        x0, y0 = tx * tile_size, ty * tile_size
        ia = (y0 + ay) * master.width + (x0 + ax)
        ib = (y0 + by) * master.width + (x0 + bx)
        master.pixels[ia], master.pixels[ib] = (
            master.pixels[ib], master.pixels[ia])


def refine_by_pattern_swaps(master, class_to_origins, objective,
                            passes=1, candidate_limit=256, tile_size=8):
    """Deterministic direct-search refinement over shared pattern classes.

    The optimizer is deliberately callback-driven.  The same engine can be
    reused with a different decoder, perceptual loss, ROM penalty, or hardware
    budget without entangling those choices with the hill climber.
    """
    history = []
    score = float(objective(master))
    for p in range(passes):
        accepted = 0
        candidates = pattern_swap_candidates(
            master, class_to_origins, tile_size, candidate_limit)
        for key, a, b in candidates:
            origins = class_to_origins[key]
            apply_pattern_swap(master, origins, a, b, tile_size)
            trial = float(objective(master))
            if trial + 1e-9 < score:
                score = trial
                accepted += 1
            else:
                apply_pattern_swap(master, origins, a, b, tile_size)
        history.append({"pass": p, "accepted": accepted, "score": score,
                        "candidates": len(candidates)})
        if not accepted:
            break
    return history
