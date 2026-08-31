#!/usr/bin/env python3
"""Compare binary, wall-angle, and wall-angle-plus-view room-light metrics."""
from __future__ import annotations

import sys
from pathlib import Path

SUM_KEYS = ("patch_bytes", "tile_bytes", "tile_loads", "changed_words")
MAX_KEYS = ("raw_peak_tile_loads", "scheduled_peak", "scheduled_budget")


def read_manifest(path: Path) -> dict[str, int]:
    totals = {k: 0 for k in SUM_KEYS}
    maxima = {k: 0 for k in MAX_KEYS}
    routes = 0
    for raw in path.read_text(encoding="utf-8").splitlines():
        if " frames=" not in raw or " patch_bytes=" not in raw:
            continue
        fields: dict[str, str] = {}
        for token in raw.split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        if not all(k in fields for k in SUM_KEYS + MAX_KEYS):
            continue
        routes += 1
        for key in SUM_KEYS:
            totals[key] += int(fields[key])
        for key in MAX_KEYS:
            maxima[key] = max(maxima[key], int(fields[key]))
    if routes == 0:
        raise SystemExit(f"no route metric lines in {path}")
    return {"routes": routes, **totals, **maxima}


def pct(new: int, old: int) -> float:
    return 0.0 if old == 0 else (new - old) * 100.0 / old


def show(label: str, data: dict[str, int]) -> None:
    print(
        "LIGHTING_AB "
        f"{label} routes={data['routes']} "
        f"patch_bytes={data['patch_bytes']} "
        f"tile_bytes={data['tile_bytes']} "
        f"tile_loads={data['tile_loads']} "
        f"changed_words={data['changed_words']} "
        f"raw_peak={data['raw_peak_tile_loads']} "
        f"scheduled_peak={data['scheduled_peak']} "
        f"scheduled_budget={data['scheduled_budget']}"
    )


def delta(label: str, old: dict[str, int], new: dict[str, int]) -> None:
    for key in SUM_KEYS:
        d = new[key] - old[key]
        print(f"LIGHTING_DELTA {label} {key}={d:+d} percent={pct(new[key], old[key]):+.3f}")
    for key in MAX_KEYS:
        print(
            f"LIGHTING_DELTA {label} {key}={new[key] - old[key]:+d} "
            f"old={old[key]} new={new[key]}"
        )


def main() -> int:
    if len(sys.argv) not in (4, 5):
        print(
            f"usage: {sys.argv[0]} CONTROL_MANIFEST ANGLE_ONLY_MANIFEST "
            "ANGLE_VIEW_MANIFEST [DITHER16_VIEW_MANIFEST]",
            file=sys.stderr,
        )
        return 2
    control = read_manifest(Path(sys.argv[1]))
    angle = read_manifest(Path(sys.argv[2]))
    angle_view = read_manifest(Path(sys.argv[3]))
    datasets = [control, angle, angle_view]
    dither_view = read_manifest(Path(sys.argv[4])) if len(sys.argv) == 5 else None
    if dither_view is not None:
        datasets.append(dither_view)
    if len({d["routes"] for d in datasets}) != 1:
        raise SystemExit("lighting A/B route count mismatch")

    show("control_binary", control)
    show("solid8_angle_only", angle)
    show("solid8_angle_plus_view", angle_view)
    delta("control_to_solid8_angle", control, angle)
    delta("solid8_angle_to_view", angle, angle_view)
    delta("control_to_solid8_angle_view", control, angle_view)
    if dither_view is not None:
        show("dither16_angle_plus_view", dither_view)
        delta("control_to_dither16_angle_view", control, dither_view)
        delta("dither16_to_solid8", dither_view, angle_view)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
