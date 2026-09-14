#!/usr/bin/env python3
"""A/B hoisting PROGJOIN dispatch invariants from chunk to run-edge scope.

For a run-edge, family and step never change. Therefore these are invariant:
  * step -> slot
  * THRESH + slot*8
  * DESC + slot*64 + family*C*2

The tuned kernel currently reconstructs all three for every C-column chunk.
This experiment resolves them once before edge_loop and stores two 16-bit base
pointers. Per chunk, rank reads the saved threshold pointer and descriptor
selection only adds 2*(want-1) to the saved (slot,family) descriptor slab.

Tables/programs are byte-identical to the selected-count tuned A/B. Full corpus
correctness is checked by the existing PROGJOIN oracle.
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
V_THPTR = 0xBF18
V_DESCPTR = 0xBF1A


def hoisted_kernel(C, stepmap, desc, thresh, blocks, bodies):
    if C != 6:
        raise RuntimeError("edge-hoist experiment currently targets C=6")
    s = tune.tuned_kernel_source(C, stepmap, desc, thresh, blocks, bodies)

    # Append invariant resolution to existing per-edge setup, just before edge_loop.
    anchor = f"""        ld a,(hl)
        ld (0x{pj.V_MASK:04x}),a
edge_loop:
"""
    repl = f"""        ld a,(hl)
        ld (0x{pj.V_MASK:04x}),a

        ; step -> slot ONCE per run-edge
        ld de,(0x{pj.V_STEP:04x})
        ld hl,0x{stepmap + 2048:04x}
        add hl,de
        ld a,(hl)
        ld (0x{pj.V_SLOT:04x}),a

        ; threshold pointer = THRESH + slot*8, ONCE per run-edge
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,0x{thresh:04x}
        add hl,de
        ld (0x{V_THPTR:04x}),hl

        ; descriptor-family slab = DESC + slot*64 + family*C*2.
        ; DESC has 32 u16 entries (64 bytes) per slot; within a slot each
        ; family contributes C entries and only want varies per chunk.
        ld a,(0x{pj.V_SLOT:04x})
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl                         ; slot * 64 bytes
        ld a,(0x{pj.V_FAMC:04x})         ; family*C entries
        add a,a                           ; -> bytes
        ld e,a
        ld d,0
        add hl,de
        ld de,0x{desc:04x}
        add hl,de
        ld (0x{V_DESCPTR:04x}),hl
edge_loop:
"""
    if anchor not in s:
        raise RuntimeError("per-edge setup anchor changed")
    s = s.replace(anchor, repl, 1)

    old_slot = f"""        ld de,(0x{pj.V_STEP:04x})          ; slot = STEPMAP[step + 2048]
        ld hl,0x{stepmap + 2048:04x}
        add hl,de
        ld a,(hl)
        ld (0x{pj.V_SLOT:04x}),a
"""
    if old_slot not in s:
        raise RuntimeError("slot chunk sequence changed")
    s = s.replace(old_slot, "", 1)

    old_thr = f"""        ld a,(0x{pj.V_SLOT:04x})           ; thresholds at THRESH + slot*8
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,0x{thresh:04x}
        add hl,de
"""
    new_thr = f"""        ld hl,(0x{V_THPTR:04x})           ; hoisted THRESH + slot*8
"""
    if old_thr not in s:
        raise RuntimeError("threshold address sequence changed")
    s = s.replace(old_thr, new_thr, 1)

    old_desc = f"""        ld a,(0x{pj.V_SLOT:04x})           ; desc idx = slot*32 + famc + (want-1)
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld a,(0x{pj.V_FAMC:04x})
        ld e,a
        ld d,0
        add hl,de
        ld a,(0x{pj.V_WANT:04x})
        dec a
        ld e,a
        ld d,0
        add hl,de
        add hl,hl
        ld de,0x{desc:04x}
        add hl,de
"""
    new_desc = f"""        ld hl,(0x{V_DESCPTR:04x})         ; hoisted slot+family slab
        ld a,(0x{pj.V_WANT:04x})
        dec a
        add a,a                           ; 2-byte descriptor entries
        ld e,a
        ld d,0
        add hl,de
"""
    if old_desc not in s:
        raise RuntimeError("descriptor sequence changed")
    s = s.replace(old_desc, new_desc, 1)
    return s


def merge(src, dst):
    for k, v in src.items():
        if k in ("region", "blob"):
            d = dst.setdefault(k, {})
            for kk, vv in v.items():
                if k == "blob": d[kk] = max(d.get(kk, 0), vv)
                else: d[kk] = d.get(kk, 0) + vv
        elif k in ("code", "C"):
            dst[k] = v
        elif k == "tables":
            dst[k] = max(dst.get(k, 0), v)
        else:
            dst[k] = dst.get(k, 0) + v


def run_variant(kernel, acc):
    pj.BUILD = TUNED_BUILD
    pj.kernel_source = kernel
    try:
        tmp = {}
        pj.run_window(tmp, quiet=True)
    finally:
        pj.BUILD = BASE_BUILD
        pj.kernel_source = tune.BASE_KERNEL
    merge(tmp, acc)


def main():
    nwin = int(sys.argv[1]) if len(sys.argv) > 1 else 63
    wpose = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    bake = ROOT / "build" / "edge_progjoin_bake"
    oracle = ROOT / "build" / "coverage_pose_oracle.txt"
    acc0, acc1 = {}, {}
    splits = [0]

    def do(start, npose):
        if npose < 1: return
        r = subprocess.run([str(bake), str(oracle), "6", str(npose),
                            str(BASE_BUILD), str(start)], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout); print(r.stderr)
            raise SystemExit(f"bake failed at pose {start}")
        try:
            tune.transform_tables(BASE_BUILD, TUNED_BUILD)
            t0, t1 = {}, {}
            pj.BUILD = TUNED_BUILD
            pj.kernel_source = tune.tuned_kernel_source
            pj.run_window(t0, quiet=True)
            pj.kernel_source = hoisted_kernel
            pj.run_window(t1, quiet=True)
        except pj.Overflow:
            if npose == 1: raise SystemExit(f"single pose overflow at {start}")
            h = npose // 2; splits[0] += 1
            do(start, h); do(start+h, npose-h); return
        finally:
            pj.BUILD = BASE_BUILD; pj.kernel_source = tune.BASE_KERNEL
        merge(t0, acc0); merge(t1, acc1)

    for w in range(nwin): do(w*wpose, wpose)

    exact = all(a.get(k,0) == 0 for a in (acc0,acc1)
                for k in ("wrong_cells","stray","wrong_edges"))
    chunks, edges = acc0["chunks"], acc0["edges"]
    print("=== PROGJOIN RUN-EDGE INVARIANT HOIST A/B ===")
    print(f"poses requested             {nwin*wpose:,} ({splits[0]} adaptive splits)")
    print(f"run-edges                   {edges:,}")
    print(f"chunks                      {chunks:,} ({chunks/max(edges,1):.3f}/edge)")
    print(f"oracle verdict              {'BOTH EXACT' if exact else 'MISMATCH'}")
    print("\nCYCLES")
    print(f"  tuned control total       {acc0['t']:,} T")
    print(f"  edge-hoisted total        {acc1['t']:,} T")
    print(f"  saved                     {acc0['t']-acc1['t']:,} T "
          f"({100.0*(acc0['t']-acc1['t'])/max(acc0['t'],1):.2f}% edge path)")
    print(f"  per run-edge              {acc0['t']/edges:.1f} -> {acc1['t']/edges:.1f} T")
    for nm in ("per-edge setup","dispatch","playback","chunk advance"):
        v0=acc0.get("region",{}).get(nm,0); v1=acc1.get("region",{}).get(nm,0)
        den = edges if nm=="per-edge setup" else (acc0["played"] if nm=="playback" else chunks)
        den1 = edges if nm=="per-edge setup" else (acc1["played"] if nm=="playback" else chunks)
        print(f"  {nm:16} {v0/max(den,1):8.1f} -> {v1/max(den1,1):8.1f} T/unit")
    print("\nSHAPE")
    print(f"  kernel bytes              {acc0['code']} -> {acc1['code']}")
    print("  emitted tables            byte-identical")
    if not exact: return 1
    if acc1['t'] >= acc0['t']:
        print("\nVERDICT: REJECT - exact but not faster"); return 2
    print("\nVERDICT: KEEP - resolve slot/threshold/descriptor family once per run-edge")
    return 0

if __name__ == "__main__": raise SystemExit(main())
