#!/usr/bin/env python3
"""Emit a real GBDK Game Gear semantic probe from the sparse-direct pack.

The probe uses fixed Frame-2 LIT banks 8..16:
  bank 8  : step map + page bases + thresholds
  bank 9  : descriptor directory
  banks10-11: sparse records (linear 16 KiB split)
  banks12-16: compiled selected-count program bodies

It also emits a stratified set of run-edge test vectors with an expected hash of
the complete 20x32 guarded scratch buffer. The ROM therefore tests actual GG
bank selection, sparse dispatch and compiled-body playback, not merely host-side
packing.
"""
from __future__ import annotations
import argparse, json, struct
from pathlib import Path

BANK=16384; GUARD=7; COLS=20; BUF_ROWS=32; BUF_BYTES=BUF_ROWS*COLS*2; C=6
FAMS={0,2}

def u16(b,o): return b[o] | (b[o+1]<<8)
def s16(v):
    v &= 0xffff
    return v-0x10000 if v&0x8000 else v

def parse_cases(path):
    for ln in path.read_text().splitlines():
        if not ln.strip(): continue
        f=[int(x) for x in ln.split()]; i=0
        fam,step,iq0,c0,ncol,shade=f[i:i+6]; i+=6
        first=f[i]; i+=1; ncs=f[i]; i+=1
        wants=f[i:i+ncs]; i+=ncs; nc=f[i]; i+=1
        expect=[]
        for k in range(nc): expect.append((f[i+2*k],f[i+2*k+1]))
        yield dict(fam=fam,step=step,iq0=iq0,ncol=ncol,first=first,wants=wants,expect=expect)

def fnv1a32(buf):
    h=2166136261
    for x in buf:
        h ^= x; h=(h*16777619)&0xffffffff
    return h

def cbytes(name,data):
    out=[f'const uint8_t {name}[{len(data)}] = {{']
    for i in range(0,len(data),16): out.append('  '+','.join(f'0x{x:02X}' for x in data[i:i+16])+',')
    out.append('};\n'); return '\n'.join(out)

def bank_file(path,bank,arrays):
    s=['#include <stdint.h>',f'#pragma codeseg LIT_{bank}','']
    for name,data in arrays: s.append(cbytes(name,data))
    path.write_text('\n'.join(s))

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('packed',type=Path); ap.add_argument('windows',type=Path); ap.add_argument('out',type=Path); ap.add_argument('--cases',type=int,default=192)
    a=ap.parse_args(); a.out.mkdir(parents=True,exist_ok=True)
    p=a.packed
    step=(p/'step_local.bin').read_bytes(); page=(p/'step_page_base.bin').read_bytes(); th=(p/'thresholds.bin').read_bytes()
    desc=(p/'descriptor.bin').read_bytes(); rec=(p/'records.bin').read_bytes(); bodies=(p/'bodies.bin').read_bytes()
    man=json.loads((p/'manifest.json').read_text())
    if man['body_banks']!=5: raise SystemExit(f"probe bank contract expects 5 body banks, got {man['body_banks']}")
    if len(rec)>2*BANK: raise SystemExit('records need >2 banks')

    bank_file(a.out/'pj_meta.c',8,[('gg_pj_step_local',step),('gg_pj_step_page_base',page),('gg_pj_thresholds',th)])
    bank_file(a.out/'pj_desc.c',9,[('gg_pj_descriptor',desc)])
    bank_file(a.out/'pj_records0.c',10,[('gg_pj_records0',rec[:BANK])])
    bank_file(a.out/'pj_records1.c',11,[('gg_pj_records1',rec[BANK:])])
    for i in range(5):
        chunk=bodies[i*BANK:(i+1)*BANK]
        bank_file(a.out/f'pj_body{i}.c',12+i,[(f'gg_pj_body{i}',chunk)])

    hdr=['#pragma once','#include <stdint.h>','extern const uint8_t gg_pj_step_local[];','extern const uint8_t gg_pj_step_page_base[];','extern const uint8_t gg_pj_thresholds[];','extern const uint8_t gg_pj_descriptor[];','extern const uint8_t gg_pj_records0[];','extern const uint8_t gg_pj_records1[];']
    for i in range(5): hdr.append(f'extern const uint8_t gg_pj_body{i}[];')
    (a.out/'pj_assets.h').write_text('\n'.join(hdr)+'\n')

    allcases=[]
    for wd in sorted(x for x in a.windows.iterdir() if x.is_dir() and x.name.startswith('w')):
        allcases += [c for c in parse_cases(wd/'progjoin_cases.txt') if c['fam'] in FAMS]
    if not allcases: raise SystemExit('no FULL cases')
    # Deterministic spread across the entire corpus, plus endpoints.
    n=min(a.cases,len(allcases)); picks=[]; seen=set()
    for j in range(n):
        ix=(j*(len(allcases)-1))//max(n-1,1)
        if ix not in seen: seen.add(ix); picks.append(allcases[ix])
    # Ensure both FULL families and all final want lengths observed are represented.
    needs={(fam,w) for fam in FAMS for w in range(1,7)}
    have={(c['fam'],c['wants'][-1]) for c in picks}
    for c in allcases:
        k=(c['fam'],c['wants'][-1])
        if k in needs-have:
            picks.append(c); have.add(k)

    vec=[]
    for c in picks:
        buf=bytearray([0x5A]*BUF_BYTES)
        for d,w in c['expect']:
            pos=GUARD*COLS*2+d
            if not 0<=pos<BUF_BYTES-1: raise SystemExit(f'expected cell outside guard buffer pos={pos}')
            buf[pos]=w&255; buf[pos+1]=(w>>8)&255
        vec.append((c['fam'],c['step'],c['iq0'],c['ncol'],c['first'],fnv1a32(buf)))

    vh=['#pragma once','#include <stdint.h>','typedef struct { uint8_t fam; int16_t step; int16_t iq0; uint8_t ncol; int16_t first_dest; uint32_t expect_hash; } GGPJProbeCase;',f'#define GG_PJ_PROBE_CASE_COUNT {len(vec)}u','extern const GGPJProbeCase gg_pj_probe_cases[];']
    (a.out/'pj_vectors.h').write_text('\n'.join(vh)+'\n')
    vc=['#include "pj_vectors.h"','const GGPJProbeCase gg_pj_probe_cases[GG_PJ_PROBE_CASE_COUNT] = {']
    for fam,st,iq,nc,fd,h in vec: vc.append(f'  {{{fam}u,{st},{iq},{nc}u,{fd},0x{h:08X}UL}},')
    vc.append('};\n'); (a.out/'pj_vectors.c').write_text('\n'.join(vc))

    rep={'cases':len(vec),'full_cases_available':len(allcases),'banks':{'meta':8,'descriptor':9,'records':[10,11],'bodies':[12,13,14,15,16]},'guard_rows':GUARD,'buffer_bytes':BUF_BYTES}
    (a.out/'probe_manifest.json').write_text(json.dumps(rep,indent=2)+'\n')
    print(f"PROBE_EMIT_PASS cases={len(vec)} full_available={len(allcases)} banks=8..16")
    return 0
if __name__=='__main__': raise SystemExit(main())
