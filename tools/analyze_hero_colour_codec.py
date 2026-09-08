#!/usr/bin/env python3
"""What a second material costs the tile vocabulary.

Colour that is one hue is free on this hardware: the palette maps onto the
shade codes the compositor already emits and not a byte of tile data changes.
A second material cannot be free, and this measures the bill rather than
guessing at it.

The reason is the pixel alphabet. Shade-only, a hero pixel is one of six
values (empty plus five ramp stops). With F material families it is one of
1 + 5F -- sixteen at F=3 -- because the palette has to name both which
material and how lit, and a Game Gear sprite pixel is a single palette index.
More distinct values means more distinct 8x8 patterns, and pattern slots are
the binding VRAM constraint (192 of them in this layout).

So both encodings are trained through the same dictionary learner at the same
pattern budgets and scored on the same views, and the difference is the price:

  shade      value = the compositor's semantic code, exactly as shipped
  family     value = 1 + family*5 + position on the brightness ramp

The family encoding's cost function ranks codes by (family, shade) with
families ordered by HUE ANGLE, not by area. That makes the ordinal distance
between two families a stand-in for how different they look, which is the
thing a quantizer should be reluctant to trade away -- confusing the red
petals for the tan robe is a smaller mistake than confusing them for the green
foliage, and a rank table ordered by area would have said the opposite.
"""

import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from analyze_doomguy_dense_corpus import Corpus
from analyze_hierarchical_sprite_lod import learn, replay_stats
from diagnose_lod_shade_metric import reconstruct, oracle_plane
from analyze_sprite_resident_lod import build_groups
from resident_tile_dictionary import TileWeights, SHADE_ORDER

RAMP_LEN = 5
# Corpus codes are semantic+1, so the three non-ramp roles -- SEM_BLACK,
# SEM_CEILING and SEM_FLOOR -- are codes 1, 2 and 3. Everything else is a
# brightness-ramp stop and a hero pixel can only ever be one of those.
NON_RAMP_CODES = (1, 2, 3)


def ramp_positions():
    """Corpus code -> position on the brightness ramp, for the five hero stops.

    Derived from SHADE_ORDER rather than written out, so it cannot drift from
    the compositor's enum -- which is NOT in brightness order (its two
    interstitial stops were appended after the original three) and has already
    cost this project one silent 21x quality regression.
    """
    hero = [c for c in SHADE_ORDER if c not in NON_RAMP_CODES]
    if len(hero) != RAMP_LEN:
        raise SystemExit(f"expected {RAMP_LEN} hero ramp stops, got {hero}")
    return {code: i for i, code in enumerate(hero)}


class Recoded:
    """A corpus sample re-expressed in one of the two encodings.

    Wraps rather than rewrites: build_groups() only ever calls .at() and reads
    the bounding box, so presenting a different .at() is the whole change and
    both encodings go through byte-identical tiling, grouping and learning
    code. Anything else risks measuring a difference between two pipelines
    instead of between two encodings.
    """
    __slots__ = ("_s", "_pos", "_families")

    def __init__(self, sample, pos, families):
        self._s = sample
        self._pos = pos
        self._families = families

    def __getattr__(self, name):
        return getattr(self._s, name)

    def at(self, sx, sy):
        v = self._s.at(sx, sy)
        if not v or self._families <= 1:
            return v
        p = self._pos.get(v)
        if p is None:
            return v
        f = min(self._s.family_at(sx, sy), self._families - 1)
        return 1 + f * RAMP_LEN + p


class RecodedCorpus:
    def __init__(self, corpus, families, pos):
        self._c = corpus
        self.angles = corpus.angles
        self.bands = corpus.bands
        self.screen_w = corpus.screen_w
        self.screen_h = corpus.screen_h
        self.radii = corpus.radii
        self._samples = [Recoded(s, pos, families) for s in corpus.samples]

    def band(self, index):
        return self._samples[index * self.angles:(index + 1) * self.angles]


def family_rank(families, hue_order):
    """rank[code] for the family encoding.

    Shade contributes its ramp position. Family contributes a whole
    FAMILY_STRIDE of rank, so swapping a pixel's material costs the quantizer
    about as much as getting its tone maximally wrong -- which is the right
    relative price: at this pixel scale a wrong hue and a wrong tone are both
    "that is not what is there", and neither should be spent to buy the other.
    """
    FAMILY_STRIDE = RAMP_LEN
    rank = [0] * (1 + RAMP_LEN * families)
    for f in range(families):
        for s in range(RAMP_LEN):
            rank[1 + f * RAMP_LEN + s] = hue_order[f] * FAMILY_STRIDE + s
    return tuple(rank)


def audit(corpus, groups, views, dictionary, score, families, pos):
    """Reconstruction fidelity in units a person argues about.

    Reported separately for silhouette, shade and material, because they fail
    independently and a single blended number would hide which one broke.
    """
    oracle = oracle_plane(groups, views)
    recon = reconstruct(groups, dictionary, score, views)
    lost = gained = filled = shade_wrong = family_wrong = 0
    stops = 0
    for v in views:
        vi = v["view_index"]
        o, r = oracle[vi], recon[vi]
        for key in set(o) | set(r):
            a, b = o.get(key, 0), r.get(key, 0)
            if bool(a) != bool(b):
                if a:
                    lost += 1
                else:
                    gained += 1
                continue
            filled += 1
            if families > 1:
                fa, fb = (a - 1) // RAMP_LEN, (b - 1) // RAMP_LEN
                sa, sb = (a - 1) % RAMP_LEN, (b - 1) % RAMP_LEN
                if fa != fb:
                    family_wrong += 1
            else:
                sa, sb = pos.get(a, 0), pos.get(b, 0)
            if sa != sb:
                shade_wrong += 1
                stops += abs(sa - sb)
    total = filled + lost + gained
    return {
        "silhouette_error_pct": 100.0 * (lost + gained) / max(1, total),
        "filled_pixels": filled,
        "mean_stops_wrong": stops / max(1, filled),
        "shade_wrong_pct": 100.0 * shade_wrong / max(1, filled),
        "family_wrong_pct": (100.0 * family_wrong / max(1, filled)
                             if families > 1 else None),
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("corpus")
    ap.add_argument("--palette", help="importer palette JSON, for hue ordering")
    ap.add_argument("--families", type=int, default=3)
    ap.add_argument("--bands", default="2,3")
    ap.add_argument("--angles", type=int, default=16)
    ap.add_argument("--patterns", default="64,128,192,256")
    ap.add_argument("--lloyd-iterations", type=int, default=4)
    ap.add_argument("--summary-json")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    if not c.has_family and args.families > 1:
        raise SystemExit(f"{args.corpus} carries no family plane (version "
                         f"{c.version}); rebake with a family-split import")
    bands = [int(x) for x in args.bands.split(",") if x != ""]
    step = max(1, c.angles // args.angles)
    angles = list(range(0, c.angles, step))
    pos = ramp_positions()

    # Families are ordered by area in the importer's report; order them by hue
    # here so ordinal rank distance stands in for visual difference.
    if args.palette:
        hues = [e["hueDeg"] for e in
                json.loads(pathlib.Path(args.palette).read_text())["entries"]]
        order = sorted(range(len(hues)), key=lambda f: hues[f])
        hue_order = [0] * len(hues)
        for slot, fam in enumerate(order):
            hue_order[fam] = slot
        hue_order = hue_order[:args.families]
    else:
        hue_order = list(range(args.families))

    rows = []
    for name, families in (("shade", 1), ("family", args.families)):
        rc = RecodedCorpus(c, families, pos)
        groups, views = build_groups(rc, angles, bands)
        weights = (TileWeights(12.0, 1.0) if families == 1 else
                   TileWeights(12.0, 1.0, family_rank(families, hue_order)))
        for count in [int(x) for x in args.patterns.split(",")]:
            result = learn(groups, count, weights, args.lloyd_iterations)
            a = audit(c, groups, views, result["shared"],
                      result["final_score"], families, pos)
            replay = replay_stats(groups, result["final_score"], views)
            rows.append({
                "encoding": name, "families": families, "patterns": count,
                "resident_bytes": count * 32,
                "mean_cost": result["final_score"]["mean_cost"],
                "sprites_mean": replay.get("sprite_refs_mean"),
                **a,
            })
            r = rows[-1]
            fam = ("     -" if r["family_wrong_pct"] is None
                   else f"{r['family_wrong_pct']:5.1f}%")
            print(f"{name:7s} {count:4d} patterns  {r['resident_bytes']:5d}B  "
                  f"silhouette {r['silhouette_error_pct']:5.2f}%  "
                  f"stops wrong {r['mean_stops_wrong']:.3f}  "
                  f"material wrong {fam}")

    if args.summary_json:
        pathlib.Path(args.summary_json).write_text(
            json.dumps({"angles": angles, "bands": bands, "rows": rows},
                       indent=2))
    print("HERO_COLOUR_CODEC_PASS")


if __name__ == "__main__":
    main()
