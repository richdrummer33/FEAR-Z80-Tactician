#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv)!=2 or sys.argv[1] not in {
    "no-finalizer","no-oldstale","legacy-lifetime","retention-off",
    "legacy-no-exact","legacy-no-edge",
    "stage23-behavior","stage23-plus-owner","stage23-plus-eager",
    "stage23-ret5","stage23-ret7-nocov","stage23-ret7-cov"
}:
    raise SystemExit(
        "usage: apply_stage24_diag_mode.py "
        "no-finalizer|no-oldstale|legacy-lifetime|retention-off|"
        "legacy-no-exact|legacy-no-edge|stage23-behavior|"
        "stage23-plus-owner|stage23-plus-eager|stage23-ret5|"
        "stage23-ret7-nocov|stage23-ret7-cov"
    )

mode=sys.argv[1]
core_p=Path("src/tilesector_core.c")
run_p=Path("src/tilesector_run_gg.s")
sym_p=Path("src/tilesector_symfull_gg.s")
nt_p=Path("src/tilesector_ntstate_gg.s")
core=core_p.read_text()
run=run_p.read_text()
sym=sym_p.read_text()
nt=nt_p.read_text()

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

def disable_eager_nonfull(text):
    start=text.find(
        "        ; If a previous retained FULL wall is replaced by a depth-0 asymmetric"
    )
    if start < 0:
        raise SystemExit("Stage24 eager non-FULL invalidation block not found")
    end=text.find("nr_ret_life_ready$:\n",start)
    if end < 0:
        raise SystemExit("Stage24 eager non-FULL invalidation tail not found")
    end += len("nr_ret_life_ready$:\n")
    return text[:start] + "nr_ret_life_ready$:\n" + text[end:]

def disable_exception_owner(text):
    old="""nte_bit_loop$:
        srl     c
        jp      nc, nte_next_bit$

        ; Generic coverage now tracks exceptions only. A stale exception cell
        ; must not erase a CURRENT retained FULL wall occupying the same tile.
        ld      a, (#nte_col$)
        ld      e, a
        ld      a, (#nte_row$)
        call    _ts_retained_full_owns_cell
        or      a
        jp      nz, nte_next_bit$

        ; Restore static base tile for this exact stale exception cell.
"""
    new="""nte_bit_loop$:
        srl     c
        jr      nc, nte_next_bit$

        ; DIAG: exact Stage18 stale exception restore behavior.
        ; Restore static base tile for this exact stale cell.
"""
    if old not in text:
        raise SystemExit("Stage24 exception ownership gate not found")
    return text.replace(old,new,1)

def force_legacy_coverage(text):
    gate="""        ld      a, (#_g_name_run_ctx + 14)
        or      a
        jp      nz, sf_cov_done$
"""
    if gate not in text:
        raise SystemExit("ordinary-FULL generic-coverage gate not found")
    return text.replace(
        gate,
        "        ; DIAG: ordinary FULL also uses legacy Stage18 coverage\n",
        1,
    )

def remove_old_cov_capture(text):
    block="""        ld      a, 5 (ix)
        ld      (#sf_old_cov_first$), a
        ld      a, 6 (ix)
        ld      (#sf_old_cov_last$), a
"""
    if block not in text:
        raise SystemExit("old coverage capture block not found")
    return text.replace(block,"",1)

def remove_cov_descriptor_store(text):
    old="""        ld      a, (#sf_clip_last$)
        ld      4 (ix), a
        ld      a, (#sf_cov_valid$)
        or      a
        jp      z, sf_ret_store_no_cov$
        ld      a, (#sf_cov_first$)
        ld      5 (ix), a
        ld      a, (#sf_cov_last$)
        ld      6 (ix), a
        jp      sf_ret_cov_stored$
sf_ret_store_no_cov$:
        ld      5 (ix), #0xff
        ld      6 (ix), #0
sf_ret_cov_stored$:
        ld      hl, (#sf_prev_gen_ptr$)
"""
    new="""        ld      a, (#sf_clip_last$)
        ld      4 (ix), a
        ld      hl, (#sf_prev_gen_ptr$)
"""
    if old not in text:
        raise SystemExit("coverage descriptor store block not found")
    return text.replace(old,new,1)

def restore_descriptor_stride5(text):
    old="""        ; IX = seven-byte retained hardware descriptor for this column.
        ld      a, (#sf_col$)
        ld      e, a
        add     a, a                   ; 2x
        add     a, a                   ; 4x
        add     a, a                   ; 8x
        sub     e                      ; 7x
        ld      e, a
"""
    new="""        ; DIAG: Stage23 five-byte retained descriptor for this column.
        ld      a, (#sf_col$)
        ld      e, a
        add     a, a                   ; 2x
        add     a, a                   ; 4x
        add     a, e                   ; 5x
        ld      e, a
"""
    if old not in text:
        raise SystemExit("seven-byte descriptor index block not found")
    return text.replace(old,new,1)

def neutralize_lifetime_support(core,run,sym,nt):
    core=disable_finalizer(core)
    run=disable_eager_nonfull(run)
    sym=disable_old_stale(sym)
    sym=force_legacy_coverage(sym)
    nt=disable_exception_owner(nt)
    return core,run,sym,nt

def bypass_retention(text):
    retain_entry="""sf_cov_done$:

        ; Retention is deliberately restricted to ordinary depth-0 FULL runs.
        ld      a, (#_g_name_run_ctx + 14)
"""
    if retain_entry not in text:
        raise SystemExit("retained entry not found")
    return text.replace(
        retain_entry,
        """sf_cov_done$:
        ; DIAG: bypass all retained descriptor comparison/reuse.
        jp      sf_ret_not_eligible$

sf_ret_diag_unreachable$:
        ld      a, (#_g_name_run_ctx + 14)
""",
        1,
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

elif mode in {"stage23-ret5","stage23-ret7-nocov","stage23-ret7-cov"}:
    # Preserve Stage23 retained exact-skip + edge-delta semantics. Neutralize
    # Stage24 lifetime authority, then progressively add only the descriptor
    # representation changes. This is the clean retention-preserving bisect.
    core,run,sym,nt=neutralize_lifetime_support(core,run,sym,nt)

    if mode in {"stage23-ret5","stage23-ret7-nocov"}:
        sym=remove_cov_descriptor_store(sym)
        sym=remove_old_cov_capture(sym)

    if mode=="stage23-ret5":
        sym=restore_descriptor_stride5(sym)

elif mode in {"stage23-behavior","stage23-plus-owner","stage23-plus-eager"}:
    # A true Stage23-semantics control built from Stage24-generated sources.
    # Retained reuse is bypassed, Stage18 generic coverage is restored, and
    # Stage24 cleanup/support mechanisms are selectively reintroduced.
    core=disable_finalizer(core)
    sym=disable_old_stale(sym)
    sym=force_legacy_coverage(sym)
    sym=bypass_retention(sym)

    if mode != "stage23-plus-eager":
        run=disable_eager_nonfull(run)
    if mode != "stage23-plus-owner":
        nt=disable_exception_owner(nt)

core_p.write_text(core)
run_p.write_text(run)
sym_p.write_text(sym)
nt_p.write_text(nt)
print(f"Applied Stage24 diagnostic mode: {mode}")
