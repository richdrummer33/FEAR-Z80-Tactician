#!/usr/bin/env python3
from pathlib import Path

core_p=Path("src/tilesector_core.c")
run_p=Path("src/tilesector_run_gg.s")
sym_p=Path("src/tilesector_symfull_gg.s")
nt_p=Path("src/tilesector_ntstate_gg.s")

core=core_p.read_text()
run=run_p.read_text()
sym=sym_p.read_text()
nt=nt_p.read_text()

if "STAGE28D_DEFERRED_FINAL_RECONCILE" in sym:
    print("Stage 28D deferred reconcile already applied")
    raise SystemExit(0)
if "STAGE28B_RETAINED_FAST_GATES" not in sym:
    raise SystemExit("apply Stage 28B retained gates first")
if "STAGE28C_OWNERSHIP_GATE" not in nt:
    raise SystemExit("apply Stage 28C ownership gate first")

sym=sym.replace(
    "; STAGE28B_RETAINED_FAST_GATES\n",
    "; STAGE28B_RETAINED_FAST_GATES\n; STAGE28D_DEFERRED_FINAL_RECONCILE\n",1)
nt=nt.replace(
    "; STAGE28C_OWNERSHIP_GATE\n",
    "; STAGE28C_OWNERSHIP_GATE\n; STAGE28D_DEFERRED_FINAL_RECONCILE\n",1)

# ---------------------------------------------------------------------------
# CORE: do NOT retire old retained FULL before portal/exception drawing.
# Render the complete root scene first, then reconcile vanished retained state
# against FINAL current exception coverage exactly once.
# ---------------------------------------------------------------------------
old="""    g_ts_render_stage=4u;
    render_sector_candidates(depth,view_c0,view_c1,out_map,cols);
#ifdef __SDCC
    if (depth==0u) {
        g_ts_render_stage=7u; /* retained FULL lifetime / unseen columns */
        ts_retained_full_finalize_unseen();
    }
#endif
    g_ts_render_stage=5u;
"""
new="""    g_ts_render_stage=4u;
    render_sector_candidates(depth,view_c0,view_c1,out_map,cols);
    g_ts_render_stage=5u;
"""
if old not in core:
    raise SystemExit("Stage24 early finalizer block not found")
core=core.replace(old,new,1)

build_i=core.find("void ts_build_tilemap(")
if build_i < 0:
    raise SystemExit("ts_build_tilemap function not found")
idle_i=core.find("    g_ts_render_stage=0u;",build_i)
if idle_i < 0:
    raise SystemExit("ts_build_tilemap final idle stage not found")
insert="""#ifdef __SDCC
    /* Final-state lifetime reconciliation: all normal, portal and exception
     * geometry has now written its current hardware-visible state. */
    g_ts_render_stage=7u;
    ts_retained_full_finalize_unseen();
#endif
"""
core=core[:idle_i]+insert+core[idle_i:]

# ---------------------------------------------------------------------------
# RUN KERNEL: KEEP Stage24/28B eager invalidation for a depth-0 non-FULL
# replacement. It consumes the prior retained-active bit BEFORE the new
# RAISED/LINTEL/RISER-style materializer writes, so the deferred unseen
# finalizer cannot later restore base over that current solid. Stage28B already
# gates this call when no previous retained FULL exists.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# NTSTATE: expose current exception occupancy as a rare cleanup query.
# A=row 0..17, E=column 0..19 -> A=1 iff CURRENT exception coverage owns cell.
# The active mask guards lazily-stale g_nt_cov_cur bytes.
# ---------------------------------------------------------------------------
anchor="_ts_nt_end_frame::\n"
if anchor not in nt:
    raise SystemExit("ntstate end-frame anchor not found")
helper=r"""; A=row, E=column. Return A=1 iff CURRENT exception coverage owns cell.
; This is a cleanup-path query, not a normal materializer operation.
_ts_nt_exception_owns_cell::
        push    bc
        push    de
        push    hl

        ld      (#nte_query_row$), a
        ld      a, e
        ld      (#nte_query_col$), a

        ; First reject columns with no current exception coverage.
        call    nte_exc_slot$
        ld      d, #0
        ld      hl, #_g_nt_exc_cur_active
        add     hl, de
        ld      a, (hl)
        and     c
        jp      z, nte_query_no$

        ; HL = current 3-byte exception coverage for queried column.
        ld      a, (#nte_query_col$)
        ld      e, a
        add     a, a
        add     a, e
        ld      e, a
        ld      d, #0
        ld      hl, #_g_nt_cov_cur
        add     hl, de
        push    hl

        ; Row LUT gives [byte offset, bit mask] within the 18-bit mask.
        ld      a, (#nte_query_row$)
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #nte_exc_row_lut$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      c, (hl)
        ld      d, #0
        pop     hl
        add     hl, de
        ld      a, (hl)
        and     c
        jp      z, nte_query_no$
        ld      a, #1
        jp      nte_query_done$

nte_query_no$:
        xor     a
nte_query_done$:
        pop     hl
        pop     de
        pop     bc
        ret

"""
nt=nt.replace(anchor,helper+anchor,1)

# Row -> [coverage byte offset, mask].
code_anchor="nt_map_rows$:\n"
if code_anchor not in nt:
    raise SystemExit("ntstate code-table anchor not found")
row_lut=r"""nte_exc_row_lut$:
        .db 0,0x01, 0,0x02, 0,0x04, 0,0x08
        .db 0,0x10, 0,0x20, 0,0x40, 0,0x80
        .db 1,0x01, 1,0x02, 1,0x04, 1,0x08
        .db 1,0x10, 1,0x20, 1,0x40, 1,0x80
        .db 2,0x01, 2,0x02

"""
nt=nt.replace(code_anchor,row_lut+code_anchor,1)

# Query scratch in existing BSS.
bss_anchor="nte_exc_cov_group$:     .ds 1\n"
if bss_anchor not in nt:
    raise SystemExit("Stage28 exception BSS anchor missing")
nt=nt.replace(
    bss_anchor,
    bss_anchor+"nte_query_row$:          .ds 1\nnte_query_col$:          .ds 1\n",1)

# ---------------------------------------------------------------------------
# SYMFULL: stale retained base restore must never erase FINAL current exception
# output. All actual rendering has already happened when final unseen cleanup
# runs, so this prevents restore-then-overwrite transactions.
# ---------------------------------------------------------------------------
glob_anchor="        .globl  _ts_edge_lut\n"
if glob_anchor not in sym:
    raise SystemExit("symfull glob anchor missing")
sym=sym.replace(
    glob_anchor,
    glob_anchor+"        .globl  _ts_nt_exception_owns_cell\n",1)

anchor="""sf_restore_base_row$:
        ld      (#sf_store_row$), a
        cp      #9
"""
repl="""sf_restore_base_row$:
        ld      (#sf_store_row$), a

        ; Preserve final current exception/portal ownership. This turns lifetime
        ; cleanup into a true final-state reconciliation rather than an eager
        ; restore that the exception materializer immediately overwrites.
        ld      a, (#sf_col$)
        ld      e, a
        ld      a, (#sf_store_row$)
        call    _ts_nt_exception_owns_cell
        or      a
        ret     nz

        ld      a, (#sf_store_row$)
        cp      #9
"""
if anchor not in sym:
    raise SystemExit("symfull base-restore anchor missing")
sym=sym.replace(anchor,repl,1)

core_p.write_text(core)
run_p.write_text(run)
sym_p.write_text(sym)
nt_p.write_text(nt)
print("Applied Stage 28D deferred final-state lifetime reconciliation.")
