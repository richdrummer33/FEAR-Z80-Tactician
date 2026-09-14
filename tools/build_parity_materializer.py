#!/usr/bin/env python3
"""Build the current Game Gear parity materializer deterministically.

This is a temporary consolidation layer over the existing proven integration
rungs. It gives CI and reviewers one command and one ordered transformation
contract while the optimized assembly is still being graduated into canonical
`src/`.

The builder intentionally stops at rungs that have real ROM integration code.
It does not pretend finite programs/PROGJOIN are integrated.
"""
from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "tilesector_polar_materialize_gg.s"
OUT = ROOT / "build" / "parity" / "tilesector_polar_materialize_gg.generated.s"

STEPS = [
    "tools/apply_integrated_materializer_rung.py",
    "tools/apply_edge_local_dda_rung.py",
    "tools/apply_interior_claimwalker_rung.py",
]


def run(rel: str) -> None:
    print(f"PARITY_APPLY={rel}")
    subprocess.run([sys.executable, rel], cwd=ROOT, check=True)


def main() -> int:
    if not SRC.is_file():
        print(f"missing materializer source: {SRC}", file=sys.stderr)
        return 2

    original = SRC.read_bytes()
    try:
        for step in STEPS:
            run(step)
        OUT.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(SRC, OUT)
        print(f"PARITY_GENERATED={OUT.relative_to(ROOT)}")
        subprocess.run(
            [sys.executable, "tools/check_renderer_parity.py", str(OUT.relative_to(ROOT))],
            cwd=ROOT,
            check=True,
        )
    finally:
        # Building the generated artifact must not leave a dirty working tree.
        SRC.write_bytes(original)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
