#!/usr/bin/env python3
"""A/B the existing PROGJOIN kernel against a tuned body layout.

The baseline baker and kernel are deliberately left untouched.  For each baked
window this harness derives a second, equivalent table set from the emitted
bytes:

    old body: [C+1 prefix counts][4-byte cell records...]
    new body: [selected cell count][4-byte cell records...]

`want` is already part of the descriptor key, so once dispatch has selected a
body the C+1 prefix header is redundant.  The tuned dispatch entry still stays
a 16-bit body offset: no 4-byte entry explosion, and the dispatch ROM does not
double.  Both kernels execute the exact same corpus cases and are checked by
the existing oracle logic in z80_progjoin_bench.py.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys

import z80_progjoin_bench as pj

ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE_KERNEL = pj.kernel_source
BASE_BUILD = pj.BUILD
TUNED_BUILD = ROOT / "build" / "progjoin_tuned"


def read_manifest(path: pathlib.Path):
    m = {}
    for line in (path / "progjoin_manifest.txt").read_text().splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == "M":
            m.setdefault("M", {})[int(f[1])] = int(f[2])
        else:
            m[f[0]] = int(f[1])
    return m


def active_descriptors(src: pathlib.Path, stepmap: bytes):
    """Return (slot,fam,want) descriptors actually reached by this window."""
    active = set()
    for line in (src / "progjoin_cases.txt").read_text().splitlines():
        f = line.split()
        if not f:
            continue
        fam = int(f[0]); step = int(f[1]); ncs = int(f[7])
        slot = stepmap[step + 2048]
        assert slot != 0xFF
        for q in range(ncs):
            active.add((slot, fam, int(f[8 + q])))
    return active


def transform_tables(src: pathlib.Path, dst: pathlib.Path):
    """Derive count-at-body-head tables without changing the original bake."""
    m = read_manifest(src)
    C, nslots = m["C"], m["NSLOTS"]
    stepmap = (src / "progjoin_stepmap.bin").read_bytes()
    desc = (src / "progjoin_desc.bin").read_bytes()
    old_blocks = (src / "progjoin_blocks.bin").read_bytes()
    old_bodies = (src / "progjoin_bodies.bin").read_bytes()
    active = active_descriptors(src, stepmap)

    new_blocks = bytearray(old_blocks)
    new_bodies = bytearray()
    intern = {}

    # Every active descriptor owns a serialized block.  Blocks are not deduped
    # by the baker, so rewriting its entries cannot contaminate another want.
    for slot, fam, want in sorted(active):
        assert 0 <= slot < nslots and 1 <= want <= C
        di = (slot * 32 + fam * C + (want - 1)) * 2
        block_base = desc[di] | (desc[di + 1] << 8)
        M = m["M"][fam]
        for ent in range(M * 8):
            p = block_base + ent * 2
            oldoff = old_blocks[p] | (old_blocks[p + 1] << 8)
            if oldoff == 0xFFFF:
                continue
            assert oldoff + C + 1 <= len(old_bodies)
            count = old_bodies[oldoff + want]
            payload0 = oldoff + C + 1
            payload1 = payload0 + 4 * count
            assert payload1 <= len(old_bodies)
            rec = bytes([count]) + old_bodies[payload0:payload1]
            newoff = intern.get(rec)
            if newoff is None:
                newoff = len(new_bodies)
                if newoff > 0xFFFE:
                    raise pj.Overflow(newoff)
                intern[rec] = newoff
                new_bodies.extend(rec)
            new_blocks[p] = newoff & 0xFF
            new_blocks[p + 1] = (newoff >> 8) & 0xFF

    dst.mkdir(parents=True, exist_ok=True)
    for nm in ("stepmap", "desc", "thresh"):
        shutil.copyfile(src / f"progjoin_{nm}.bin", dst / f"progjoin_{nm}.bin")
    shutil.copyfile(src / "progjoin_cases.txt", dst / "progjoin_cases.txt")
    (dst / "progjoin_blocks.bin").write_bytes(new_blocks)
    (dst / "progjoin_bodies.bin").write_bytes(new_bodies)

    lines = []
    for line in (src / "progjoin_manifest.txt").read_text().splitlines():
        if line.startswith("bodies "):
            lines.append(f"bodies {len(new_bodies)}")
        else:
            lines.append(line)
    (dst / "progjoin_manifest.txt").write_text("\n".join(lines) + "\n")
    return len(old_bodies), len(new_bodies), len(intern)


def tuned_kernel_source(C, stepmap, desc, thresh, blocks, bodies):
    """Baseline kernel with only the redundant body-prefix chase removed."""
    s = BASE_KERNEL(C, stepmap, desc, thresh, blocks, bodies)
    old = f"""        ld (0x{pj.V_BODY:04x}),hl
        ld a,(0x{pj.V_WANT:04x})           ; count = body[want] (prefix sums)
        ld e,a
        ld d,0
        add hl,de
        ld a,(hl)
        ld (0x{pj.V_COUNT:04x}),a
        or a
        jr z,chunk_done
        ld hl,(0x{pj.V_BODY:04x})          ; program starts after the C+1 header
        ld de,{C + 1}
        add hl,de
        ld sp,hl                     ; <<< the join
"""
    new = """        ld a,(hl)                      ; selected count is body byte 0
        inc hl                         ; payload follows immediately
        or a
        jr z,chunk_done
        ld sp,hl                       ; <<< tuned join: no pointer round-trip
"""
    if old not in s:
        raise RuntimeError("baseline body-dispatch sequence changed; refusing a dishonest A/B")
    return s.replace(old, new, 1)


def run_one(acc_base, acc_tuned):
    # Baseline first, from the baker's exact output.
    pj.BUILD = BASE_BUILD
    pj.kernel_source = BASE_KERNEL
    pj.run_window(acc_base, quiet=True)

    old_b, new_b, nprog = transform_tables(BASE_BUILD, TUNED_BUILD)

    # Tuned run over the same cases, with only blocks/bodies transformed.
    pj.BUILD = TUNED_BUILD
    pj.kernel_source = tuned_kernel_source
    try:
        pj.run_window(acc_tuned, quiet=True)
    finally:
        pj.BUILD = BASE_BUILD
        pj.kernel_source = BASE_KERNEL
    return old_b, new_b, nprog


def metric(acc, region, denom):
    return acc.get("region", {}).get(region, 0) / max(acc.get(denom, 0), 1)


def main():
    nwin = int(sys.argv[1]) if len(sys.argv) > 1 else 63
    wpose = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    bake = ROOT / "build" / "edge_progjoin_bake"
    oracle = ROOT / "build" / "coverage_pose_oracle.txt"
    acc0, acc1 = {}, {}
    splits = [0]
    body_old_max = body_new_max = nprog_max = 0

    def do(start, npose):
        nonlocal body_old_max, body_new_max, nprog_max
        if npose < 1:
            return
        r = subprocess.run([str(bake), str(oracle), "6", str(npose),
                            str(BASE_BUILD), str(start)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout); print(r.stderr)
            raise SystemExit(f"bake failed at pose {start}")
        try:
            ob, nb, np = run_one(acc0, acc1)
        except pj.Overflow:
            if npose == 1:
                raise SystemExit(f"a single pose overflows at {start}")
            h = npose // 2
            splits[0] += 1
            do(start, h)
            do(start + h, npose - h)
            return
        body_old_max = max(body_old_max, ob)
        body_new_max = max(body_new_max, nb)
        nprog_max = max(nprog_max, np)

    try:
        for w in range(nwin):
            do(w * wpose, wpose)
    finally:
        pj.BUILD = BASE_BUILD
        pj.kernel_source = BASE_KERNEL

    print("=== PROGJOIN TUNE A/B: redundant prefix header -> selected count byte ===")
    print(f"poses requested             {nwin*wpose:,} ({splits[0]} adaptive splits)")
    print(f"run-edges, baseline/tuned   {acc0['edges']:,} / {acc1['edges']:,}")
    print(f"dispatches                  {acc0['chunks']:,} / {acc1['chunks']:,}")
    print(f"cells played                {acc0['played']:,} / {acc1['played']:,}")
    print(f"wrong cells                 {acc0['wrong_cells']} / {acc1['wrong_cells']}")
    print(f"stray writes                {acc0['stray']} / {acc1['stray']}")
    print(f"faulted run-edges           {acc0['wrong_edges']} / {acc1['wrong_edges']}")
    exact = all(acc[k] == 0 for acc in (acc0, acc1)
                for k in ("wrong_cells", "stray", "wrong_edges"))
    print(f"oracle verdict              {'BOTH EXACT' if exact else 'MISMATCH'}")

    t0, t1 = acc0["t"], acc1["t"]
    save = t0 - t1
    print("\nCYCLES")
    print(f"  baseline total            {t0:,} T")
    print(f"  tuned total               {t1:,} T")
    print(f"  saved                     {save:,} T  ({100.0*save/max(t0,1):.2f}%)")
    print(f"  per run-edge              {t0/max(acc0['edges'],1):.1f} -> "
          f"{t1/max(acc1['edges'],1):.1f} T")
    print(f"  dispatch per chunk        {metric(acc0,'dispatch','chunks'):.1f} -> "
          f"{metric(acc1,'dispatch','chunks'):.1f} T")
    print(f"  playback per cell         {metric(acc0,'playback','played'):.1f} -> "
          f"{metric(acc1,'playback','played'):.1f} T")
    print(f"  chunk advance             {metric(acc0,'chunk advance','chunks'):.1f} -> "
          f"{metric(acc1,'chunk advance','chunks'):.1f} T")

    print("\nKERNEL / TABLE SHAPE")
    print(f"  kernel bytes              {acc0['code']} -> {acc1['code']}")
    print(f"  largest-window bodies     {body_old_max:,} -> {body_new_max:,} bytes")
    print(f"  largest-window programs   {nprog_max:,}")
    print("  block entries             unchanged: 2 bytes each")
    print("  desc/step/threshold ROM   unchanged")

    # This rung is useful only if the tuned path is exact and genuinely faster.
    if not exact:
        return 1
    if t1 >= t0:
        print("\nVERDICT: REJECT - exact, but no cycle win")
        return 2
    print("\nVERDICT: KEEP - exact and faster; prefix header is redundant once want is keyed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
