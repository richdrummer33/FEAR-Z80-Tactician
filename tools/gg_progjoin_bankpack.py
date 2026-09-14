#!/usr/bin/env python3
"""Repack emitted PROGJOIN tables into a Game-Gear-friendly FULL-wall bank plan.

Input is the exact byte vocabulary emitted by edge_progjoin_bake.c.  This tool
DOES NOT invent another renderer model.  It narrows the corpus to FULL-wall
families (top=0, bottom=2), keeps the original step/threshold selector data,
compacts only descriptors/blocks/bodies reachable by observed FULL run-edges,
and lays variable program bodies out so no body crosses a 16 KiB ROM bank.

The result is an integration artifact, not yet an executor.  Its purpose is to
turn the flat-address research tables into a deterministic bank contract before
assembly work begins.
"""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

BANK = 16 * 1024
FULL_FAMS = {0, 2}


def u16(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8)


def p16(v: int) -> bytes:
    return struct.pack('<H', v)


def s16(v: int) -> int:
    """Exactly the two's-complement int16 wrap used by the C baker."""
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def load_manifest(path: Path) -> dict:
    out: dict[str, object] = {"M": {}}
    for ln in path.read_text().splitlines():
        f = ln.split()
        if not f:
            continue
        if f[0] == "M":
            out["M"][int(f[1])] = int(f[2])
        elif len(f) == 2:
            try:
                out[f[0]] = int(f[1])
            except ValueError:
                out[f[0]] = f[1]
    return out


def parse_cases(path: Path):
    for ln in path.read_text().splitlines():
        if not ln.strip():
            continue
        f = [int(x) for x in ln.split()]
        i = 0
        fam, step, iq0, c0, ncol, shade = f[i:i+6]; i += 6
        first_dest = f[i]; i += 1
        ncs = f[i]; i += 1
        wants = f[i:i+ncs]; i += ncs
        ncell = f[i]; i += 1
        expect = f[i:i+2*ncell]
        yield {
            "fam": fam, "step": step, "iq0": iq0, "c0": c0,
            "ncol": ncol, "shade": shade, "first_dest": first_dest,
            "ncs": ncs, "wants": wants, "ncell": ncell, "expect": expect,
        }


def align_body(buf: bytearray, body: bytes) -> int:
    within = len(buf) & (BANK - 1)
    if within + len(body) > BANK:
        buf.extend(b'\x00' * (BANK - within))
    off = len(buf)
    if off + len(body) > 0x10000:
        raise SystemExit("FULL body pack exceeds 64 KiB logical window")
    buf.extend(body)
    return off


def align_block(buf: bytearray, block: bytes) -> int:
    within = len(buf) & (BANK - 1)
    if within + len(block) > BANK:
        buf.extend(b'\xFF' * (BANK - within))
    off = len(buf)
    if off + len(block) > 0x10000:
        raise SystemExit("FULL block pack exceeds 64 KiB logical window")
    buf.extend(block)
    return off


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("emit_dir", type=Path)
    ap.add_argument("out_dir", type=Path)
    args = ap.parse_args()
    src, out = args.emit_dir, args.out_dir
    out.mkdir(parents=True, exist_ok=True)

    man = load_manifest(src / "progjoin_manifest.txt")
    C = int(man["C"]); nslots = int(man["NSLOTS"])
    if C != 6:
        raise SystemExit(f"bank pack currently pins target C=6, got {C}")

    stepmap = (src / "progjoin_stepmap.bin").read_bytes()
    desc = (src / "progjoin_desc.bin").read_bytes()
    thresh = (src / "progjoin_thresh.bin").read_bytes()
    blocks = (src / "progjoin_blocks.bin").read_bytes()
    bodies = (src / "progjoin_bodies.bin").read_bytes()
    cases = list(parse_cases(src / "progjoin_cases.txt"))

    if len(stepmap) != 4096:
        raise SystemExit(f"bad stepmap size {len(stepmap)}")
    if len(desc) != nslots * 32 * 2:
        raise SystemExit(f"bad descriptor size {len(desc)} for {nslots} slots")
    if len(thresh) != nslots * 8:
        raise SystemExit(f"bad threshold size {len(thresh)} for {nslots} slots")

    # Reachability comes from real emitted cases, avoiding the ambiguity that a
    # zero descriptor can mean either an unused calloc entry or a real block at
    # offset zero.
    used_desc: set[tuple[int,int,int]] = set()
    full_cases = 0
    for c in cases:
        if c["fam"] not in FULL_FAMS:
            continue
        full_cases += 1
        si = c["step"] + 2048
        if not (0 <= si < 4096) or stepmap[si] == 0xFF:
            raise SystemExit(f"FULL case has unmapped step {c['step']}")
        slot = stepmap[si]
        for want in c["wants"]:
            used_desc.add((slot, c["fam"], want - 1))

    # Collect body references from the reachable FULL blocks.  Body length is
    # encoded by the original C+1-byte prefix header: prefix[want] cells.
    body_need: dict[int, int] = {}
    block_specs: list[tuple[tuple[int,int,int], int, int]] = []
    M = man["M"]
    for key in sorted(used_desc):
        slot, fam, wl = key
        di = ((slot * 32) + fam * C + wl) * 2
        old_bo = u16(desc, di)
        m = int(M[fam])
        blen = m * 8 * 2
        if old_bo + blen > len(blocks):
            raise SystemExit(f"block OOB for {key}: {old_bo}+{blen}>{len(blocks)}")
        block_specs.append((key, old_bo, blen))
        want = wl + 1
        for p in range(old_bo, old_bo + blen, 2):
            old_body = u16(blocks, p)
            if old_body == 0xFFFF:
                continue
            if old_body + C + 1 > len(bodies):
                raise SystemExit(f"body header OOB at {old_body}")
            cells = bodies[old_body + want]
            need = C + 1 + 4 * cells
            if old_body + need > len(bodies):
                raise SystemExit(f"body OOB at {old_body}, need {need}")
            prev = body_need.get(old_body, 0)
            body_need[old_body] = max(prev, need)

    # Repack bodies first.  Identical original offsets remain shared; every body
    # is wholly contained in one 16 KiB page, so runtime needs one bank select
    # per program body, not an in-body boundary path.
    new_bodies = bytearray()
    body_map: dict[int, int] = {}
    for old in sorted(body_need):
        blob = bodies[old:old + body_need[old]]
        body_map[old] = align_body(new_bodies, blob)

    # Compact reachable blocks and rewrite their body pointers to packed logical
    # offsets.  Descriptor table keeps the research index shape but unused GG
    # entries are explicit 0xFFFF rather than ambiguous zeroes.
    new_blocks = bytearray()
    new_desc = bytearray(b'\xFF' * len(desc))
    block_map: dict[tuple[int,int,int], int] = {}
    for key, old_bo, blen in block_specs:
        bb = bytearray(blocks[old_bo:old_bo + blen])
        for p in range(0, len(bb), 2):
            old_body = u16(bb, p)
            if old_body == 0xFFFF:
                continue
            if old_body not in body_map:
                raise SystemExit(f"internal body reachability miss {old_body}")
            bb[p:p+2] = p16(body_map[old_body])
        new_bo = align_block(new_blocks, bytes(bb))
        block_map[key] = new_bo
        slot, fam, wl = key
        di = ((slot * 32) + fam * C + wl) * 2
        new_desc[di:di+2] = p16(new_bo)

    # Structural re-validation of every FULL case against the packed selector.
    # Recompute each chunk's iq from the original run input exactly as the C
    # baker does: int16(iq0 + int16(cs*step)).  Do not let Python's unbounded
    # integer arithmetic accidentally define a different selector at wrap.
    checked_chunks = 0
    for c in cases:
        fam = c["fam"]
        if fam not in FULL_FAMS:
            continue
        slot = stepmap[c["step"] + 2048]
        m = int(M[fam])
        cs = 0
        for want in c["wants"]:
            wl = want - 1
            di = ((slot * 32) + fam * C + wl) * 2
            bo = u16(new_desc, di)
            if bo == 0xFFFF:
                raise SystemExit(f"packed descriptor missing {(slot,fam,wl)}")
            iq = s16(c["iq0"] + s16(cs * c["step"]))
            a = iq + 32
            H = a >> 7
            u = a & 127
            ts = thresh[slot*8:slot*8 + C + 1]
            # Exact rank_of(): count sorted thresholds already crossed.
            rank = sum(1 for t in ts if u >= t)
            base = H & (m - 1)
            pi = bo + (base * 8 + rank) * 2
            if pi + 2 > len(new_blocks):
                raise SystemExit(f"packed block pointer OOB at {pi}")
            body = u16(new_blocks, pi)
            if body == 0xFFFF:
                raise SystemExit(f"reachable packed body missing fam={fam} step={c['step']} want={want}")
            if body + C + 1 > len(new_bodies):
                raise SystemExit(f"packed body header OOB {body}")
            cells = new_bodies[body + want]
            need = C + 1 + 4*cells
            if (body // BANK) != ((body + need - 1) // BANK):
                raise SystemExit(f"body crosses bank at {body} len={need}")
            cs += C
            checked_chunks += 1

    (out / "progjoin_stepmap.bin").write_bytes(stepmap)
    (out / "progjoin_thresh.bin").write_bytes(thresh)
    (out / "progjoin_desc_full.bin").write_bytes(new_desc)
    (out / "progjoin_blocks_full.bin").write_bytes(new_blocks)
    (out / "progjoin_bodies_full.bin").write_bytes(new_bodies)

    report = {
        "format": "GG_PROGJOIN_FULL_BANKPACK_V1",
        "C": C,
        "bank_bytes": BANK,
        "full_families": sorted(FULL_FAMS),
        "full_cases": full_cases,
        "checked_chunks": checked_chunks,
        "used_descriptors": len(used_desc),
        "unique_bodies": len(body_map),
        "sizes": {
            "stepmap": len(stepmap), "thresholds": len(thresh),
            "descriptors": len(new_desc), "blocks": len(new_blocks),
            "bodies": len(new_bodies),
        },
        "banks": {
            "blocks": (len(new_blocks) + BANK - 1) // BANK,
            "bodies": (len(new_bodies) + BANK - 1) // BANK,
        },
        "padding": {
            "blocks": len(new_blocks) - sum(x[2] for x in block_specs),
            "bodies": len(new_bodies) - sum(body_need.values()),
        },
        "contract": [
            "selector vocabulary is emitted by edge_progjoin_bake.c",
            "only observed FULL top/bottom descriptors are retained",
            "unused descriptors are 0xFFFF",
            "no packed program body crosses a 16 KiB bank",
            "16-bit body offsets remain sufficient; runtime bank = offset >> 14",
            "selector int16 wrapping matches edge_progjoin_bake.c exactly"
        ],
    }
    (out / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")

    print("=== GG FULL RUN-EDGE BANK PACK ===")
    print(f"FULL cases/chunks       {full_cases:,} / {checked_chunks:,}")
    print(f"descriptors retained    {len(used_desc):,}")
    print(f"unique program bodies   {len(body_map):,}")
    for k, v in report["sizes"].items():
        print(f"{k:20s} {v:8,d} bytes")
    print(f"blocks/body banks       {report['banks']['blocks']} / {report['banks']['bodies']}")
    print(f"bank padding            {report['padding']['blocks']:,} / {report['padding']['bodies']:,} bytes")
    print("BANKPACK_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
