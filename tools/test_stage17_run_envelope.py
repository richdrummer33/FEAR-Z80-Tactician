#!/usr/bin/env python3
"""Exact equivalence stress test for Stage-17 run-envelope visibility.

The fast path reasons about whole horizontal runs. It only falls back to
column-wise comparison when two reciprocal-depth lines are within one Q6
quantization unit or cross. The output must exactly match the Stage-11/12
byte-reciprocal nearest-wall rule, including tie-wins-existing behavior.
"""
from dataclasses import dataclass
import random

NO_WALL = 0xFF
COLS = 20

@dataclass
class Run:
    end: int
    seg: int
    inv_q6: int
    step_q6: int

def mid_q6(inv_q6, step_q6):
    return inv_q6 + (step_q6 >> 1)

def inv8(inv_q6, step_q6):
    return mid_q6(inv_q6, step_q6) >> 6

def advance(inv_q6, step_q6, n):
    return inv_q6 + step_q6 * n

def append_run(out, end, seg, inv_q6, step_q6):
    if out and out[-1].seg == seg:
        out[-1].end = end
    else:
        out.append(Run(end, seg, inv_q6, step_q6))

def merge_candidate(runs, view0, view1, seg, c0, c1, cand_inv, cand_step):
    out = []
    run_start = view0
    for r in runs:
        run_end = r.end
        if run_end < c0 or run_start > c1:
            append_run(out, run_end, r.seg, r.inv_q6, r.step_q6)
            run_start = run_end + 1
            continue

        ov0 = max(run_start, c0)
        ov1 = min(run_end, c1)
        if run_start < ov0:
            append_run(out, ov0 - 1, r.seg, r.inv_q6, r.step_q6)

        old_inv = advance(r.inv_q6, r.step_q6, ov0 - run_start)
        new_inv = advance(cand_inv, cand_step, ov0 - c0)

        if r.seg == NO_WALL:
            append_run(out, ov1, seg, new_inv, cand_step)
        else:
            old_end = advance(old_inv, r.step_q6, ov1 - ov0)
            new_end = advance(new_inv, cand_step, ov1 - ov0)
            d0 = mid_q6(new_inv, cand_step) - mid_q6(old_inv, r.step_q6)
            d1 = mid_q6(new_end, cand_step) - mid_q6(old_end, r.step_q6)

            # >= one whole Q6 reciprocal unit means quantized new depth is
            # strictly nearer at every column. <= 0 means existing can never
            # lose because equal quantized depth keeps the existing segment.
            if d0 >= 64 and d1 >= 64:
                append_run(out, ov1, seg, new_inv, cand_step)
            elif d0 <= 0 and d1 <= 0:
                append_run(out, ov1, r.seg, old_inv, r.step_q6)
            else:
                oi, ni = old_inv, new_inv
                for col in range(ov0, ov1 + 1):
                    if inv8(ni, cand_step) > inv8(oi, r.step_q6):
                        append_run(out, col, seg, ni, cand_step)
                    else:
                        append_run(out, col, r.seg, oi, r.step_q6)
                    oi += r.step_q6
                    ni += cand_step

        if run_end > ov1:
            old_after = advance(r.inv_q6, r.step_q6, ov1 + 1 - run_start)
            append_run(out, run_end, r.seg, old_after, r.step_q6)
        run_start = run_end + 1

    assert len(out) <= COLS
    return out

def expand_runs(runs, view0, view1):
    result = []
    start = view0
    for r in runs:
        result.extend([r.seg] * (r.end - start + 1))
        start = r.end + 1
    assert len(result) == view1 - view0 + 1
    return result

def exact_columns(candidates, view0=0, view1=19):
    best = [NO_WALL] * (view1 - view0 + 1)
    depth = [0] * len(best)
    for seg, c0, c1, inv_q6, step_q6 in candidates:
        for col in range(max(c0, view0), min(c1, view1) + 1):
            q = inv8(advance(inv_q6, step_q6, col - c0), step_q6)
            i = col - view0
            if best[i] == NO_WALL or q > depth[i]:
                best[i] = seg
                depth[i] = q
    return best

def run_envelope(candidates, view0=0, view1=19):
    runs = [Run(view1, NO_WALL, 0, 0)]
    for seg, c0, c1, inv_q6, step_q6 in candidates:
        a, b = max(c0, view0), min(c1, view1)
        if a > b:
            continue
        start_inv = advance(inv_q6, step_q6, a - c0)
        runs = merge_candidate(runs, view0, view1, seg, a, b, start_inv, step_q6)
    return expand_runs(runs, view0, view1), runs

def main():
    rng = random.Random(0xFEA217)
    fallback_like = 0
    max_runs = 1
    for case in range(200_000):
        candidates = []
        for seg in range(rng.randint(1, 7)):
            c0 = rng.randint(-5, 19)
            c1 = rng.randint(c0, 24)
            inv_q6 = rng.randint(20, 255) << 6
            step_q6 = rng.randint(-400, 400)
            candidates.append((seg, c0, c1, inv_q6, step_q6))

        exact = exact_columns(candidates)
        got, runs = run_envelope(candidates)
        max_runs = max(max_runs, len(runs))
        if got != exact:
            raise SystemExit(
                f"Stage17 run-envelope mismatch case={case}\n"
                f"candidates={candidates}\nexact={exact}\ngot={got}\nruns={runs}"
            )

    print(f"Stage17 run-envelope equivalence: 200000 randomized scenes PASS; max_runs={max_runs}")

if __name__ == "__main__":
    main()
