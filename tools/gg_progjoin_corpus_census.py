#!/usr/bin/env python3
"""Union the windowed PROGJOIN target into one cartridge vocabulary census.

The Z80 harness deliberately bakes small windows because its executable test has
a flat 64 KiB address space.  A cartridge does not get to assume one window.
This tool runs the exact baker over the entire pose oracle in bounded windows,
resolves every observed FULL-wall dispatch through each window's emitted tables,
and unions semantic selector entries and body bytes across windows.

No GG executor format is chosen here.  The output answers the prerequisite
question: how large is the *whole* observed FULL-wall vocabulary, and do any
semantic keys conflict across windows?
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
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
        fam, step, iq0, c0, ncol, shade = f[i:i+6]; i += 6
        first_dest = f[i]; i += 1
        ncs = f[i]; i += 1
        wants = f[i:i+ncs]; i += ncs
        ncell = f[i]; i += 1
        expect = f[i:i+2*ncell]
        yield dict(fam=fam, step=step, iq0=iq0, c0=c0, ncol=ncol,
                   shade=shade, first_dest=first_dest, ncs=ncs,
                   wants=wants, ncell=ncell, expect=expect)


def count_oracle_poses(path: Path) -> int:
    n = 0
    for ln in path.read_text().splitlines():
        if ln.strip() and int(ln.split()[0]) > 0:
            n += 1
    return n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('oracle', type=Path)
    ap.add_argument('baker', type=Path)
    ap.add_argument('work_dir', type=Path)
    ap.add_argument('--window', type=int, default=40)
    args = ap.parse_args()
    if args.window < 1:
        raise SystemExit('window must be positive')

    total_poses = count_oracle_poses(args.oracle)
    if args.work_dir.exists():
        shutil.rmtree(args.work_dir)
    args.work_dir.mkdir(parents=True)

    # semantic key: (step, family, want, base, rank) -> exact serialized body
    programs: dict[tuple[int,int,int,int,int], bytes] = {}
    descriptors: set[tuple[int,int,int]] = set()
    steps: set[int] = set()
    thresholds_by_step: dict[int, bytes] = {}
    unique_bodies: set[bytes] = set()
    full_edges = chunks = cells = conflicts = 0
    windows = 0

    for start in range(0, total_poses, args.window):
        npose = min(args.window, total_poses - start)
        wd = args.work_dir / f'w{start:04d}'
        wd.mkdir()
        p = subprocess.run([str(args.baker), str(args.oracle), str(C),
                            str(npose), str(wd), str(start)],
                           text=True, capture_output=True)
        (wd / 'bake.txt').write_text(p.stdout + p.stderr)
        if p.returncode:
            print(p.stdout); print(p.stderr)
            raise SystemExit(f'baker failed at pose {start}')
        windows += 1

        stepmap = (wd / 'progjoin_stepmap.bin').read_bytes()
        desc = (wd / 'progjoin_desc.bin').read_bytes()
        thresh = (wd / 'progjoin_thresh.bin').read_bytes()
        blocks = (wd / 'progjoin_blocks.bin').read_bytes()
        bodies = (wd / 'progjoin_bodies.bin').read_bytes()

        # Recover slot->step from the emitted step map.
        slot_step = {}
        for i, slot in enumerate(stepmap):
            if slot != 0xFF:
                slot_step[slot] = i - 2048
        nslots = len(slot_step)
        if len(desc) != nslots * 32 * 2 or len(thresh) != nslots * 8:
            raise SystemExit(f'bad emitted table shape in window {start}')
        for slot, step in slot_step.items():
            t = thresh[slot*8:slot*8+8]
            old = thresholds_by_step.setdefault(step, t)
            if old != t:
                raise SystemExit(f'threshold conflict for step {step}')

        for case in parse_cases(wd / 'progjoin_cases.txt'):
            fam = case['fam']
            if fam not in FULL_FAMS:
                continue
            full_edges += 1
            step = case['step']; steps.add(step)
            si = step + 2048
            if not 0 <= si < 4096 or stepmap[si] == 0xFF:
                raise SystemExit(f'unmapped case step {step} in window {start}')
            slot = stepmap[si]
            M = FAM_M[fam]
            cs = 0
            for want in case['wants']:
                iq = s16(case['iq0'] + s16(cs * step))
                a = iq + 32
                H, u = a >> 7, a & 127
                ts = thresh[slot*8:slot*8+C+1]
                rank = sum(1 for t in ts if u >= t)
                base = H & (M - 1)
                di = ((slot * 32) + fam*C + (want - 1)) * 2
                if di + 2 > len(desc):
                    raise SystemExit(f'descriptor OOB window={start} key={(step,fam,want)}')
                bo = u16(desc, di)
                pi = bo + (base * 8 + rank) * 2
                if pi + 2 > len(blocks):
                    raise SystemExit(f'block OOB window={start} key={(step,fam,want,base,rank)}')
                body = u16(blocks, pi)
                if body == 0xFFFF or body + C + 1 > len(bodies):
                    raise SystemExit(f'body missing window={start} key={(step,fam,want,base,rank)}')
                ncell = bodies[body + want]
                blen = C + 1 + 4*ncell
                if body + blen > len(bodies):
                    raise SystemExit(f'body OOB window={start} offset={body} len={blen}')
                blob = bodies[body:body+blen]
                key = (step, fam, want, base, rank)
                prior = programs.get(key)
                if prior is not None and prior != blob:
                    conflicts += 1
                    raise SystemExit(f'PROGRAM_CONFLICT window={start} key={key}')
                programs[key] = blob
                unique_bodies.add(blob)
                descriptors.add((step, fam, want))
                chunks += 1
                cells += ncell
                cs += C

    # Minimum block payload if we preserve target block addressing: every
    # observed (step,fam,want) descriptor owns M*8 16-bit entries.
    block_payload = sum(FAM_M[fam] * 8 * 2 for step, fam, want in descriptors)
    body_payload = sum(len(x) for x in unique_bodies)
    # Global stepmap remains 4096 bytes under the current dispatcher; thresholds
    # are 8 bytes per distinct observed step.  Descriptor table under the target
    # shape is 32 entries per step x 2 bytes.
    target_shape = {
        'stepmap': 4096,
        'thresholds': len(steps) * 8,
        'descriptors': len(steps) * 32 * 2,
        'blocks_min_payload': block_payload,
        'unique_body_payload': body_payload,
    }
    target_shape['total_min_payload'] = sum(target_shape.values())

    report = {
        'format': 'GG_PROGJOIN_FULL_CORPUS_CENSUS_V1',
        'poses': total_poses,
        'window': args.window,
        'windows': windows,
        'full_run_edges_observed': full_edges,
        'chunks_observed': chunks,
        'cells_selected_observed': cells,
        'distinct_steps': len(steps),
        'semantic_descriptors': len(descriptors),
        'semantic_program_entries': len(programs),
        'unique_program_bodies': len(unique_bodies),
        'program_conflicts': conflicts,
        'target_shape_bytes': target_shape,
        'banks_16k_min': (target_shape['total_min_payload'] + 16383) // 16384,
        'notes': [
            'union is reconstructed from exact windowed baker output',
            'semantic key is step,family,want,base,rank',
            'body equality is byte-for-byte serialized target equality',
            'no GG pointer representation is assumed by this census',
        ],
    }
    (args.work_dir / 'corpus_census.json').write_text(json.dumps(report, indent=2) + '\n')

    print('=== GG PROGJOIN FULL-CORPUS VOCABULARY CENSUS ===')
    print(f"poses/windows             {total_poses:,} / {windows}")
    print(f"FULL run-edges/chunks     {full_edges:,} / {chunks:,}")
    print(f"distinct steps            {len(steps):,}")
    print(f"semantic descriptors      {len(descriptors):,}")
    print(f"semantic program entries  {len(programs):,}")
    print(f"unique program bodies     {len(unique_bodies):,}")
    print(f"program conflicts         {conflicts}")
    for k, v in target_shape.items():
        print(f"{k:24s} {v:10,d} bytes")
    print(f"minimum 16 KiB banks      {report['banks_16k_min']}")
    print('CORPUS_CENSUS_PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
