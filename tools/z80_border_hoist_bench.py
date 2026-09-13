"""BORDERHOIST: the border byte only matters on two columns of a run.

A46 profiled the border path at 6,230 T/pose (3.5% of the materializer).  It
runs on all 33.4 columns per pose but can only matter on the 8.4 that begin or
end a run - only `c0` can carry a left border and only `c1` a right one.

So compute the FULL tile pointer for `c0` once before the loop, and refresh it
in the carry only when the NEXT column is `c1`.  Every middle column then does
one compare and one store instead of two compares, two loads, a mask and an
add.  Exact: nothing else reads the border byte.

A/B against HOIST_A (A41's early-out reorder, itself exact and shipping), one
change, nothing else touched.
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble                            # noqa: E402
import z80_materialize_masked_bench as mb               # noqa: E402
import z80_materialize_dda_bench as dda                 # noqa: E402
import z80_edge_lut_bench as el                         # noqa: E402
import z80_edge_hoist_bench as eh                       # noqa: E402

BORDER_BLOCK_START = "; ---- border: only the first and last column can carry one ----"
BORDER_BLOCK_END = "; ---- column pointer"

PRELOOP = """; ---- BORDERHOIST: the FULL pointer for c0, computed once ----
        ld hl,(0xc038)               ; FULL base, no border
        ld a,(0xc007)                ; LREAL
        or a
        jp z,bh_no_l0
        inc hl                       ; border bit 0
bh_no_l0:
        ld a,(0xc005)                ; C0
        ld b,a
        ld a,(0xc006)                ; C1
        cp b
        jp nz,bh_set0                ; a one-column run carries BOTH borders
        ld a,(0xc008)                ; RREAL
        or a
        jp z,bh_set0
        inc hl
        inc hl                       ; border bit 1
bh_set0:
        ld (0xc02c),hl
run_loop:"""

CARRY_OLD = """        ld a,b
        inc a
        ld (0xc030),a
        jp run_loop"""

CARRY_NEW = """        ld a,b
        inc a
        ld (0xc030),a
; ---- BORDERHOIST: only the LAST column can still gain a border ----
        ld c,a                       ; the column about to be drawn
        ld hl,(0xc038)
        ld a,(0xc006)                ; C1
        cp c
        jp nz,bh_setn
        ld a,(0xc008)                ; RREAL
        or a
        jp z,bh_setn
        inc hl
        inc hl
bh_setn:
        ld (0xc02c),hl
        jp run_loop"""


def build_borderhoist(src):
    i = src.index(BORDER_BLOCK_START)
    j = src.index(BORDER_BLOCK_END, i)
    src = src[:i] + src[j:]                      # drop the per-column block
    assert "run_loop:" in src
    src = src.replace("run_loop:", PRELOOP, 1)
    assert CARRY_OLD in src, "carry tail moved"
    return src.replace(CARRY_OLD, CARRY_NEW, 1)


def main():
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    lines = [l.split() for l in
             open(ROOT / "build" / "coverage_pose_oracle.txt") if l.strip()]
    step = max(1, len(lines) // limit)
    cases = []
    for f in lines[::step]:
        n = int(f[0])
        runs = [tuple(int(v) for v in f[1 + 8 * k: 9 + 8 * k]) for k in range(n)]
        cases.append((runs, [int(v) for v in f[1 + 8 * n:]]))

    low, high = mb.tables()
    bgm = mb.background_map()
    rows = el.load_table()
    base = el.build_lut_variant(dda.SRC_DDA, 3)
    hoist = eh.build_hoist(base)

    res = {}
    for name, src in (("EDGELUT3", base),
                      ("HOIST_A", hoist),
                      ("BORDERHOIST", build_borderhoist(hoist))):
        code, _ = assemble(src, 0)
        img = bytearray(0x10000)
        img[0:len(code)] = code
        img[mb.LOWTAB:mb.LOWTAB + len(low)] = low
        img[mb.HIGHTAB:mb.HIGHTAB + len(high)] = high
        for r, vals in rows.items():
            for i, v in enumerate(vals):
                img[el.LUT + 64 * r + 2 * i] = v & 0xFF
                img[el.LUT + 64 * r + 2 * i + 1] = (v >> 8) & 0xFF
        for r in range(30):
            a = el.LUT + 64 * r
            img[0xB800 + 2 * r] = a & 0xFF
            img[0xB800 + 2 * r + 1] = a >> 8
        fails, ts = 0, []
        for runs, want in cases:
            t, got = mb.run_pose(img, runs, False, bgm)
            ts.append(t)
            if got != want:
                fails += 1
                if fails == 1:
                    bad = [i for i in range(360) if got[i] != want[i]]
                    print(f"  MISMATCH {name}: {len(bad)} cells, first "
                          f"r{bad[0]//20} c{bad[0]%20} want {want[bad[0]]} "
                          f"got {got[bad[0]]}")
        res[name] = (stt.mean(ts), len(code), fails)
        print(f"{name:13} {stt.mean(ts):10,.1f} T/pose  {len(code):5d} bytes  "
              f"{'EXACT' if not fails else str(fails)+'/'+str(len(cases))+' WRONG'}")

    a = res["HOIST_A"][0]
    b = res["BORDERHOIST"][0]
    print(f"\n  BORDERHOIST vs HOIST_A: {100*b/a-100:+.1f}%  saves {a-b:,.0f} T/pose")
    print(f"  vs EDGELUT3 baseline:   {100*b/res['EDGELUT3'][0]-100:+.1f}%")
    print(f"  whole update 223,266 -> {223266-(res['EDGELUT3'][0]-b):,.0f} T")
    return 1 if any(v[2] for v in res.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
