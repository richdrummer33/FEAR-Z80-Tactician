#!/usr/bin/env python3
"""Cross-check machine-readable parity claims against repository integration files."""
from __future__ import annotations

import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
STATUS = ROOT / "docs" / "PARITY_STATUS.json"


def main() -> int:
    data = json.loads(STATUS.read_text())
    allowed = set(data["states"])
    bad = False
    seen = set()

    for f in data["features"]:
        fid = f["id"]
        if fid in seen:
            print(f"PARITY_MANIFEST_FAIL duplicate feature {fid}", file=sys.stderr)
            bad = True
        seen.add(fid)

        if f["status"] not in allowed:
            print(f"PARITY_MANIFEST_FAIL {fid}: unknown status {f['status']}", file=sys.stderr)
            bad = True

        integ = f.get("integration")
        if f["status"] in {"ROM-EXPERIMENT", "ROM-EXACT", "ROM-EQUIV"}:
            if not integ or integ == "multiple":
                print(f"PARITY_MANIFEST_FAIL {fid}: integrated status lacks concrete source", file=sys.stderr)
                bad = True
            elif not (ROOT / integ).exists():
                print(f"PARITY_MANIFEST_FAIL {fid}: missing integration path {integ}", file=sys.stderr)
                bad = True

        if f["status"] == "Z80-ONLY" and integ:
            print(f"PARITY_MANIFEST_FAIL {fid}: Z80-ONLY unexpectedly names ROM integration {integ}", file=sys.stderr)
            bad = True

    if bad:
        return 1
    print(f"PARITY_MANIFEST_RESULT=PASS features={len(seen)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
