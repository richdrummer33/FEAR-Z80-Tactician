#!/usr/bin/env python3
"""Measure whether Polar runtime sorting can become baked span-event chains.

This intentionally reuses the CURRENT generated topology recipe/selector field.
It does not invent another translation grid.  For every Q4 camera position in
one 4-world-unit Polar cell it:

  1. replays the existing base + conditional visibility recipe;
  2. treats each active key as a directed visible wall span;
  3. orders those spans by the same underlying wall-plane distance quantity
     that feeds the renderer's inverse-depth path;
  4. measures how many different orders occur for one topology signature;
  5. derives the extra pairwise depth-order events needed to distinguish them;
  6. checks whether each such comparison reduces to one cell-local linear
     half-plane selector (the desired cheap Z80 form).

The result is a feasibility/entropy probe, NOT yet a replacement renderer.
The current GG renderer remains the oracle.  A later stage must compare a
compiled chain player against tsp_polar_render before deleting runtime sort.
"""
from __future__ import annotations

import argparse
import math
import pathlib
import re
from collections import Counter, defaultdict
from dataclasses import dataclass
from functools import lru_cache
from typing import Dict, Iterable, List, Sequence, Tuple

ROOT = pathlib.Path(__file__).resolve().parents[2]
GEN = ROOT / "src" / "generated"
GRID_W, GRID_H = 48, 24
CELL_Q4 = 64                    # 4 world units, with four fractional bits.
EMPTY_RECIPE = 255


def arr(text: str, name: str) -> List[int]:
    m = re.search(
        r"static\s+const\s+[^;=]+?\b" + re.escape(name)
        + r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};",
        text,
        re.S,
    )
    if not m:
        raise SystemExit("missing generated array " + name)
    return [int(x, 0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+", m.group(1))]


@dataclass(frozen=True)
class Data:
    keys: Sequence[int]
    sel_a: Sequence[int]
    sel_b: Sequence[int]
    sel_c: Sequence[int]
    sel_inv: Sequence[int]
    base_off: Sequence[int]
    base_stream: Sequence[int]
    recipe_off: Sequence[int]
    recipe_stream: Sequence[int]
    recipe_grid: Sequence[int]
    vx: Sequence[int]
    vy: Sequence[int]
    nx: Sequence[int]
    ny: Sequence[int]
    anchor: Sequence[int]


def load() -> Data:
    text = "\n".join(p.read_text() for p in sorted(GEN.glob("tilesector_polar_data_part*.inc")))
    return Data(
        arr(text, "k_tspf_keys"),
        arr(text, "k_tspf_sel_a"), arr(text, "k_tspf_sel_b"),
        arr(text, "k_tspf_sel_c"), arr(text, "k_tspf_sel_inv"),
        arr(text, "k_tspf_base_off"), arr(text, "k_tspf_base_stream"),
        arr(text, "k_tspf_recipe_off"), arr(text, "k_tspf_recipe_stream"),
        arr(text, "k_tspf_recipe_grid"),
        arr(text, "k_tspf_vx"), arr(text, "k_tspf_vy"),
        arr(text, "k_tspf_nx_q5"), arr(text, "k_tspf_ny_q5"),
        arr(text, "k_tspf_seg_anchor"),
    )


def keyparts(d: Data, kid: int) -> Tuple[int, int, int]:
    w = d.keys[kid]
    return w & 31, (w >> 5) & 15, (w >> 9) & 15


def selector_pass(d: Data, sel: int, lx: int, ly: int) -> bool:
    v = d.sel_a[sel] * lx + d.sel_b[sel] * ly + d.sel_c[sel]
    return bool((1 if v >= 0 else 0) ^ d.sel_inv[sel])


@lru_cache(maxsize=None)
def recipe_parts_cached(recipe: int, base_off_t: Tuple[int, ...], base_t: Tuple[int, ...],
                        recipe_off_t: Tuple[int, ...], recipe_t: Tuple[int, ...]):
    off = recipe_off_t[recipe]
    base_id = recipe_t[off]
    nc = recipe_t[off + 1]
    bo = base_off_t[base_id]
    nbase = base_t[bo]
    base = tuple(base_t[bo + 1:bo + 1 + nbase])
    p = off + 2
    cond = tuple((recipe_t[p + i * 2], recipe_t[p + i * 2 + 1]) for i in range(nc))
    return base, cond


def recipe_parts(d: Data, recipe: int):
    return recipe_parts_cached(
        recipe, tuple(d.base_off), tuple(d.base_stream),
        tuple(d.recipe_off), tuple(d.recipe_stream),
    )


def active_keys(d: Data, recipe: int, lx: int, ly: int) -> Tuple[int, ...]:
    base, cond = recipe_parts(d, recipe)
    out = list(base)
    for kid, sel in cond:
        if selector_pass(d, sel, lx, ly):
            out.append(kid)
    # Key identity includes directed endpoints.  Preserve identity, but make the
    # topology signature independent of recipe stream order.
    return tuple(sorted(set(out)))


def plane_value(d: Data, sid: int, gx: int, gy: int, lx: int, ly: int) -> int:
    """Unshifted Q5 wall-plane distance numerator used for ordering.

    The renderer's wall_d_q4 is this signed normal dot product followed by a
    common fixed-point scale. inv_for_dq4 is monotonic in |distance|, so the
    comparison can be done before the shift/table lookup.
    """
    a = d.anchor[sid]
    px = gx * CELL_Q4 + lx
    py = gy * CELL_Q4 + ly
    dx = d.vx[a] * 16 - px
    dy = d.vy[a] * 16 - py
    return d.nx[sid] * dx + d.ny[sid] * dy


def ordered_chain(d: Data, topology: Tuple[int, ...], gx: int, gy: int, lx: int, ly: int) -> Tuple[int, ...]:
    depth: Dict[int, int] = {}
    for kid in topology:
        sid, _, _ = keyparts(d, kid)
        if sid not in depth:
            depth[sid] = abs(plane_value(d, sid, gx, gy, lx, ly))
    # Geometry/shade GG path is near -> far.  Same-wall pieces retain key order.
    return tuple(sorted(topology, key=lambda kid: (depth[keyparts(d, kid)[0]], kid)))


def pair_relation(order: Tuple[int, ...], a: int, b: int) -> bool:
    rank = {kid: i for i, kid in enumerate(order)}
    return rank[a] < rank[b]


def discriminating_pairs(orders: Sequence[Tuple[int, ...]]) -> List[Tuple[int, int]]:
    if len(orders) <= 1:
        return []
    keys = sorted(set().union(*map(set, orders)))
    common = [k for k in keys if all(k in o for o in orders)]
    out = []
    for i, a in enumerate(common):
        for b in common[i + 1:]:
            vals = {pair_relation(o, a, b) for o in orders}
            if len(vals) > 1:
                # Same segment plane never needs a depth event; deterministic key
                # order already handles multiple directed pieces of one surface.
                if keyparts(DATA, a)[0] != keyparts(DATA, b)[0]:
                    out.append((a, b))
    return out


def greedy_tree_nodes(orders: Sequence[Tuple[int, ...]], weights: Dict[Tuple[int, ...], int],
                      features: Sequence[Tuple[int, int]]) -> int:
    """Decision-node count needed to select an observed order from pair tests."""
    uniq = tuple(dict.fromkeys(orders))
    if len(uniq) <= 1:
        return 0

    def rec(cur: Tuple[Tuple[int, ...], ...], available: Tuple[Tuple[int, int], ...]) -> int:
        if len(cur) <= 1:
            return 0
        best = None
        best_score = None
        for f in available:
            a, b = f
            lo = tuple(o for o in cur if pair_relation(o, a, b))
            hi = tuple(o for o in cur if not pair_relation(o, a, b))
            if not lo or not hi:
                continue
            wl = sum(weights[o] for o in lo)
            wh = sum(weights[o] for o in hi)
            score = (max(wl, wh), abs(wl - wh))
            if best_score is None or score < best_score:
                best_score, best = score, (f, lo, hi)
        if best is None:
            # Distinct total orders should always differ on a pair.  If we land
            # here, the feature model is incomplete; make that visible in output.
            return 1_000_000
        f, lo, hi = best
        rest = tuple(x for x in available if x != f)
        return 1 + rec(lo, rest) + rec(hi, rest)

    return rec(uniq, tuple(features))


def local_plane(d: Data, sid: int, gx: int, gy: int) -> Tuple[int, int, int]:
    a = d.anchor[sid]
    # D(lx,ly) = A*lx + B*ly + C.
    A = -d.nx[sid]
    B = -d.ny[sid]
    C = d.nx[sid] * (d.vx[a] * 16 - gx * CELL_Q4) + d.ny[sid] * (d.vy[a] * 16 - gy * CELL_Q4)
    return A, B, C


def sign_set_for_pair(d: Data, topology: Tuple[int, ...], gx: int, gy: int,
                      pair: Tuple[int, int], samples: Sequence[Tuple[int, int]]) -> Tuple[set, set]:
    sa, sb = keyparts(d, pair[0])[0], keyparts(d, pair[1])[0]
    va, vb = set(), set()
    for lx, ly in samples:
        va.add(1 if plane_value(d, sa, gx, gy, lx, ly) >= 0 else -1)
        vb.add(1 if plane_value(d, sb, gx, gy, lx, ly) >= 0 else -1)
    return va, vb


def comparator_line(d: Data, gx: int, gy: int, pair: Tuple[int, int], sign_a: int, sign_b: int):
    sa, sb = keyparts(d, pair[0])[0], keyparts(d, pair[1])[0]
    aa, ab, ac = local_plane(d, sa, gx, gy)
    ba, bb, bc = local_plane(d, sb, gx, gy)
    # |Da| - |Db| = sign_a*Da - sign_b*Db.
    A = sign_a * aa - sign_b * ba
    B = sign_a * ab - sign_b * bb
    C = sign_a * ac - sign_b * bc
    g = math.gcd(abs(A), math.gcd(abs(B), abs(C))) or 1
    A //= g; B //= g; C //= g
    # Canonical geometric line orientation; branch polarity can be one bit.
    if A < 0 or (A == 0 and B < 0) or (A == 0 and B == 0 and C < 0):
        A, B, C = -A, -B, -C
    return A, B, C


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--step", type=int, default=1, help="Q4 local sample stride; 1 exhausts every sub-position")
    args = ap.parse_args()
    if args.step < 1 or CELL_Q4 % args.step:
        raise SystemExit("--step must be a positive divisor of 64")

    d = DATA
    occupied_cells = 0
    sampled_positions = 0
    topo_global = set()
    chains_global = set()
    multi_order_topologies = 0
    cell_topologies = 0
    max_orders = 0
    extra_tree_nodes = 0
    unresolved_trees = 0
    event_lines = set()
    sign_split_events = 0
    event_occurrences = 0
    topology_order_hist = Counter()
    chain_len_hist = Counter()

    for gy in range(GRID_H):
        for gx in range(GRID_W):
            recipe = d.recipe_grid[gy * GRID_W + gx]
            if recipe == EMPTY_RECIPE:
                continue
            occupied_cells += 1
            groups: Dict[Tuple[int, ...], Counter] = defaultdict(Counter)
            positions: Dict[Tuple[int, ...], List[Tuple[int, int]]] = defaultdict(list)
            for ly in range(0, CELL_Q4, args.step):
                for lx in range(0, CELL_Q4, args.step):
                    topo = active_keys(d, recipe, lx, ly)
                    if not topo:
                        continue
                    order = ordered_chain(d, topo, gx, gy, lx, ly)
                    groups[topo][order] += 1
                    positions[topo].append((lx, ly))
                    topo_global.add(topo)
                    chains_global.add(order)
                    sampled_positions += 1

            for topo, counts in groups.items():
                cell_topologies += 1
                orders = tuple(counts.keys())
                norder = len(orders)
                topology_order_hist[norder] += 1
                max_orders = max(max_orders, norder)
                if norder > 1:
                    multi_order_topologies += 1
                    features = discriminating_pairs(orders)
                    nodes = greedy_tree_nodes(orders, counts, features)
                    if nodes >= 1_000_000:
                        unresolved_trees += 1
                    else:
                        extra_tree_nodes += nodes
                    for pair in features:
                        event_occurrences += 1
                        va, vb = sign_set_for_pair(d, topo, gx, gy, pair, positions[topo])
                        if len(va) != 1 or len(vb) != 1:
                            sign_split_events += 1
                            continue
                        line = comparator_line(d, gx, gy, pair, next(iter(va)), next(iter(vb)))
                        event_lines.add(line)

    for c in chains_global:
        chain_len_hist[len(c)] += 1

    chain_bytes = sum(1 + len(c) for c in chains_global)  # count byte + key bytes
    # Straw pack: global deduped line = A8+B8+C16 = 4 B. Decision node uses
    # selector-id16 + child16 + child16 = 6 B. Leaf uses chain-id16 = 2 B.
    # A full binary tree with N nodes has N+1 leaves.
    selector_bytes = 4 * len(event_lines)
    tree_bytes = 6 * extra_tree_nodes + 2 * (extra_tree_nodes + max(0, cell_topologies - multi_order_topologies))

    print("=== POLAR SPAN-EVENT CHAIN POC ===")
    print(f"q4_sample_step={args.step}")
    print(f"occupied_recipe_cells={occupied_cells}")
    print(f"sampled_nonempty_positions={sampled_positions}")
    print(f"unique_topology_signatures_global={len(topo_global)}")
    print(f"cell_topology_regions={cell_topologies}")
    print(f"cell_topologies_with_multiple_depth_orders={multi_order_topologies}")
    pct = 100.0 * multi_order_topologies / max(1, cell_topologies)
    print(f"multi_order_cell_topology_pct={pct:.3f}")
    print(f"max_orders_for_one_cell_topology={max_orders}")
    print(f"unique_ordered_span_chains={len(chains_global)}")
    print(f"ordered_chain_dictionary_bytes={chain_bytes}")
    print(f"extra_depth_decision_nodes={extra_tree_nodes}")
    print(f"unresolved_decision_trees={unresolved_trees}")
    print(f"pair_event_occurrences={event_occurrences}")
    print(f"pair_events_crossing_a_wall_plane_sign={sign_split_events}")
    print(f"dedup_linear_depth_event_lines={len(event_lines)}")
    print(f"depth_event_line_dictionary_bytes={selector_bytes}")
    print(f"straw_order_tree_bytes={tree_bytes}")
    print(f"straw_chain_plus_order_bytes={chain_bytes + selector_bytes + tree_bytes}")
    print("topology_order_count_hist=" + ",".join(f"{k}:{v}" for k, v in sorted(topology_order_hist.items())))
    print("chain_length_hist=" + ",".join(f"{k}:{v}" for k, v in sorted(chain_len_hist.items())))
    print("MODEL: visibility uses existing baked selectors; only depth-order changes add events.")
    print("MODEL: an order event is accepted as one cheap half-plane only when both wall-plane signs stay fixed over that active topology region.")
    print("NEXT_GATE: compare compiled chain playback against the current renderer/oracle before removing insertion sort.")


DATA = load()

if __name__ == "__main__":
    main()
