#!/usr/bin/env python3
"""PROGJOIN dispatch autopsy + exact chunk-advance A/B.

Runs the already-proven selected-count tuned representation over the corpus,
adds zero-byte labels around each dispatch sub-stage, and attributes every Z80
cycle by PC.  In the same pass it compares the current repeated-add
`iq += want*step` loop against a finite C=6 unrolled kernel.  Both variants
consume identical baked programs and must remain oracle-EXACT.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys

import z80_progjoin_bench as pj
import z80_progjoin_tune_bench as tune

ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE_BUILD = tune.BASE_BUILD
TUNED_BUILD = tune.TUNED_BUILD
FIXED_BUILD = ROOT / "build" / "progjoin_fixedadv"
ORIG_ASSEMBLE = pj.assemble
ORIG_Z80 = pj.Z80

PROFILE = {}
CURRENT_LABELS = {}
WANT_HIST = {i: 0 for i in range(1, 7)}


def labelled_tuned_kernel(C, stepmap, desc, thresh, blocks, bodies):
    s = tune.tuned_kernel_source(C, stepmap, desc, thresh, blocks, bodies)
    reps = [
        (f"        ld de,(0x{pj.V_STEP:04x})          ; slot = STEPMAP[step + 2048]",
         f"disp_step:\n        ld de,(0x{pj.V_STEP:04x})          ; slot = STEPMAP[step + 2048]"),
        (f"        ld hl,(0x{pj.V_IQ:04x})            ; a = iq + 32",
         f"disp_phase:\n        ld hl,(0x{pj.V_IQ:04x})            ; a = iq + 32"),
        (f"        ld a,(0x{pj.V_SLOT:04x})           ; thresholds at THRESH + slot*8",
         f"disp_rank_addr:\n        ld a,(0x{pj.V_SLOT:04x})           ; thresholds at THRESH + slot*8"),
        ("        ld b,0                       ; rank = #thresholds <= u, sorted so",
         "disp_rank_scan:\n        ld b,0                       ; rank = #thresholds <= u, sorted so"),
        (f"        ld hl,(0x{pj.V_ACC:04x})           ; base = (a >> 7) & mask",
         f"disp_base:\n        ld hl,(0x{pj.V_ACC:04x})           ; base = (a >> 7) & mask"),
        (f"        ld a,(0x{pj.V_SLOT:04x})           ; desc idx = slot*32 + famc + (want-1)",
         f"disp_desc:\n        ld a,(0x{pj.V_SLOT:04x})           ; desc idx = slot*32 + famc + (want-1)"),
        (f"        ld a,(0x{pj.V_BASE:04x})           ; entry = base*16 + rank*2",
         f"disp_entry:\n        ld a,(0x{pj.V_BASE:04x})           ; entry = base*16 + rank*2"),
        (f"        ld e,(hl)\n        inc hl\n        ld d,(hl)\n        ld hl,0x{bodies:04x}",
         f"disp_body:\n        ld e,(hl)\n        inc hl\n        ld d,(hl)\n        ld hl,0x{bodies:04x}"),
        ("        ld a,(hl)                      ; selected count is body byte 0",
         "disp_join:\n        ld a,(hl)                      ; selected count is body byte 0"),
    ]
    for old, new in reps:
        if old not in s:
            raise RuntimeError(f"dispatch audit anchor changed: {old!r}")
        s = s.replace(old, new, 1)
    return s


def fixed_advance_kernel(C, stepmap, desc, thresh, blocks, bodies):
    if C != 6:
        raise RuntimeError("this finite advance experiment is deliberately C=6")
    s = labelled_tuned_kernel(C, stepmap, desc, thresh, blocks, bodies)
    old = f"""        ld a,(0x{pj.V_WANT:04x})           ; iq += want*step
        ld b,a
        ld hl,(0x{pj.V_IQ:04x})
        ld de,(0x{pj.V_STEP:04x})
adv_loop:
        add hl,de
        djnz adv_loop
        ld (0x{pj.V_IQ:04x}),hl
"""
    new = f"""        ld a,(0x{pj.V_WANT:04x})           ; iq += want*step, finite C=6
        ld hl,(0x{pj.V_IQ:04x})
        ld de,(0x{pj.V_STEP:04x})
        cp 6
        jr z,adv6
        cp 5
        jr z,adv5
        cp 4
        jr z,adv4
        cp 3
        jr z,adv3
        cp 2
        jr z,adv2
adv1:
        add hl,de
        jr adv_done
adv2:
        add hl,de
        add hl,de
        jr adv_done
adv3:
        add hl,de
        add hl,de
        add hl,de
        jr adv_done
adv4:
        add hl,de
        add hl,de
        add hl,de
        add hl,de
        jr adv_done
adv5:
        add hl,de
        add hl,de
        add hl,de
        add hl,de
        add hl,de
        jr adv_done
adv6:
        add hl,de
        add hl,de
        add hl,de
        add hl,de
        add hl,de
        add hl,de
adv_done:
        ld (0x{pj.V_IQ:04x}),hl
"""
    if old not in s:
        raise RuntimeError("chunk-advance baseline changed; refusing dishonest A/B")
    return s.replace(old, new, 1)


def capture_assemble(src, base=0):
    global CURRENT_LABELS
    code, labels = ORIG_ASSEMBLE(src, base)
    CURRENT_LABELS = labels
    return code, labels


STAGES = [
    ("want/min", "edge_loop", "disp_step"),
    ("step->slot", "disp_step", "disp_phase"),
    ("phase/u", "disp_phase", "disp_rank_addr"),
    ("threshold addr", "disp_rank_addr", "disp_rank_scan"),
    ("rank scan", "disp_rank_scan", "disp_base"),
    ("base extract", "disp_base", "disp_desc"),
    ("descriptor", "disp_desc", "disp_entry"),
    ("block entry", "disp_entry", "disp_body"),
    ("body pointer", "disp_body", "disp_join"),
    ("count/join", "disp_join", "wp_loop"),
]


class ProfiledZ80(ORIG_Z80):
    def _step(self):
        pc = self.pc
        t0 = self.t
        ORIG_Z80._step(self)
        d = self.t - t0
        L = CURRENT_LABELS
        if not L:
            return
        for nm, a, b in STAGES:
            if a in L and b in L and L[a] <= pc < L[b]:
                PROFILE[nm] = PROFILE.get(nm, 0) + d
                break


def count_wants(path: pathlib.Path):
    for line in (path / "progjoin_cases.txt").read_text().splitlines():
        f = line.split()
        if not f:
            continue
        ncs = int(f[7])
        for q in range(ncs):
            w = int(f[8 + q])
            WANT_HIST[w] = WANT_HIST.get(w, 0) + 1


def run_variant(build: pathlib.Path, kernel, acc, profile=False):
    pj.BUILD = build
    pj.kernel_source = kernel
    pj.assemble = capture_assemble if profile else ORIG_ASSEMBLE
    pj.Z80 = ProfiledZ80 if profile else ORIG_Z80
    try:
        pj.run_window(acc, quiet=True)
    finally:
        pj.BUILD = BASE_BUILD
        pj.kernel_source = tune.BASE_KERNEL
        pj.assemble = ORIG_ASSEMBLE
        pj.Z80 = ORIG_Z80


def main():
    nwin = int(sys.argv[1]) if len(sys.argv) > 1 else 63
    wpose = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    bake = ROOT / "build" / "edge_progjoin_bake"
    oracle = ROOT / "build" / "coverage_pose_oracle.txt"
    FIXED_BUILD.mkdir(parents=True, exist_ok=True)
    acc0, acc1 = {}, {}
    splits = [0]

    def do(start, npose):
        if npose < 1:
            return
        r = subprocess.run([str(bake), str(oracle), "6", str(npose),
                            str(BASE_BUILD), str(start)], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout); print(r.stderr)
            raise SystemExit(f"bake failed at pose {start}")
        try:
            tune.transform_tables(BASE_BUILD, TUNED_BUILD)
            count_wants(TUNED_BUILD)
            run_variant(TUNED_BUILD, labelled_tuned_kernel, acc0, profile=True)
            # The fixed-advance variant uses byte-identical tuned tables.
            run_variant(TUNED_BUILD, fixed_advance_kernel, acc1, profile=False)
        except pj.Overflow:
            if npose == 1:
                raise SystemExit(f"single pose overflows at {start}")
            # Undo histogram from the failed oversized window by rebuilding
            # it only in the successful children. Simplicity beats cleverness.
            for k in WANT_HIST:
                WANT_HIST[k] = 0
            # Re-run all preceding successful windows would be wrong, so fail
            # closed here; normal 40-pose windows are already known to split
            # only during run_window. Use recursive children before counting.
            h = npose // 2
            splits[0] += 1
            do(start, h)
            do(start + h, npose - h)

    # Adaptive overflow handling needs histogram accounting per successful
    # window. Use a local implementation that counts only after both runs fit.
    def safe_do(start, npose):
        if npose < 1:
            return
        r = subprocess.run([str(bake), str(oracle), "6", str(npose),
                            str(BASE_BUILD), str(start)], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout); print(r.stderr)
            raise SystemExit(f"bake failed at pose {start}")
        try:
            tune.transform_tables(BASE_BUILD, TUNED_BUILD)
            tmp0, tmp1 = {}, {}
            run_variant(TUNED_BUILD, labelled_tuned_kernel, tmp0, profile=True)
            run_variant(TUNED_BUILD, fixed_advance_kernel, tmp1, profile=False)
        except pj.Overflow:
            if npose == 1:
                raise SystemExit(f"single pose overflows at {start}")
            h = npose // 2
            splits[0] += 1
            safe_do(start, h)
            safe_do(start + h, npose - h)
            return
        count_wants(TUNED_BUILD)
        for src, dst in ((tmp0, acc0), (tmp1, acc1)):
            for k, v in src.items():
                if k in ("region", "blob"):
                    d = dst.setdefault(k, {})
                    for kk, vv in v.items():
                        if k == "blob":
                            d[kk] = max(d.get(kk, 0), vv)
                        else:
                            d[kk] = d.get(kk, 0) + vv
                elif k in ("code", "C"):
                    dst[k] = v
                elif k == "tables":
                    dst[k] = max(dst.get(k, 0), v)
                else:
                    dst[k] = dst.get(k, 0) + v

    for w in range(nwin):
        safe_do(w * wpose, wpose)

    exact = all(a.get(k, 0) == 0 for a in (acc0, acc1)
                for k in ("wrong_cells", "stray", "wrong_edges"))
    chunks = acc0["chunks"]
    print("=== PROGJOIN DISPATCH AUTOPSY + FINITE ADVANCE A/B ===")
    print(f"poses requested             {nwin*wpose:,} ({splits[0]} adaptive splits)")
    print(f"run-edges                   {acc0['edges']:,}")
    print(f"dispatches/chunks           {chunks:,}")
    print(f"cells played                {acc0['played']:,}")
    print(f"oracle verdict              {'BOTH EXACT' if exact else 'MISMATCH'}")

    print("\nDISPATCH SUB-STAGES, selected-count tuned kernel")
    prof_total = sum(PROFILE.values())
    for nm, _, _ in STAGES:
        v = PROFILE.get(nm, 0)
        print(f"  {nm:16} {v/max(chunks,1):8.1f} T/chunk  "
              f"{100.0*v/max(prof_total,1):5.1f}% of dispatch")
    broad = acc0.get("region", {}).get("dispatch", 0)
    print(f"  {'TOTAL':16} {prof_total/max(chunks,1):8.1f} T/chunk")
    print(f"  broad-region crosscheck  {broad/max(chunks,1):8.1f} T/chunk")
    if prof_total != broad:
        print(f"  *** attribution mismatch {prof_total-broad:+,} T ***")

    print("\nWANT DISTRIBUTION")
    for w in range(1, 7):
        n = WANT_HIST.get(w, 0)
        print(f"  want={w}: {n:7,}  {100.0*n/max(chunks,1):5.1f}%")

    t0, t1 = acc0["t"], acc1["t"]
    a0 = acc0.get("region", {}).get("chunk advance", 0)
    a1 = acc1.get("region", {}).get("chunk advance", 0)
    print("\nFINITE CHUNK-ADVANCE A/B")
    print(f"  kernel bytes              {acc0['code']} -> {acc1['code']}")
    print(f"  total edge-path cycles    {t0:,} -> {t1:,} T")
    print(f"  chunk advance             {a0/max(chunks,1):.1f} -> {a1/max(chunks,1):.1f} T/chunk")
    print(f"  saved                     {t0-t1:,} T over corpus "
          f"({100.0*(t0-t1)/max(t0,1):.2f}% edge path)")

    if not exact or prof_total != broad:
        return 1
    if t1 >= t0:
        print("\nVERDICT: PROFILE VALID; REJECT finite advance (not faster)")
        return 2
    print("\nVERDICT: KEEP finite advance; exact and faster. Dispatch profile is cycle-exact by PC.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
