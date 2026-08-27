#!/usr/bin/env python3
from pathlib import Path
import re

s=Path("src/tilesector_symfull_gg.s").read_text()
m=re.search(
    r"sf_restore_base_ready\$:(.*?)sf_ret_edge_delta\$:",
    s,re.S
)
if not m:
    raise SystemExit("retained base-restore block not found")
block=m.group(1)
pat=re.compile(
    r"push\s+de.*?call\s+sf_map_ptr_for_row\$.*?pop\s+de"
    r".*?ld\s+\(hl\),\s*e.*?inc\s+hl.*?ld\s+\(hl\),\s*d",
    re.S
)
if not pat.search(block):
    raise SystemExit(
        "FAIL: retained base word is not preserved across "
        "sf_map_ptr_for_row$ DE-clobbering helper"
    )
print("Stage28 retained restore ABI: DE preservation PASS")
