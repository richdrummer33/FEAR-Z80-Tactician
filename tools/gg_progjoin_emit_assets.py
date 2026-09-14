#!/usr/bin/env python3
"""Emit sparse-direct PROGJOIN assets as fixed Game Gear Frame-2 LIT banks.

Unlike gg_progjoin_emit_probe.py this emits only runtime assets: no probe vectors
or host oracle. `--base-bank` lets the standalone probe use 8..16 while a
playable experimental ROM can park the same representation high in cartridge
space (for example 48..56 in a 1 MiB ROM) away from ordinary autobanked code.
"""
from __future__ import annotations
import argparse, json
from pathlib import Path
BANK=16384

def cbytes(name,data):
    out=[f'const uint8_t {name}[{len(data)}] = {{']
    for i in range(0,len(data),16): out.append('  '+','.join(f'0x{x:02X}' for x in data[i:i+16])+',')
    out.append('};\n'); return '\n'.join(out)

def bank_file(path,bank,arrays):
    s=['#include <stdint.h>',f'#pragma codeseg LIT_{bank}','']
    for name,data in arrays: s.append(cbytes(name,data))
    path.write_text('\n'.join(s))

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('packed',type=Path); ap.add_argument('out',type=Path); ap.add_argument('--base-bank',type=int,default=8)
    a=ap.parse_args(); a.out.mkdir(parents=True,exist_ok=True); p=a.packed
    step=(p/'step_local.bin').read_bytes(); page=(p/'step_page_base.bin').read_bytes(); th=(p/'thresholds.bin').read_bytes(); desc=(p/'descriptor.bin').read_bytes(); rec=(p/'records.bin').read_bytes(); bodies=(p/'bodies.bin').read_bytes()
    man=json.loads((p/'manifest.json').read_text())
    if man['body_banks']!=5: raise SystemExit(f"expected 5 body banks, got {man['body_banks']}")
    if len(rec)>2*BANK: raise SystemExit('records exceed two banks')
    b=a.base_bank
    bank_file(a.out/'pj_meta.c',b,[('gg_pj_step_local',step),('gg_pj_step_page_base',page),('gg_pj_thresholds',th)])
    bank_file(a.out/'pj_desc.c',b+1,[('gg_pj_descriptor',desc)])
    bank_file(a.out/'pj_records0.c',b+2,[('gg_pj_records0',rec[:BANK])])
    bank_file(a.out/'pj_records1.c',b+3,[('gg_pj_records1',rec[BANK:])])
    for i in range(5): bank_file(a.out/f'pj_body{i}.c',b+4+i,[(f'gg_pj_body{i}',bodies[i*BANK:(i+1)*BANK])])
    hdr=['#pragma once','#include <stdint.h>','extern const uint8_t gg_pj_step_local[];','extern const uint8_t gg_pj_step_page_base[];','extern const uint8_t gg_pj_thresholds[];','extern const uint8_t gg_pj_descriptor[];','extern const uint8_t gg_pj_records0[];','extern const uint8_t gg_pj_records1[];']
    for i in range(5): hdr.append(f'extern const uint8_t gg_pj_body{i}[];')
    (a.out/'pj_assets.h').write_text('\n'.join(hdr)+'\n')
    rep={'base_bank':b,'banks':{'meta':b,'descriptor':b+1,'records':[b+2,b+3],'bodies':[b+4+i for i in range(5)]},'bytes':sum(map(len,[step,page,th,desc,rec,bodies]))}
    (a.out/'pj_live_assets.json').write_text(json.dumps(rep,indent=2)+'\n')
    print(f"LIVE_ASSETS_PASS base={b} last={b+8} bytes={rep['bytes']}")
if __name__=='__main__': main()
