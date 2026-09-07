#!/usr/bin/env python3
"""Measure whether Polar runtime sorting can become baked span-event chains.

This deliberately reuses the CURRENT generated topology recipe/selector field.
For every sampled Q4 camera position in each 4-world-unit Polar cell it replays
visibility, records the ordered visible-key chain, and asks whether changes in
that order can be represented by the same cheap cell-local half-plane events
already used for visibility.

This is an entropy/feasibility probe, not yet a replacement renderer.  The
current tsp_polar_render path remains the correctness oracle.
"""
from __future__ import annotations

import argparse
import math
import pathlib
import re
from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Dict, List, Sequence, Tuple

ROOT = pathlib.Path(__file__).resolve().parents[2]
GEN = ROOT / "src" / "generated"
GRID_W, GRID_H = 48, 24
CELL_Q4 = 64                    # 4 world units * 16 Q4 subpositions.
EMPTY_RECIPE = 255


def arr(text: str, name: str) -> List[int]:
    m = re.search(
        r"static\s+const\s+[^;=]+?\b" + re.escape(name)
        + r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};", text, re.S)
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


def decode_recipes(d: Data):
    out = []
    for recipe in range(len(d.recipe_off)):
        off = d.recipe_off[recipe]
        base_id = d.recipe_stream[off]
        nc = d.recipe_stream[off + 1]
        bo = d.base_off[base_id]
        nb = d.base_stream[bo]
        base = tuple(d.base_stream[bo + 1:bo + 1 + nb])
        p = off + 2
        cond = tuple((d.recipe_stream[p + i * 2], d.recipe_stream[p + i * 2 + 1]) for i in range(nc))
        out.append((base, cond))
    return tuple(out)


def keyparts(d: Data, kid: int) -> Tuple[int, int, int]:
    w = d.keys[kid]
    return w & 31, (w >> 5) & 15, (w >> 9) & 15


def selector_pass(d: Data, sel: int, lx: int, ly: int) -> bool:
    v = d.sel_a[sel] * lx + d.sel_b[sel] * ly + d.sel_c[sel]
    return bool((1 if v >= 0 else 0) ^ d.sel_inv[sel])


def active_keys(d: Data, recipe: int, lx: int, ly: int) -> Tuple[int, ...]:
    base, cond = RECIPES[recipe]
    out = list(base)
    for kid, sel in cond:
        if selector_pass(d, sel, lx, ly):
            out.append(kid)
    return tuple(sorted(set(out)))


def plane_value(d: Data, sid: int, gx: int, gy: int, lx: int, ly: int) -> int:
    """Unshifted wall-plane numerator underlying wall_d_q4.

    inv_for_dq4 is monotonic in absolute wall distance, so relative wall-plane
    order can be tested before the common fixed-point shift/table lookup.
    """
    a = d.anchor[sid]
    px = gx * CELL_Q4 + lx
    py = gy * CELL_Q4 + ly
    return d.nx[sid] * (d.vx[a] * 16 - px) + d.ny[sid] * (d.vy[a] * 16 - py)


def ordered_chain(d: Data, topology: Tuple[int, ...], gx: int, gy: int, lx: int, ly: int) -> Tuple[int, ...]:
    depth: Dict[int, int] = {}
    for kid in topology:
        sid, _, _ = keyparts(d, kid)
        if sid not in depth:
            depth[sid] = abs(plane_value(d, sid, gx, gy, lx, ly))
    # Geometry/shade GG path is near -> far; same-wall pieces use key order.
    return tuple(sorted(topology, key=lambda kid: (depth[keyparts(d, kid)[0]], kid)))


def ranks(order: Tuple[int, ...]):
    return {kid: i for i, kid in enumerate(order)}


def discriminating_pairs(d: Data, orders: Sequence[Tuple[int, ...]]) -> List[Tuple[int, int]]:
    if len(orders) <= 1:
        return []
    rankmaps = [ranks(o) for o in orders]
    common = sorted(set(orders[0]).intersection(*(set(o) for o in orders[1:])))
    out = []
    for i, a in enumerate(common):
        sa = keyparts(d, a)[0]
        for b in common[i + 1:]:
            if sa == keyparts(d, b)[0]:
                continue
            vals = {r[a] < r[b] for r in rankmaps}
            if len(vals) > 1:
                out.append((a, b))
    return out


def greedy_tree_nodes(orders: Sequence[Tuple[int, ...]], weights: Dict[Tuple[int, ...], int],
                      features: Sequence[Tuple[int, int]]) -> int:
    uniq = tuple(dict.fromkeys(orders))
    rankmap = {o: ranks(o) for o in uniq}
    if len(uniq) <= 1:
        return 0

    def rec(cur, available):
        if len(cur) <= 1:
            return 0
        best = None
        best_score = None
        for f in available:
            a, b = f
            lo = tuple(o for o in cur if rankmap[o][a] < rankmap[o][b])
            hi = tuple(o for o in cur if not (rankmap[o][a] < rankmap[o][b]))
            if not lo or not hi:
                continue
            wl = sum(weights[o] for o in lo)
            wh = sum(weights[o] for o in hi)
            score = (max(wl, wh), abs(wl - wh))
            if best_score is None or score < best_score:
                best_score, best = score, (f, lo, hi)
        if best is None:
            return 1_000_000
        f, lo, hi = best
        rest = tuple(x for x in available if x != f)
        return 1 + rec(lo, rest) + rec(hi, rest)

    return rec(uniq, tuple(features))


def local_plane(d: Data, sid: int, gx: int, gy: int) -> Tuple[int, int, int]:
    a = d.anchor[sid]
    A, B = -d.nx[sid], -d.ny[sid]
    C = d.nx[sid] * (d.vx[a] * 16 - gx * CELL_Q4) + d.ny[sid] * (d.vy[a] * 16 - gy * CELL_Q4)
    return A, B, C


def comparator_line(d: Data, gx: int, gy: int, pair: Tuple[int, int], sign_a: int, sign_b: int):
    sa, sb = keyparts(d, pair[0])[0], keyparts(d, pair[1])[0]
    aa, ab, ac = local_plane(d, sa, gx, gy)
    ba, bb, bc = local_plane(d, sb, gx, gy)
    A = sign_a * aa - sign_b * ba
    B = sign_a * ab - sign_b * bb
    C = sign_a * ac - sign_b * bc
    g = math.gcd(abs(A), math.gcd(abs(B), abs(C))) or 1
    A //= g; B //= g; C //= g
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
    occupied_cells = sampled_positions = cell_topologies = multi_order = 0
    max_orders = extra_tree_nodes = unresolved_trees = 0
    event_occurrences = sign_split_events = 0
    topo_global, chains_global, event_lines = set(), set(), set()
    order_hist, chain_len_hist = Counter(), Counter()

    for gy in range(GRID_H):
        for gx in range(GRID_W):
            recipe = d.recipe_grid[gy * GRID_W + gx]
            if recipe == EMPTY_RECIPE:
                continue
            occupied_cells += 1
            groups = defaultdict(Counter)
            positions = defaultdict(list)
            for ly in range(0, CELL_Q4, args.step):
                for lx in range(0, CELL_Q4, args.step):
                    topo = active_keys(d, recipe, lx, ly)
                    if not topo:
                        continue
                    order = ordered_chain(d, topo, gx, gy, lx, ly)
                    groups[topo][order] += 1
                    positions[topo].append((lx, ly))
                    topo_global.add(topo); chains_global.add(order); sampled_positions += 1

            for topo, counts in groups.items():
                cell_topologies += 1
                orders = tuple(counts)
                norder = len(orders)
                order_hist[norder] += 1
                max_orders = max(max_orders, norder)
                if norder <= 1:
                    continue
                multi_order += 1
                features = discriminating_pairs(d, orders)
                nodes = greedy_tree_nodes(orders, counts, features)
                if nodes >= 1_000_000:
                    unresolved_trees += 1
                else:
                    extra_tree_nodes += nodes
                for pair in features:
                    event_occurrences += 1
                    sa, sb = keyparts(d, pair[0])[0], keyparts(d, pair[1])[0]
                    va = {1 if plane_value(d, sa, gx, gy, x, y) >= 0 else -1 for x, y in positions[topo]}
                    vb = {1 if plane_value(d, sb, gx, gy, x, y) >= 0 else -1 for x, y in positions[topo]}
                    if len(va) != 1 or len(vb) != 1:
                        sign_split_events += 1
                        continue
                    event_lines.add(comparator_line(d, gx, gy, pair, next(iter(va)), next(iter(vb))))

    for chain in chains_global:
        chain_len_hist[len(chain)] += 1

    chain_bytes = sum(1 + len(c) for c in chains_global)
    selector_bytes = 4 * len(event_lines)  # A8+B8+C16 straw pack.
    # Upper-bound-ish tree packing: selector-id16 + two child16 per decision,
    # two-byte chain id at each leaf.  Stable single-order regions need one leaf.
    leaves = extra_tree_nodes + cell_topologies
    tree_bytes = 6 * extra_tree_nodes + 2 * leaves

    print("=== POLAR SPAN-EVENT CHAIN POC ===")
    print(f"q4_sample_step={args.step}")
    print(f"occupied_recipe_cells={occupied_cells}")
    print(f"sampled_nonempty_positions={sampled_positions}")
    print(f"unique_topology_signatures_global={len(topo_global)}")
    print(f"cell_topology_regions={cell_topologies}")
    print(f"cell_topologies_with_multiple_depth_orders={multi_order}")
    print(f"multi_order_cell_topology_pct={100.0 * multi_order / max(1, cell_topologies):.3f}")
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
    print("topology_order_count_hist=" + ",".join(f"{k}:{v}" for k, v in sorted(order_hist.items())))
    print("chain_length_hist=" + ",".join(f"{k}:{v}" for k, v in sorted(chain_len_hist.items())))
    print("MODEL: existing visibility selectors stay authoritative; only depth-order changes add events.")
    print("MODEL: one depth event is accepted as a cheap half-plane only while both participating wall-plane signs stay fixed in that active topology region.")
    print("NEXT_GATE: instrument exact current g_run_order across x/y/yaw and compare compiled chain playback before deleting insertion sort.")


DATA = load()
RECIPES = decode_recipes(DATA)

if __name__ == "__main__":
    main()
