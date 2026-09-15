#!/usr/bin/env python3
"""Run the accepted Z80 research target and enforce its measured contract.

Parity is not 'same idea'.  This gate records the exact research implementation
that the GG path is trying to converge on and refuses to let the target silently
move underneath the ROM work.
"""
from __future__ import annotations
import re, subprocess, sys

COMMANDS = [
    ("progjoin-tuned", [sys.executable, "tools/z80_progjoin_tune_bench.py", "63", "40"], [
        r"oracle verdict\s+BOTH EXACT",
        r"dispatches\s+31,806 / 31,806",
        r"cells played\s+160,717 / 160,717",
        r"tuned total\s+52,266,733 T",
        r"largest-window bodies\s+6,003 -> 4,677 bytes",
        r"VERDICT: KEEP",
    ]),
    ("progjoin-edge-hoist", [sys.executable, "tools/z80_progjoin_edge_hoist_audit.py", "63", "40"], [
        r"oracle verdict\s+BOTH EXACT",
        r"run-edges\s+19,912",
        r"chunks\s+31,806",
        r"edge-hoisted total\s+50,479,935 T",
        r"dispatch\s+946\.7 ->\s+697\.7 T/unit",
        r"VERDICT: KEEP",
    ]),
    ("progjoin-posthoist-profile", [sys.executable, "tools/z80_progjoin_posthoist_profile.py", "63", "40"], [
        r"run-edges 19,912\s+chunks 31,806\s+oracle faults 0",
        r"TOTAL\s+697\.7 T/chunk",
        r"broad crosscheck\s+697\.7 T/chunk",
    ]),
]


def main() -> int:
    print("=== EXECUTABLE Z80 PARITY TARGET SNAPSHOT ===")
    for name, cmd, checks in COMMANDS:
        print(f"\n[{name}] {' '.join(cmd)}")
        p = subprocess.run(cmd, text=True, capture_output=True)
        out = p.stdout + p.stderr
        print(out, end="" if out.endswith("\n") else "\n")
        if p.returncode:
            print(f"TARGET_FAIL {name}: rc={p.returncode}", file=sys.stderr)
            return p.returncode
        for pat in checks:
            if not re.search(pat, out):
                print(f"TARGET_FAIL {name}: missing /{pat}/", file=sys.stderr)
                return 2
        print(f"TARGET_PASS {name}")
    print("\nTARGET_SNAPSHOT_PASS")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
