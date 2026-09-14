#!/usr/bin/env python3
"""Build a compact whole-corpus FULL-wall PROGJOIN selector for Game Gear.

Consumes the window directories emitted by gg_progjoin_corpus_census.py.  The
research target's dense Mx8 blocks are a good executable proof but a terrible
cartridge format once 549 distinct steps are unioned.  This pack keeps the exact
semantic mapping while removing empty lattice entries.

Runtime shape proposed here:

  step -> global slot
      16 page bases (u16) + 4096 one-byte page-local ranks

  slot,family,want -> sparse record offset
      549 * 12 u16 entries (FULL top/bottom x wants 1..6)

  sparse record
      repeated: key=(base<<3)|rank, body_id:u16
      terminator: 0xFF

  body_id -> body location
      u16 logical offset into a bank-packed tuned-body blob

  tuned body
      count:u8, followed by count * 4-byte cells

The tuned body is byte-equivalent to the already accepted selected-count target:
the old C+1 prefix array is replaced by the one count byte selected by `want`,
and cells after that count are dropped. No geometry is recomputed here.
"""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

C = 6
BANK = 16 * 1024
FULL_FAMS = (0, 2)


def u16(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8)


def p16(v: int) -> bytes:
    return struct.pack('<H', v)


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


def window_dirs(root: Path):
    return sorted(p for p in root.iterdir() if p.is_dir() and p.name.startswith('w'))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('windows_dir', type=Path)
    ap.add_argument('out_dir', type=Path)
    args = ap.parse_args()
    out = args.out_dir
    out.mkdir(parents=True, exist_ok=True)

    # Exact semantic map reconstructed independently from every emitted window.
    # key=(step,fam,want,base,rank), value=tuned selected-count bytes.
    sem: dict[tuple[int,int,int,int,int], bytes] = {}
    thresholds: dict[int, bytes] = {}
    conflicts = 0

    for wd in window_dirs(args.windows_dir):
        stepmap = (wd / 'progjoin_stepmap.bin').read_bytes()
        desc = (wd / 'progjoin_desc.bin').read_bytes()
        thresh = (wd / 'progjoin_thresh.bin').read_bytes()
        blocks = (wd / 'progjoin_blocks.bin').read_bytes()
        bodies = (wd / 'progjoin_bodies.bin').read_bytes()

        slot_steps = {}
        for i, slot in enumerate(stepmap):
            if slot != 0xFF:
                slot_steps[slot] = i - 2048
        for slot, step in slot_steps.items():
            t = thresh[slot*8:slot*8+8]
            old = thresholds.setdefault(step, t)
            if old != t:
                raise SystemExit(f'threshold conflict step={step}')

        for c in parse_cases(wd / 'progjoin_cases.txt'):
            fam = c['fam']
            if fam not in FULL_FAMS:
                continue
            step = c['step']; slot = stepmap[step + 2048]
            cs = 0
            for want in c['wants']:
                iq = s16(c['iq0'] + s16(cs * step))
                a = iq + 32
                H, u = a >> 7, a & 127
                rank = sum(1 for t in thresh[slot*8:slot*8+C+1] if u >= t)
                base = H & 7
                di = ((slot * 32) + fam*C + (want - 1)) * 2
                bo = u16(desc, di)
                pi = bo + (base * 8 + rank) * 2
                body = u16(blocks, pi)
                if body == 0xFFFF:
                    raise SystemExit(f'missing body {wd.name} {(step,fam,want,base,rank)}')
                count = bodies[body + want]
                old_len = C + 1 + 4*count
                if body + old_len > len(bodies):
                    raise SystemExit(f'body OOB {wd.name} off={body}')
                tuned = bytes([count]) + bodies[body+C+1:body+C+1+4*count]
                key = (step, fam, want, base, rank)
                prev = sem.get(key)
                if prev is not None and prev != tuned:
                    conflicts += 1
                    raise SystemExit(f'TUNED_PROGRAM_CONFLICT {wd.name} key={key}')
                sem[key] = tuned
                cs += C

    steps = sorted({k[0] for k in sem})
    slot_of = {step: i for i, step in enumerate(steps)}
    nslots = len(steps)

    # 4096-byte direct step map, but page-local so >255 total slots are legal.
    # The 16 u16 page bases turn each byte into a global slot cheaply.
    page_steps = [[] for _ in range(16)]
    for step in steps:
        idx = step + 2048
        page_steps[idx >> 8].append(step)
    max_page = max(map(len, page_steps))
    if max_page >= 255:
        raise SystemExit(f'page-local byte map overflow: max page has {max_page} steps')
    page_base = []
    running = 0
    for ps in page_steps:
        page_base.append(running)
        running += len(ps)
    # Global slot order must be page order/local rank for base+rank to work.
    ordered_steps = [s for ps in page_steps for s in sorted(ps)]
    slot_of = {step: i for i, step in enumerate(ordered_steps)}
    local_rank = bytearray(b'\xFF' * 4096)
    for page, ps in enumerate(page_steps):
        for r, step in enumerate(sorted(ps)):
            local_rank[step + 2048] = r
    page_base_blob = b''.join(p16(v) for v in page_base)

    # Thresholds follow the exact global slot order.
    threshold_blob = b''.join(thresholds[s] for s in ordered_steps)

    # Intern tuned bodies, then bank-pack so none crosses 16 KiB. Body IDs are
    # dense and stable by byte ordering, decoupling selector records from bank
    # layout. A separate body-id table contains a 16-bit logical packed offset.
    body_set = sorted(set(sem.values()))
    body_id = {b: i for i, b in enumerate(body_set)}
    if len(body_set) >= 65535:
        raise SystemExit('body id overflow')
    body_blob = bytearray()
    body_offsets = []
    pad = 0
    for b in body_set:
        within = len(body_blob) & (BANK - 1)
        if within + len(b) > BANK:
            n = BANK - within
            body_blob.extend(b'\x00' * n); pad += n
        if len(body_blob) > 0xFFFF:
            # The current body-id representation below is intentionally a first
            # census rung. If this trips, switch the body directory to bank+off.
            raise SystemExit('packed bodies exceed 64 KiB; need bank+offset body directory')
        body_offsets.append(len(body_blob))
        body_blob.extend(b)

    body_dir = b''.join(p16(x) for x in body_offsets)

    # 12 descriptors per step: fam0 wants1..6, then fam2 wants1..6.
    # Each points into records. 0xFFFF means unsupported/fallback.
    desc_dir = bytearray(b'\xFF' * (nslots * 12 * 2))
    records = bytearray()
    descriptors = 0
    entries = 0
    for step in ordered_steps:
        slot = slot_of[step]
        for fi, fam in enumerate(FULL_FAMS):
            for want in range(1, C+1):
                es = []
                for base in range(8):
                    for rank in range(8):
                        b = sem.get((step, fam, want, base, rank))
                        if b is not None:
                            es.append(((base << 3) | rank, body_id[b]))
                if not es:
                    continue
                if len(records) >= 0xFFFF:
                    raise SystemExit('record offset overflow')
                di = (slot * 12 + fi*C + (want-1)) * 2
                desc_dir[di:di+2] = p16(len(records))
                for keybyte, bid in es:
                    records.append(keybyte)
                    records.extend(p16(bid))
                    entries += 1
                records.append(0xFF)
                descriptors += 1

    # Exact round-trip validation of every semantic mapping through the packed
    # page-map, descriptor, sparse record, body-id and tuned-body layers.
    for (step, fam, want, base, rank), expected in sem.items():
        idx = step + 2048
        page = idx >> 8
        lr = local_rank[idx]
        if lr == 0xFF:
            raise SystemExit(f'roundtrip step miss {step}')
        slot = u16(page_base_blob, page*2) + lr
        fi = 0 if fam == 0 else 1
        di = (slot * 12 + fi*C + (want-1))*2
        ro = u16(desc_dir, di)
        if ro == 0xFFFF:
            raise SystemExit(f'roundtrip descriptor miss {(step,fam,want)}')
        want_key = (base << 3) | rank
        p = ro; found = None
        while records[p] != 0xFF:
            k = records[p]; bid = u16(records, p+1); p += 3
            if k == want_key:
                found = bid; break
        if found is None:
            raise SystemExit(f'roundtrip entry miss {(step,fam,want,base,rank)}')
        off = body_offsets[found]
        got = bytes(body_blob[off:off+len(expected)])
        if got != expected:
            raise SystemExit(f'roundtrip body mismatch {(step,fam,want,base,rank)}')

    files = {
        'step_local.bin': bytes(local_rank),
        'step_page_base.bin': page_base_blob,
        'thresholds.bin': threshold_blob,
        'descriptor_dir.bin': bytes(desc_dir),
        'records.bin': bytes(records),
        'body_dir.bin': body_dir,
        'bodies.bin': bytes(body_blob),
    }
    for name, data in files.items():
        (out / name).write_bytes(data)

    sizes = {k: len(v) for k, v in files.items()}
    report = {
        'format': 'GG_PROGJOIN_FULL_SPARSE_V1',
        'steps': nslots,
        'max_steps_in_256_value_page': max_page,
        'descriptors': descriptors,
        'semantic_entries': entries,
        'unique_tuned_bodies': len(body_set),
        'program_conflicts': conflicts,
        'body_padding': pad,
        'sizes': sizes,
        'total_bytes': sum(sizes.values()),
        'banks_16k_equivalent': (sum(sizes.values()) + BANK - 1)//BANK,
        'roundtrip': 'exact',
        'fallback': 'descriptor 0xFFFF or missing sparse key',
    }
    (out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')

    print('=== GG PROGJOIN FULL SPARSE PACK ===')
    print(f"steps / max-page         {nslots:,} / {max_page}")
    print(f"descriptors / entries   {descriptors:,} / {entries:,}")
    print(f"unique tuned bodies      {len(body_set):,}")
    for k, v in sizes.items():
        print(f"{k:24s} {v:10,d} bytes")
    print(f"body padding             {pad:10,d} bytes")
    print(f"TOTAL                    {report['total_bytes']:10,d} bytes")
    print(f"16 KiB equivalent banks  {report['banks_16k_equivalent']}")
    print('SPARSE_PACK_EXACT')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
