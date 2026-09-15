#!/usr/bin/env python3
"""Does the PROGJOIN body vocabulary generalize to unseen poses, or only its keys?

Both the renderer stocktake and its amendment assume the compiled *bodies* are
generic raster programs and that only the dispatch *key* is pose-trained. That
assumption decides whether canonical keying is worth building: if the bodies
generalize, a pose-independent key recovers the coverage; if they do not, no
keying scheme can, and the vocabulary itself is the problem.

This measures it. Poses are split into disjoint train and test sets, both are
baked through the exact same windowed baker the corpus census uses, and every
FULL chunk is resolved to its byte-exact serialized body. Two coverages are then
reported for the held-out poses:

  key coverage   test chunks whose (step, fam, want, base, rank) was baked
  body coverage  test chunks whose serialized body was baked, under ANY key

key coverage is what the shipped dispatcher achieves. body coverage is the
ceiling a perfect pose-independent key could reach without adding one byte of
ROM. The gap between them is the prize, and it is the number that decides
whether to build canonical keying at all.

Splits:
  yaw-parity   train on even headings, test on odd. The most favourable case:
               every test pose is one heading unit from a trained one.
  yaw-shipped  train on headings 0,16,32,... (exactly what ships), test on the
               other 15/16. Measures how much of the observed live coverage
               failure is keying rather than vocabulary.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

C = 6
FULL_FAMS = {0, 2}
FAM_M = {0: 8, 1: 8, 2: 8, 3: 16, 4: 32}


def u16(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8)


def s16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def parse_cases(path: Path):
    for ln in path.read_text().splitlines():
        if not ln.strip():
            continue
        f = [int(x) for x in ln.split()]
        i = 0
        fam, step, iq0, c0, ncol, shade = f[i:i + 6]; i += 6
        i += 1                      # first_dest
        ncs = f[i]; i += 1
        wants = f[i:i + ncs]; i += ncs
        yield dict(fam=fam, step=step, iq0=iq0, ncol=ncol, wants=wants)


def resolve(oracle: Path, baker: Path, work: Path, window: int):
    """Yield (key, body_bytes) for every FULL chunk in this oracle."""
    npose_total = sum(1 for ln in oracle.read_text().splitlines() if ln.strip())
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    for start in range(0, npose_total, window):
        npose = min(window, npose_total - start)
        wd = work / f"w{start:06d}"
        wd.mkdir()
        p = subprocess.run([str(baker), str(oracle), str(C), str(npose), str(wd), str(start)],
                           text=True, capture_output=True)
        if p.returncode:
            sys.exit(f"baker failed at pose {start}: {p.stderr[:400]}")
        stepmap = (wd / "progjoin_stepmap.bin").read_bytes()
        desc = (wd / "progjoin_desc.bin").read_bytes()
        thresh = (wd / "progjoin_thresh.bin").read_bytes()
        blocks = (wd / "progjoin_blocks.bin").read_bytes()
        bodies = (wd / "progjoin_bodies.bin").read_bytes()
        for case in parse_cases(wd / "progjoin_cases.txt"):
            fam = case["fam"]
            if fam not in FULL_FAMS:
                continue
            step = case["step"]
            si = step + 2048
            if not 0 <= si < 4096 or stepmap[si] == 0xFF:
                continue
            slot = stepmap[si]
            M = FAM_M[fam]
            cs = 0
            for want in case["wants"]:
                iq = s16(case["iq0"] + s16(cs * step))
                a = iq + 32
                H, u = a >> 7, a & 127
                ts = thresh[slot * 8:slot * 8 + C + 1]
                rank = sum(1 for t in ts if u >= t)
                base = H & (M - 1)
                di = ((slot * 32) + fam * C + (want - 1)) * 2
                cs += C
                if di + 2 > len(desc):
                    continue
                bo = u16(desc, di)
                pi = bo + (base * 8 + rank) * 2
                if pi + 2 > len(blocks):
                    continue
                body = u16(blocks, pi)
                if body == 0xFFFF or body + C + 1 > len(bodies):
                    continue
                ncell = bodies[body + want]
                blen = C + 1 + 4 * ncell
                if body + blen > len(bodies):
                    continue
                yield (step, fam, want, base, rank), bytes(bodies[body:body + blen])
        shutil.rmtree(wd)


def split_oracle(src: Path, out_dir: Path, mode: str, yaw_period: int, positions_stride: int):
    """Write train/test oracles. Line order is (gy, gx, sub_offset, yaw) with yaw
    innermost, so line_index % yaw_period recovers the heading index."""
    lines = [ln for ln in src.read_text().splitlines() if ln.strip()]
    train, test = [], []
    for i, ln in enumerate(lines):
        pos_i, yaw_i = divmod(i, yaw_period)
        if pos_i % positions_stride:
            continue
        if mode == "yaw-parity":
            (train if yaw_i % 2 == 0 else test).append(ln)
        elif mode == "yaw-shipped":
            (train if yaw_i % 16 == 0 else test).append(ln)
        else:
            sys.exit(f"unknown split {mode}")
    out_dir.mkdir(parents=True, exist_ok=True)
    tr, te = out_dir / "train.txt", out_dir / "test.txt"
    tr.write_text("\n".join(train) + "\n")
    te.write_text("\n".join(test) + "\n")
    return tr, te, len(train), len(test)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("oracle", type=Path, help="oracle with every heading (yaw_step=1, stride=1)")
    ap.add_argument("baker", type=Path)
    ap.add_argument("work", type=Path)
    ap.add_argument("--window", type=int, default=40)
    ap.add_argument("--split", default="yaw-shipped", choices=["yaw-parity", "yaw-shipped"])
    ap.add_argument("--yaw-period", type=int, default=256)
    ap.add_argument("--positions-stride", type=int, default=16,
                    help="keep every Nth position, with its complete heading sweep")
    args = ap.parse_args()

    tr, te, ntr, nte = split_oracle(args.oracle, args.work, args.split,
                                    args.yaw_period, args.positions_stride)
    print(f"split {args.split}: train={ntr:,} poses  test={nte:,} poses "
          f"(every {args.positions_stride}th position, full heading sweep)")

    train_keys, train_bodies = set(), set()
    ntrain_chunks = 0
    for key, body in resolve(tr, args.baker, args.work / "wtrain", args.window):
        train_keys.add(key); train_bodies.add(body); ntrain_chunks += 1
    print(f"train: {ntrain_chunks:,} chunks -> {len(train_keys):,} keys, {len(train_bodies):,} unique bodies")

    tot = key_hit = body_hit = 0
    missed_bodies = Counter()
    for key, body in resolve(te, args.baker, args.work / "wtest", args.window):
        tot += 1
        if key in train_keys:
            key_hit += 1
        if body in train_bodies:
            body_hit += 1
        else:
            missed_bodies[body] += 1
    if not tot:
        sys.exit("no test chunks resolved")

    print(f"test : {tot:,} chunks")
    print()
    print(f"  key coverage   {key_hit:>8,} / {tot:,} = {100.0*key_hit/tot:5.1f}%   "
          f"(what the shipped dispatcher can reach)")
    print(f"  body coverage  {body_hit:>8,} / {tot:,} = {100.0*body_hit/tot:5.1f}%   "
          f"(ceiling for a perfect pose-independent key)")
    print(f"  recoverable    {100.0*(body_hit-key_hit)/tot:5.1f} points, "
          f"{len(missed_bodies):,} genuinely new bodies "
          f"({sum(missed_bodies.values()):,} chunks)")
    print()
    if body_hit > key_hit * 1.2:
        print("  VERDICT  the body vocabulary generalizes well beyond its keys; canonical")
        print("           keying is the right investment and this is the size of the prize")
    elif body_hit > key_hit:
        print("  VERDICT  bodies generalize only modestly beyond keys; canonical keying")
        print("           helps but will not on its own reach the coverage target")
    else:
        print("  VERDICT  bodies are as pose-specific as their keys; canonical keying")
        print("           CANNOT recover coverage and the vocabulary itself must change")
    return 0


if __name__ == "__main__":
    sys.exit(main())
