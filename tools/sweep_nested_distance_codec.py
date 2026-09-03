#!/usr/bin/env python3
"""Rate/distortion sweep for the nested distance co-design experiment.

Runs the stable alternating solver across representative angle indices and
nearest-view quality budgets, continuously rewriting one compact JSON summary.
This is a research harness rather than a shipping codec tool.

The sweep intentionally reuses the exact same solver/problem classes as the
single-run experiment so the frontier cannot quietly drift onto a different
objective or decoder.
"""

import argparse
import json
import os
import pathlib
import time

from analyze_doomguy_dense_corpus import Corpus
from iterative_solver import AtomicSolverJournal, StableAlternatingSolver
from solve_nested_distance_codec import NestedDistanceProblem


def parse_csv_ints(text):
    out = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        value = int(part)
        if value not in out:
            out.append(value)
    if not out:
        raise argparse.ArgumentTypeError("need at least one integer")
    return out


def atomic_json(path, payload):
    if not path:
        return
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n")
    os.replace(tmp, path)


def frontier(points):
    """Return nondominated points sorted by actual near damage.

    A point is dominated when another point spends no more nearest-view error
    and has no worse farther-distance objective, with at least one strict win.
    """
    ordered = sorted(
        points,
        key=lambda p: (
            p["near_weighted"],
            p["far_weighted"],
            p["budget"]))
    out = []
    best_far = float("inf")
    for point in ordered:
        if point["far_weighted"] < best_far - 1e-9:
            out.append(point)
            best_far = point["far_weighted"]
    return out


def summarize_angle(angle, points):
    base = min(points, key=lambda p: p["budget"])
    best = min(points, key=lambda p: p["far_weighted"])
    f = frontier(points)
    return {
        "angle": angle,
        "baseline_far_weighted": base["far_weighted"],
        "best_far_weighted": best["far_weighted"],
        "best_improvement_abs": (
            base["far_weighted"] - best["far_weighted"]),
        "best_improvement_pct": (
            round(100.0 * (base["far_weighted"] - best["far_weighted"]) /
                  base["far_weighted"], 4)
            if base["far_weighted"] else 0.0),
        "frontier": [
            {
                "budget": p["budget"],
                "near_weighted": p["near_weighted"],
                "near_silhouette_pct": p["near_silhouette_pct"],
                "far_weighted": p["far_weighted"],
                "far_improvement_abs": (
                    base["far_weighted"] - p["far_weighted"]),
                "far_improvement_pct": (
                    round(100.0 * (base["far_weighted"] -
                                   p["far_weighted"]) /
                          base["far_weighted"], 4)
                    if base["far_weighted"] else 0.0),
            }
            for p in f
        ],
    }


def write_tsv(path, results):
    if not path:
        return
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "angle\tbudget\tnear_weighted\tnear_silhouette_pct\t"
        "far_weighted\timprovement_abs\timprovement_pct\t"
        "patterns\tselector_bytes\tstatus"
    ]
    by_angle = {}
    for p in results:
        by_angle.setdefault(p["angle"], []).append(p)
    baselines = {
        angle: min(points, key=lambda p: p["budget"])["far_weighted"]
        for angle, points in by_angle.items()
    }
    for p in sorted(results, key=lambda x: (x["angle"], x["budget"])):
        base = baselines[p["angle"]]
        imp = base - p["far_weighted"]
        pct = 100.0 * imp / base if base else 0.0
        lines.append(
            f'{p["angle"]}\t{p["budget"]}\t{p["near_weighted"]:.3f}\t'
            f'{p["near_silhouette_pct"]:.4f}\t{p["far_weighted"]:.3f}\t'
            f'{imp:.3f}\t{pct:.4f}\t{p["patterns"]}\t'
            f'{p["selector_bytes"]}\t{p["status"]}'
        )
    path.write_text("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--angles", type=parse_csv_ints,
                    default=parse_csv_ints("0,64,128,192"))
    ap.add_argument("--budgets", type=parse_csv_ints,
                    default=parse_csv_ints("0,64,128,192,320"))
    ap.add_argument("--iterations", type=int, default=2)
    ap.add_argument("--patience", type=int, default=1)
    ap.add_argument("--proposal-limit", type=int, default=12)
    ap.add_argument("--phase-radius", type=int, default=2)
    ap.add_argument("--near-lambda", type=float, default=4.0)
    ap.add_argument("--escape-probes", type=int, default=1)
    ap.add_argument("--escape-steps", type=int, default=4)
    ap.add_argument("--escape-temperature", type=float, default=24.0)
    ap.add_argument("--escape-cooling", type=float, default=0.82)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--summary-json", required=True)
    ap.add_argument("--table-tsv")
    ap.add_argument("--trace-dir")
    args = ap.parse_args()

    c = Corpus(args.corpus)
    for angle in args.angles:
        if angle < 0 or angle >= c.angles:
            raise SystemExit(
                f"angle {angle} outside 0..{c.angles - 1}")

    started = time.time()
    results = []
    summary = {
        "schema": "nested-distance-rate-distortion-v1",
        "status": "running",
        "elapsed_sec": 0.0,
        "angles": args.angles,
        "budgets": args.budgets,
        "completed": 0,
        "total": len(args.angles) * len(args.budgets),
        "current": None,
        "results": results,
        "angle_summaries": [],
    }
    atomic_json(args.summary_json, summary)

    for angle in args.angles:
        for budget in args.budgets:
            current = {
                "angle": angle,
                "budget": budget,
                "started_elapsed_sec": round(time.time() - started, 3),
            }
            summary["current"] = current
            summary["elapsed_sec"] = round(time.time() - started, 3)
            atomic_json(args.summary_json, summary)
            print(
                "RD_SWEEP_START " +
                json.dumps(current, sort_keys=True),
                flush=True)

            problem = NestedDistanceProblem(
                c,
                angle=angle,
                phase_radius=args.phase_radius,
                near_budget=float(budget),
                near_lambda=args.near_lambda,
                near_objective_weight=0.0,
                proposal_limit=args.proposal_limit,
            )

            status_path = None
            trace_path = None
            if args.trace_dir:
                td = pathlib.Path(args.trace_dir)
                td.mkdir(parents=True, exist_ok=True)
                stem = f"a{angle:03d}-b{budget:04d}"
                status_path = str(td / f"{stem}-status.json")
                trace_path = str(td / f"{stem}-trace.ndjson")

            journal = AtomicSolverJournal(
                status_path, trace_path, echo=False)
            solver = StableAlternatingSolver(
                problem,
                journal,
                max_iterations=args.iterations,
                patience=args.patience,
                escape_probes=args.escape_probes,
                escape_steps=args.escape_steps,
                escape_temperature=args.escape_temperature,
                escape_cooling=args.escape_cooling,
                seed=args.seed + angle * 1009 + budget * 17,
                status_every=max(1, args.proposal_limit),
            )
            _, _, ev = solver.solve(problem.master_oracle.copy())
            m = ev.metrics
            result = {
                "angle": angle,
                "budget": budget,
                "status": "ok" if ev.feasible else "infeasible",
                "objective": float(ev.objective),
                "far_weighted": float(m["far_weighted"]),
                "near_weighted": float(m["near_weighted"]),
                "near_budget_fraction": m["near_budget_fraction"],
                "near_silhouette_pct": float(m["near_silhouette_pct"]),
                "near_changed_tiles": int(m["near_changed_tiles"]),
                "patterns": int(m["distinct_master_patterns"]),
                "selector_bytes": int(m["selector_bytes_estimate"]),
                "levels": m["levels"],
                "elapsed_sec": round(time.time() - started, 3),
            }
            results.append(result)

            by_angle = {}
            for p in results:
                by_angle.setdefault(p["angle"], []).append(p)
            summary["completed"] = len(results)
            summary["current"] = None
            summary["elapsed_sec"] = round(time.time() - started, 3)
            summary["angle_summaries"] = [
                summarize_angle(a, pts)
                for a, pts in sorted(by_angle.items())
            ]
            atomic_json(args.summary_json, summary)
            write_tsv(args.table_tsv, results)
            print(
                "RD_SWEEP_RESULT " +
                json.dumps(result, sort_keys=True),
                flush=True)

    summary["status"] = "complete"
    summary["current"] = None
    summary["elapsed_sec"] = round(time.time() - started, 3)
    atomic_json(args.summary_json, summary)
    write_tsv(args.table_tsv, results)

    print(
        "RD_SWEEP_SUMMARY " +
        json.dumps({
            "completed": summary["completed"],
            "total": summary["total"],
            "elapsed_sec": summary["elapsed_sec"],
            "angle_summaries": summary["angle_summaries"],
        }, sort_keys=True),
        flush=True)
    print("RD_SWEEP_PASS")


if __name__ == "__main__":
    main()
