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
    """Exact port of project_key's clip. Returns None if culled, else (lo,hi).

    Wrap thresholds are +/-512 (the FOV half-width), matching
    src/tilesector_polar_renderer.c:426 verbatim - NOT +/-2048 (a first port
    of this used 2048 and was wrong; caught by re-reading source rather than
    trusting the earlier port, see docs/TODO_DEFERRED.md A7)."""
    length = (a1 - a0) % 4096.0
    if length == 0.0 or length >= 2048.0:
        return None
    st_ = signed_q12(a0 - yaw_q12)
    en = st_ + length
    while en < -512.0:
        st_ += 4096.0
        en += 4096.0
    while st_ > 512.0:
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
    print()
    instruction_counted_decode_estimate(st.mean(tested_l), st.mean(gates_l))


def instruction_counted_decode_estimate(spans_tested_mean, gates_mean):
    """INSTRUCTION-COUNTED, NOT CYCLE-EXACT. Read the warning before citing this.

    Per-span clip cost (worst case, treating every tested span as a fresh SPAN
    with no shared-corner reuse - the true SPAN/SPANC runtime mix is not yet
    measured):

      a0 lookup (16-bit indexed table read)        ~55 T
      a1 lookup (16-bit indexed table read)         55 T
      len = (a1-a0)&4095  (16-bit sub + mask)        34 T
      len==0 test                                    15 T
      len>=2048 test                                  18 T
      st = signed_q12(a0-yawq)                        39 T
      en = st+len                                     11 T
      wrap-loop guard tests (usually 0 iterations)     40 T
      lo=max(st,-512)                                 15 T
      hi=min(en,512)                                  15 T
      hi<=lo reject test                              20 T
      -----------------------------------------------------
      worst case (fresh SPAN)                        317 T
      best case (SPANC, a0 reused)                    262 T

    GATE selector (two 8x8->16 exact-decomposition products + add + sign test,
    same shape as ratio_q8_exact in tilesector_polar_renderer.c)  ~115 T

    This mirrors the ORIGINAL emit estimate's methodology exactly - and that
    estimate was measured 94% LOW once actually simulated cycle-exactly
    (make span-emit-bench). Treat this number the same way: a plausible
    order-of-magnitude, not a trustworthy budget line, until it gets the same
    treatment (see docs/TODO_DEFERRED.md A7).
    """
    worst = 317 * spans_tested_mean + 115 * gates_mean
    best = 262 * spans_tested_mean + 115 * gates_mean
    print("=== DECODE T-STATE ESTIMATE (instruction-counted, NOT cycle-exact) ===")
    print(f"worst case (all fresh SPAN lookups)   {worst:,.0f} T/update")
    print(f"best case  (max SPANC reuse)          {best:,.0f} T/update")
    print()
    print("WARNING: the equivalent hand-count for emit was 11,200 T; the")
    print("cycle-exact measurement (make span-emit-bench) came back at")
    print("21,756 T - 94% higher. This decode estimate has NOT received that")
    print("treatment yet and should be trusted proportionally less.")
    print()
    emit_measured = 21756
    print(f"current best-effort whole-update picture, confidence labeled:")
    print(f"  decode  {best:,.0f}-{worst:,.0f} T   [instruction-counted estimate]")
    print(f"  emit    {emit_measured:,} T   [CYCLE-EXACT, verified against 30 real "
          f"viewports]")
    print(f"  column-solve   NOT YET COSTED (wall_d_q4, inv_for_dq4, "
          f"inv_at_invd, Q6 start/step)")
    print(f"  TOTAL (decode+emit only, excludes column-solve)  "
          f"{best+emit_measured:,.0f}-{worst+emit_measured:,.0f} T")


if __name__ == "__main__":
    main()
