#!/usr/bin/env python3
"""Gate downstream parity claims on prerequisite architecture.

This prevents the old failure mode where a downstream Z80 optimization appears
in a composed estimate before the ROM contains the architecture it depends on.
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
DATA = json.loads((ROOT / "docs" / "PARITY_STATUS.json").read_text())
FEATURES = {f["id"]: f for f in DATA["features"]}
ROM_STATES = {"ROM-EXACT", "ROM-EQUIV", "ROM-EXPERIMENT"}

DEPS = {
    "progjoin": ["finite-edge-programs"],
    "run-edge-descriptor-hoist": ["finite-edge-programs", "progjoin"],
}


def main() -> int:
    bad = False
    for feature, deps in DEPS.items():
        state = FEATURES[feature]["status"]
        if state not in ROM_STATES:
            continue
        for dep in deps:
            dep_state = FEATURES[dep]["status"]
            if dep_state not in ROM_STATES:
                print(
                    f"PARITY_DEP_FAIL {feature}={state} requires {dep}, "
                    f"but {dep}={dep_state}",
                    file=sys.stderr,
                )
                bad = True
    if bad:
        return 1
    print("PARITY_DEP_RESULT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
