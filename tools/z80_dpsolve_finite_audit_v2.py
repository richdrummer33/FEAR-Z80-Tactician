#!/usr/bin/env python3
"""Corrected runner for z80_dpsolve_finite_audit.

The first audit correctly exposed a harness bug: its jump-table lookup clobbered
HL/DE before entering an unrolled endq case.  Reload iq/step inside each case,
then run the same full exact A/B.
"""
import z80_dpsolve_finite_audit as a
import z80_column_solve_bench as cs
import z80_target_solve_bench as ds


def corrected_end_jump_block():
    cases = []
    for n in range(1, 21):
        cases.append(
            f"dp_end_{n}:\n"
            f"        ld hl,({cs.IQ:#06x})\n"
            f"        ld de,({cs.STEP:#06x})\n"
            f"{a.unrolled_add(n)}"
            f"        jp dp_end_done\n")
    return f"""; ---- endq = iq + n*step, finite n=1..20 ----
        ld a,({cs.NCOL:#06x})
        add a,a
        ld l,a
        ld h,0
        ld de,{a.ENDTAB:#06x}
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ex de,hl
        jp (hl)
{''.join(cases)}dp_end_done:
        ld ({ds.ENDQ:#06x}),hl
"""


a.end_jump_block = corrected_end_jump_block
raise SystemExit(a.main())
