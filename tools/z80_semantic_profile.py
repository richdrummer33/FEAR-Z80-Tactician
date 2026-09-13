"""Semantic cost breakdown of the EDGELUT3 materializer.

A23 profiled by subroutine, which answered "which routine is hot" but not
"which ARCHITECTURAL STAGE is hot".  The rungs under discussion each attack a
different stage, so the upper bound on every rung is a number this file has to
produce rather than assume.

Method: inject zero-byte marker labels at the semantic boundaries inside
`run_loop` (a label emits no code, so the kernel measured here is byte-for-byte
the verified EDGELUT3), then attribute every executed instruction's T to the
nearest preceding label and aggregate labels into stages.

Exactness is re-verified in the same pass: the profiled image must reproduce
the pose oracle's name table or the run aborts.
"""
from __future__ import annotations
import bisect
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80                       # noqa: E402
import z80_materialize_run_bench as rb                  # noqa: E402
import z80_materialize_masked_bench as mb               # noqa: E402
import z80_materialize_dda_bench as dda                 # noqa: E402
import z80_edge_lut_bench as el                         # noqa: E402

CODE = 0x0000
ROWS, COLS = rb.ROWS, rb.COLS

# ---- marker injection -------------------------------------------------------
# Each anchor is a unique piece of the shipped source; the label goes in front
# of it.  assert-guarded because a silent miss would mis-attribute a whole
# stage (the A24 `0xC027` vs `0xc027` trap).
MARKERS = [
    ("M_coldecode", "run_loop:\n"),
    ("M_border",    "; ---- border: only the first and last column can carry one ----\n"),
    ("M_colptr",    "; ---- column pointer: MAP + c*2 (row stride added per row) ----\n"),
    ("M_geom",      "; ---- geometry (identical to the per-column kernel) ----\n"),
    ("M_dispatch",  "        call col_bounds\n"),
    ("M_carry",     "; ---- THE CARRY: this column's right endpoint IS the next column's left ----\n"),
]

# ---- label -> architectural stage -------------------------------------------
STAGE_OF = {}
def _st(stage, *labels):
    for l in labels:
        STAGE_OF[l] = stage

_st("1 per-run setup",      "<top>")
_st("2 endpoint decode",    "M_coldecode", "shr6_clamp", "s6_pos", "s6_nonneg",
                            "s6_fits")
_st("3 column walk",        "M_border", "bd_not_first", "bd_done", "M_colptr",
                            "M_carry", "run_loop", "run_done")
_st("4 endpoint geometry",  "M_geom", "pf_not_full", "pf_not_lintel",
                            "pf_not_raised", "pf_done", "sx_tl", "sx_tr",
                            "sx_bl", "sx_br", "sx_hl", "zx_bl", "zx_br",
                            "zx_tl", "zx_tr", "rowfloor", "rf_pos", "shr3_u",
                            "cmps")
_st("5 row extents",        "col_bounds", "cb_hmax_r", "cb_h", "cb_top_dec",
                            "cb_top_riser", "cb_bottom", "cb_bot_lintel",
                            "cb_bot_raised", "cb_bot_store", "cb_ful_ok",
                            "cb_r0_signed", "cb_r0_pos", "cb_r1_signed",
                            "cb_r1_pos", "cb_full_first", "cb_ff_pos",
                            "cb_riser")
_st("6 edge setup",         "M_dispatch", "draw_edge", "de_sl_lo", "de_sl_hi",
                            "de_rows2", "de_rows_bottom", "de_r1ok",
                            "de_rows_have", "de_lb_top")
_st("7 edge row walk",      "de_loop")
_st("8 edge tile select",   "edge_entry", "eel_pos", "eel_neg", "eel_hi",
                            "eel_lo", "eel_have")
_st("9 full setup",         "draw_full", "df_r1ok")
_st("10 full interior fill", "df_loop")
_st("11 row addressing",    "row_addr")


def instrumented_source():
    src = el.build_lut_variant(dda.SRC_DDA, 3)
    for name, anchor in MARKERS:
        assert anchor in src, f"marker anchor moved: {name}"
        src = src.replace(anchor, f"{name}:\n{anchor}", 1)
    return src


def base_image(src, rows):
    code, labels = assemble(src, CODE)
    img = bytearray(0x10000)
    img[CODE:CODE + len(code)] = code
    low, high = mb.tables()
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
    return img, labels, len(code)


def main():
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 120
    dump = ROOT / "build" / "coverage_pose_oracle.txt"
    if not dump.exists():
        raise SystemExit(f"missing {dump} - run `make coverage-potential`")
    lines = [l.split() for l in dump.read_text().splitlines() if l.strip()]
    step = max(1, len(lines) // limit)
    cases = []
    for f in lines[::step]:
        n = int(f[0])
        runs = [tuple(int(v) for v in f[1 + 8 * k: 9 + 8 * k]) for k in range(n)]
        cases.append((runs, [int(v) for v in f[1 + 8 * n:]]))

    src = instrumented_source()
    img, labels, nbytes = base_image(src, el.load_table())
    marks = sorted((v, k) for k, v in labels.items())
    addrs = [a for a, _ in marks]
    names = [n for _, n in marks]
    bgm = mb.background_map()

    cost, total, fails = {}, 0, 0
    ncols = nruns = 0
    # Write census, counted in the same pass so it cannot drift from the
    # profile.  The fill loop is register-resident after FILLLOOP_D, so it
    # stores with `ld (hl),c` / `ld (hl),b`, not `ld (hl),a` - counting only
    # 0x77 undercounts the traffic by 3x.  All three are counted here.
    STORE_OPS = (0x77, 0x71, 0x70)
    tally = dict(de=0, dl=0, df=0, fl=0, words=0)
    cells = set()
    L_DE, L_DL = labels["draw_edge"], labels["de_loop"]
    L_DF, L_FL = labels["draw_full"], labels["df_loop"]

    class P(Z80):
        def _step(self):
            pc, t0 = self.pc, self.t
            op = self.m[pc]
            if pc == L_DE: tally["de"] += 1
            elif pc == L_DL: tally["dl"] += 1
            elif pc == L_DF: tally["df"] += 1
            elif pc == L_FL: tally["fl"] += 1
            if op in STORE_OPS:
                h = self.hl
                if mb.MAP <= h < mb.MAP + 2 * ROWS * COLS:
                    if not (h - mb.MAP) & 1:          # count each word once
                        tally["words"] += 1
                        cells.add((h - mb.MAP) // 2)
            Z80._step(self)
            i = bisect.bisect_right(addrs, pc) - 1
            nm = names[i] if i >= 0 else "<top>"
            cost[nm] = cost.get(nm, 0) + (self.t - t0)

    for runs, want in cases:
        mem = bytearray(img)
        for i, v in enumerate(bgm):
            mem[mb.MAP + 2 * i] = v & 0xFF
            mem[mb.MAP + 2 * i + 1] = v >> 8
        for i in range(COLS * 3):
            mem[mb.COV + i] = 0
        for iq, stp, c0, c1, prof, lr, rr, sh in reversed(runs):
            mem[rb.IQ] = iq & 0xFF; mem[rb.IQ + 1] = (iq >> 8) & 0xFF
            mem[rb.STEP] = stp & 0xFF; mem[rb.STEP + 1] = (stp >> 8) & 0xFF
            mem[rb.PROFILE], mem[rb.C0], mem[rb.C1] = prof, c0, c1
            mem[rb.LREAL], mem[rb.RREAL], mem[rb.SHADE] = lr, rr, sh
            cpu = P(mem)
            cpu.run(CODE)
            total += cpu.t
            mem = cpu.m
            nruns += 1
            ncols += c1 - c0 + 1
        got = [mem[mb.MAP + 2 * i] | (mem[mb.MAP + 2 * i + 1] << 8)
               for i in range(ROWS * COLS)]
        if got != want:
            fails += 1
    if fails:
        raise SystemExit(f"FAIL: instrumented image wrong on {fails}/{len(cases)} poses")

    n = len(cases)
    print(f"EDGELUT3 semantic profile - {n} poses, {nruns} runs, {ncols} "
          f"column-materializations, {total:,} T  ({nbytes} bytes)")
    print(f"VERIFIED {n}/{n} poses exact against the pose oracle\n")
    per = total / n

    by_stage = {}
    unknown = []
    for nm, t in cost.items():
        s = STAGE_OF.get(nm)
        if s is None:
            unknown.append((nm, t))
            s = "?? UNMAPPED"
        by_stage[s] = by_stage.get(s, 0) + t
    if unknown:
        print("  UNMAPPED LABELS:", unknown)

    print(f"{'stage':24} {'T/pose':>10} {'share':>7} {'T/column':>9}")
    order = sorted(by_stage.items(), key=lambda kv: -kv[1])
    for s, t in order:
        print(f"{s:24} {t/n:10,.0f} {100.0*t/total:6.1f}% {t/ncols:9,.1f}")
    print(f"{'TOTAL':24} {per:10,.0f} {100.0:6.1f}% {total/ncols:9,.1f}")

    # ---- write census and the output floor ------------------------------
    w = tally["words"] / n
    print(f"\nwrite census")
    print(f"  draw_edge calls/pose   {tally['de']/n:8.1f}"
          f"   rows drawn {tally['dl']/n:7.1f}"
          f"   rows per call {tally['dl']/max(1,tally['de']):5.2f}")
    print(f"  draw_full calls/pose   {tally['df']/n:8.1f}"
          f"   rows drawn {tally['fl']/n:7.1f}"
          f"   rows per call {tally['fl']/max(1,tally['df']):5.2f}")
    print(f"  name-table words written/pose {w:8.1f}  of 360")
    print(f"  T per word actually written   {total/max(1,tally['words']):8.0f}")
    edge_t = sum(by_stage.get(k, 0) for k in
                 ("6 edge setup", "7 edge row walk", "8 edge tile select"))
    print(f"  edge path  {edge_t/n:9,.0f} T/pose over {tally['dl']/n:.1f} rows"
          f"  = {edge_t/max(1,tally['dl']):6.0f} T per edge row")
    fill_t = by_stage.get("10 full interior fill", 0)
    print(f"  fill path  {fill_t/n:9,.0f} T/pose over {tally['fl']/n:.1f} rows"
          f"  = {fill_t/max(1,tally['fl']):6.0f} T per fill row")

    print(f"\noutput floor - the same {w:.1f} words, with ALL derivation removed")
    print(f"  {'basis':38} {'T/pose':>9} {'% of materializer':>18}")
    for label, tpw in (("emit bench, measured (A3)   60.43 T/word", 60.43),
                       ("register-resident store     26 T/word", 26.0),
                       ("LDIR from a patch stream    42 T/word", 42.0)):
        print(f"  {label:38} {w*tpw:9,.0f} {100.0*w*tpw/per:17.1f}%")

    print(f"\nper-label detail (top 40)")
    for nm, t in sorted(cost.items(), key=lambda kv: -kv[1])[:40]:
        print(f"  {nm:18s} {t/n:9,.0f} T/pose {100.0*t/total:6.1f}%"
              f"   [{STAGE_OF.get(nm,'?')}]")


if __name__ == "__main__":
    main()
