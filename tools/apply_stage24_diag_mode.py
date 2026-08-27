#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv)!=2 or sys.argv[1] not in {
    "no-finalizer","no-oldstale","legacy-lifetime","retention-off",
    "legacy-no-exact","legacy-no-edge"
}:
    raise SystemExit(
        "usage: apply_stage24_diag_mode.py "
        "no-finalizer|no-oldstale|legacy-lifetime|retention-off|"
        "legacy-no-exact|legacy-no-edge"
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

elif mode=="retention-off":
    # Preserve Stage24-generated code layout, but route ordinary FULL through
    # the Stage21 full materializer every frame and let Stage18 own lifetime.
    # If this matches Stage23, geometry/raster is sound and the divergence is
    # specifically inside retained descriptor/reuse state.
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
        "        ; DIAG: ordinary FULL uses legacy generic coverage\n",
        1,
    )

    retain_entry="""sf_cov_done$:

        ; Retention is deliberately restricted to ordinary depth-0 FULL runs.
        ld      a, (#_g_name_run_ctx + 14)
"""
    if retain_entry not in sym:
        raise SystemExit("retained entry not found")
    sym=sym.replace(
        retain_entry,
        """sf_cov_done$:
        ; DIAG: bypass all retained descriptor comparison/reuse.
        jp      sf_ret_not_eligible$

sf_ret_diag_unreachable$:
        ld      a, (#_g_name_run_ctx + 14)
""",
        1,
    )

elif mode in {"legacy-no-exact","legacy-no-edge"}:
    # Keep the Stage24 7-byte descriptor and retained bookkeeping, but return
    # lifetime ownership to Stage18 so only reuse classification/materializer
    # differences remain under test.
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

    if mode=="legacy-no-exact":
        exact="""        ld      a, (_g_ts_ret_full_skip)
        inc     a
        ld      (_g_ts_ret_full_skip), a
        jp      sf_skip_close$
"""
        if exact not in sym:
            raise SystemExit("exact-skip block not found")
        sym=sym.replace(
            exact,
            """        ; DIAG: exact descriptor hits still fully materialize.
        jp      sf_ret_store_current$
""",
            1,
        )
    else:
        edge="""sf_ret_edge_only$:
        ; DIAG: per-column old->new stale restore disabled
        ld      a, (_g_ts_ret_full_edgeonly)
        inc     a
        ld      (_g_ts_ret_full_edgeonly), a
"""
        if edge not in sym:
            raise SystemExit("edge-only block after cleanup-disable not found")
        sym=sym.replace(
            edge,
            """sf_ret_edge_only$:
        ; DIAG: edge-only classification falls back to full materializer.
        jp      sf_ret_store_current$

sf_ret_edge_diag_unreachable$:
        ld      a, (_g_ts_ret_full_edgeonly)
        inc     a
        ld      (_g_ts_ret_full_edgeonly), a
""",
            1,
        )

core_p.write_text(core)
sym_p.write_text(sym)
print(f"Applied Stage24 diagnostic mode: {mode}")
