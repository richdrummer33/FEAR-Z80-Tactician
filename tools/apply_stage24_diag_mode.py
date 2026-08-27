#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv)!=2 or sys.argv[1] not in {
    "no-finalizer","no-oldstale","legacy-lifetime"
}:
    raise SystemExit(
        "usage: apply_stage24_diag_mode.py "
        "no-finalizer|no-oldstale|legacy-lifetime"
    )

mode=sys.argv[1]
core_p=Path("src/tilesector_core.c")
sym_p=Path("src/tilesector_symfull_gg.s")
core=core_p.read_text()
sym=sym_p.read_text()

if "STAGE24_RETAINED_LIFETIME" not in sym:
    raise SystemExit("diagnostic mode requires generated Stage24")
if "STAGE25_ACTIVE_MASK" in sym:
    raise SystemExit("diagnostic mode must run before Stage25")

def disable_finalizer(text):
    needle="        ts_retained_full_finalize_unseen();\n"
    if needle not in text:
        raise SystemExit("Stage24 finalizer call not found")
    return text.replace(
        needle,
        "        /* DIAG: retained unseen finalizer disabled */\n",
        1,
    )

def disable_old_stale(text):
    needle="        call    sf_restore_old_stale$\n"
    n=text.count(needle)
    if n < 2:
        raise SystemExit(f"expected >=2 old-stale calls, found {n}")
    return text.replace(
        needle,
        "        ; DIAG: per-column old->new stale restore disabled\n",
    )

if mode=="no-finalizer":
    core=disable_finalizer(core)

elif mode=="no-oldstale":
    sym=disable_old_stale(sym)

elif mode=="legacy-lifetime":
    # Retention/materialization remains enabled, but lifetime ownership falls
    # back to Stage18 exactly: ordinary FULL marks generic coverage, while the
    # Stage24 per-column restore and unseen finalizer are both disabled.
    core=disable_finalizer(core)
    sym=disable_old_stale(sym)
    gate="""        ld      a, (#_g_name_run_ctx + 14)
        or      a
        jp      nz, sf_cov_done$
"""
    if gate not in sym:
        raise SystemExit("ordinary-FULL generic-coverage gate not found")
    sym=sym.replace(
        gate,
        "        ; DIAG: ordinary FULL also uses legacy Stage18 coverage\n",
        1,
    )

core_p.write_text(core)
sym_p.write_text(sym)
print(f"Applied Stage24 diagnostic mode: {mode}")
