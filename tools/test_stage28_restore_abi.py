#!/usr/bin/env python3
from pathlib import Path
import re

s=Path("src/tilesector_symfull_gg.s").read_text()
run=Path("src/tilesector_run_gg.s").read_text()
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

m=re.search(
    r"_ts_retained_full_owns_cell::(.*?)(?:sf_owns_done\$:.*?ret)",
    s,re.S
)
if not m:
    raise SystemExit("retained ownership helper not found")
owns=m.group(1)
if not re.search(
    r"ld\s+c,\s*a.*?push\s+bc.*?call\s+sf_test_active_cur\$"
    r".*?pop\s+bc.*?jp\s+z,\s*sf_owns_no\$",
    owns,re.S
):
    raise SystemExit(
        "FAIL: queried row/column are not preserved across "
        "sf_test_active_cur$ BC-clobbering helper"
    )
print("Stage28 retained owns-cell ABI: BC preservation PASS")

m=re.search(
    r"sf_restore_range\$:(.*?)(?:sf_restore_base_row\$:)",
    s,re.S
)
if not m:
    raise SystemExit("retained restore-range helper not found")
rr=m.group(1)
if not re.search(r"push\s+de", rr) or not re.search(r"pop\s+de.*?ret", rr, re.S):
    raise SystemExit(
        "FAIL: sf_restore_range$ does not preserve DE stale bounds"
    )
print("Stage28 retained restore-range ABI: DE preservation PASS")

if "_ts_retained_full_invalidate_range" not in run:
    raise SystemExit("FAIL: non-FULL retained invalidation symbol disappeared")
if not re.search(
    r"ld\s+a,\s*\(#_g_ts_ret_full_prev_any\).*?"
    r"jp\s+z,\s*nr_ret_life_ready\$.*?"
    r"call\s+_ts_retained_full_invalidate_range",
    run,re.S
):
    raise SystemExit(
        "FAIL: depth-0 non-FULL replacement no longer uses gated eager "
        "retained invalidation"
    )
print("Stage28 non-FULL replacement invalidation gate: PASS")
