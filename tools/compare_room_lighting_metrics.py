#!/usr/bin/env python3
"""Compare legacy binary point-light bundle metrics with wall-angle lighting."""
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


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} CONTROL_MANIFEST ANGLE_MANIFEST", file=sys.stderr)
        return 2
    control = read_manifest(Path(sys.argv[1]))
    angle = read_manifest(Path(sys.argv[2]))
    if control["routes"] != angle["routes"]:
        raise SystemExit("control/angle route count mismatch")

    for label, data in (("control", control), ("angle", angle)):
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

    for key in SUM_KEYS:
        delta = angle[key] - control[key]
        print(
            f"LIGHTING_DELTA {key}={delta:+d} "
            f"percent={pct(angle[key], control[key]):+.3f}"
        )
    for key in MAX_KEYS:
        print(
            f"LIGHTING_DELTA {key}={angle[key] - control[key]:+d} "
            f"control={control[key]} angle={angle[key]}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
