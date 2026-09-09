#!/usr/bin/env python3
"""Measure the span interpreter's DECODE-stage workload on real camera poses.

`span_block_bake.py` proves the per-cell instruction stream is correct at the
level of "which key set is drawn" (16,776 sub-cell positions, base + GATE-
passing conditionals). It does not say how many of those instructions survive
the yaw-relative visibility clip at a real camera pose, which is the number
the decode stage actually walks and the number that drives the "span decode +
setup" line of the interpreter budget.

This fills that gap using the same real-pose sampling as
`span_workload_probe.c` (every walkable 4-world-unit cell centre, every Nth
yaw) and the exact clip arithmetic `project_key()` uses
(`src/tilesector_polar_renderer.c:416`):

    len  = (a1 - a0) mod 4096; reject if 0 or >= 2048
    st   = signed_q12(a0 - yaw<<4); en = st + len
    wrap st,en into [-2048, 2048) by +/-4096
    lo = max(st,-512); hi = min(en,512); reject if hi <= lo

Bearings are the exact analytic value (matching the host oracle), not the
baked local-projection approximation - this measures decode WORKLOAD
(instructions walked, spans surviving), not runtime T-states, so exactness
here matters more than matching the field's small baked error.

CROSS-VALIDATION
-----------------
`make polar-test` already reports avg_runs=4.06 (mean draw_run() calls per
update in the existing C renderer, over its own 360-frame scripted demo). This
tool's "visible spans/update" should be structurally comparable - both count
spans that survive yaw/FOV clipping - though the pose samplings differ (this
tool samples cell centres x every-Nth-yaw across the whole walkable grid;
polar-test replays one scripted 360-frame camera path). Reported side by side
as a plausibility check, not asserted equal.

    make span-decode-workload
"""
from __future__ import annotations
import math
import pathlib
import statistics as st
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from span_block_bake import (  # noqa: E402
    load, build_block, keyparts, OP_SPAN, OP_SPANC, OP_GATE, OP_END,
    selector_pass,
)
from local_projection_field_poc import CELL_Q4, GRID_W, GRID_H  # noqa: E402

QTURN = 4096.0
TAU = math.tau


def exact_bearing(d, v, xq, yq):
    dx = d.vx[v] * 16.0 - xq
    dy = d.vy[v] * 16.0 - yq
    return (math.atan2(dy, dx) * QTURN / TAU) % QTURN


def signed_q12(a):
    a = a % 4096.0
    return a - 4096.0 if a >= 2048.0 else a


def visible_window(a0, a1, yaw_q12):
    """Exact port of project_key's clip. Returns None if culled, else (lo,hi)."""
    length = (a1 - a0) % 4096.0
    if length == 0.0 or length >= 2048.0:
        return None
    st_ = signed_q12(a0 - yaw_q12)
    en = st_ + length
    while en < -2048.0:
        st_ += 4096.0
        en += 4096.0
    while st_ > 2048.0:
        st_ -= 4096.0
        en -= 4096.0
    lo = max(st_, -512.0)
    hi = min(en, 512.0)
    if hi <= lo:
        return None
    return lo, hi


def walk_block(d, ops, px, py, yaw_q12, lx, ly):
    """Decode one block at a real pose. Returns per-update counters."""
    ops_tested = spans_tested = spans_visible = gates_tested = 0
    prev_bearing = None
    skip = False
    for op in ops:
        kind = op[0]
        if kind == OP_END:
            break
        if kind == OP_GATE:
            gates_tested += 1
            skip = not selector_pass(d, op[1], lx, ly)
            continue
        ops_tested += 1
        if skip:
            skip = False
            continue
        spans_tested += 1
        if kind == OP_SPAN:
            _, k, v0, v1, sid = op
            a0 = exact_bearing(d, v0, px, py)
        else:  # OP_SPANC: v0 implicit = previous span's v1
            _, k, v1, sid = op
            a0 = prev_bearing
        a1 = exact_bearing(d, v1, px, py)
        prev_bearing = a1
        if visible_window(a0, a1, yaw_q12) is not None:
            spans_visible += 1
    return ops_tested, spans_tested, spans_visible, gates_tested


def main():
    d = load()
    yaw_step = int(sys.argv[1]) if len(sys.argv) > 1 else 8

    ops_l, tested_l, visible_l, gates_l = [], [], [], []
    n = 0
    for gy in range(GRID_H):
        for gx in range(GRID_W):
            ops, _ = build_block(d, gx, gy)
            if ops is None:
                continue
            px = gx * CELL_Q4 + CELL_Q4 / 2
            py = gy * CELL_Q4 + CELL_Q4 / 2
            for yaw in range(0, 256, yaw_step):
                yaw_q12 = yaw * 16.0
                ot, st_, sv, gt = walk_block(d, ops, px, py, yaw_q12,
                                              CELL_Q4 / 2, CELL_Q4 / 2)
                ops_l.append(ot); tested_l.append(st_)
                visible_l.append(sv); gates_l.append(gt)
                n += 1

    print("=== SPAN INTERPRETER: DECODE-STAGE WORKLOAD (real poses) ===")
    print(f"poses sampled           {n}  (yaw_step={yaw_step})")
    print(f"instructions walked/upd mean={st.mean(ops_l):.2f} "
          f"median={st.median(ops_l)} max={max(ops_l)}")
    print(f"spans tested/update     mean={st.mean(tested_l):.2f} "
          f"median={st.median(tested_l)} max={max(tested_l)}")
    print(f"spans VISIBLE/update    mean={st.mean(visible_l):.2f} "
          f"median={st.median(visible_l)} max={max(visible_l)}")
    print(f"gates tested/update     mean={st.mean(gates_l):.2f} "
          f"max={max(gates_l)}")
    rejected = st.mean(tested_l) - st.mean(visible_l)
    print(f"spans REJECTED by clip  mean={rejected:.2f} "
          f"({rejected/st.mean(tested_l):.1%} of tested)")
    print()
    print("cross-check against make polar-test (avg_runs=4.06, different")
    print("pose sampling - one scripted 360-frame path vs this tool's full")
    print(f"grid x every-{yaw_step}th-yaw sweep):")
    print(f"  this tool's spans VISIBLE/update mean = {st.mean(visible_l):.2f}")
    print()
    print("this replaces the ISA-spec's ~450 T/span x ~10 spans decode")
    print(f"estimate's SPAN COUNT with a measured {st.mean(visible_l):.2f} "
          f"visible + {rejected:.2f} rejected-but-tested per update")


if __name__ == "__main__":
    main()
