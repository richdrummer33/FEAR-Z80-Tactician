#!/usr/bin/env python3
"""Pack the full observed FULL-wall PROGJOIN vocabulary for GG execution.

This is the cartridge-oriented successor to the research target's dense block
lattice.  It preserves every observed semantic choice exactly, but stores only
reachable choices and gives each choice its program body's bank+offset directly.

Selector layout:
  step_local[4096] : page-local rank, 0xFF absent
  step_page_base[16] : u16 global slot base for each 256-value step page
  thresholds[slot][8]
  descriptor[slot][2 families][6 wants] : u16 sparse-record offset, FFFF fallback
  sparse record: repeated {key:u8, body_bank:u8, body_off:u16}, then key=FF
  body blob: tuned selected-count bodies, no body crosses a 16 KiB bank

`key = base*8 + rank`. A body is the accepted selected-count format: one count
byte followed by exactly count four-byte cells. The old seven-byte prefix table
and unused suffix cells are removed exactly as in the measured Z80 tuning rung.
"""
from __future__ import annotations
import argparse, json, struct
from pathlib import Path

C=6; BANK=16384; FAMS=(0,2)

def u16(b,o): return b[o] | (b[o+1]<<8)
def p16(v): return struct.pack('<H',v)
def s16(v):
    v &= 0xffff
    return v-0x10000 if v&0x8000 else v

def cases(path):
    for ln in path.read_text().splitlines():
        if not ln.strip(): continue
        f=[int(x) for x in ln.split()]; i=0
        fam,step,iq0,c0,ncol,shade=f[i:i+6]; i+=6
        first=f[i]; i+=1; ncs=f[i]; i+=1
        wants=f[i:i+ncs]; i+=ncs; nc=f[i]; i+=1
        yield fam,step,iq0,wants

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('windows',type=Path); ap.add_argument('out',type=Path)
    a=ap.parse_args(); a.out.mkdir(parents=True,exist_ok=True)
    sem={}; thresholds={}
    for wd in sorted(p for p in a.windows.iterdir() if p.is_dir() and p.name.startswith('w')):
        sm=(wd/'progjoin_stepmap.bin').read_bytes(); d=(wd/'progjoin_desc.bin').read_bytes()
        th=(wd/'progjoin_thresh.bin').read_bytes(); bl=(wd/'progjoin_blocks.bin').read_bytes(); bo=(wd/'progjoin_bodies.bin').read_bytes()
        for ix,slot in enumerate(sm):
            if slot!=0xff:
                step=ix-2048; t=th[slot*8:slot*8+8]
                if step in thresholds and thresholds[step]!=t: raise SystemExit(f'threshold conflict {step}')
                thresholds[step]=t
        for fam,step,iq0,wants in cases(wd/'progjoin_cases.txt'):
            if fam not in FAMS: continue
            slot=sm[step+2048]; cs=0
            for want in wants:
                iq=s16(iq0+s16(cs*step)); q=iq+32; base=(q>>7)&7; u=q&127
                rank=sum(1 for t in th[slot*8:slot*8+C+1] if u>=t)
                di=((slot*32)+fam*C+(want-1))*2; block=u16(d,di); pi=block+(base*8+rank)*2; body=u16(bl,pi)
                if body==0xffff: raise SystemExit('reachable body missing')
                count=bo[body+want]
                tuned=bytes([count])+bo[body+C+1:body+C+1+4*count]
                k=(step,fam,want,base,rank)
                if k in sem and sem[k]!=tuned: raise SystemExit(f'program conflict {k}')
                sem[k]=tuned; cs+=C

    steps=sorted({k[0] for k in sem})
    pages=[[] for _ in range(16)]
    for s in steps: pages[(s+2048)>>8].append(s)
    maxpage=max(map(len,pages))
    if maxpage>=255: raise SystemExit(f'page rank overflow {maxpage}')
    order=[]; bases=[]; run=0
    for ps in pages:
        bases.append(run); q=sorted(ps); order+=q; run+=len(q)
    slotof={s:i for i,s in enumerate(order)}
    local=bytearray(b'\xff'*4096)
    for ps in pages:
        for r,s in enumerate(sorted(ps)): local[s+2048]=r
    baseblob=b''.join(p16(x) for x in bases)
    threshblob=b''.join(thresholds[s] for s in order)

    # Bank-pack deduplicated tuned bodies. Store (bank, offset-within-bank).
    bodyset=sorted(set(sem.values())); loc={}; bodies=bytearray(); padding=0
    for body in bodyset:
        within=len(bodies)%BANK
        if within+len(body)>BANK:
            n=BANK-within; bodies.extend(b'\0'*n); padding+=n; within=0
        bank=len(bodies)//BANK; off=len(bodies)%BANK
        if bank>255: raise SystemExit('body bank overflow')
        loc[body]=(bank,off); bodies.extend(body)

    desc=bytearray(b'\xff'*(len(order)*12*2)); records=bytearray(); ndesc=nentry=0
    for step in order:
        slot=slotof[step]
        for fi,fam in enumerate(FAMS):
            for want in range(1,7):
                es=[]
                for base in range(8):
                    for rank in range(8):
                        body=sem.get((step,fam,want,base,rank))
                        if body is not None: es.append(((base<<3)|rank,body))
                if not es: continue
                if len(records)>0xfffe: raise SystemExit('record offset overflow')
                di=(slot*12+fi*6+want-1)*2; desc[di:di+2]=p16(len(records)); ndesc+=1
                for key,body in es:
                    bank,off=loc[body]; records += bytes((key,bank))+p16(off); nentry+=1
                records.append(0xff)

    # Round-trip every observed semantic entry through the packed tables.
    for (step,fam,want,base,rank),expected in sem.items():
        ix=step+2048; page=ix>>8; lr=local[ix]
        if lr==0xff: raise SystemExit('step roundtrip miss')
        slot=u16(baseblob,page*2)+lr; fi=0 if fam==0 else 1
        ro=u16(desc,(slot*12+fi*6+want-1)*2)
        if ro==0xffff: raise SystemExit('descriptor roundtrip miss')
        needle=(base<<3)|rank; p=ro; found=None
        while records[p]!=0xff:
            if records[p]==needle: found=(records[p+1],u16(records,p+2)); break
            p+=4
        if found is None: raise SystemExit('record roundtrip miss')
        bank,off=found; absolute=bank*BANK+off
        if bytes(bodies[absolute:absolute+len(expected)])!=expected: raise SystemExit('body roundtrip miss')
        if off+len(expected)>BANK: raise SystemExit('body crosses bank')

    files={'step_local.bin':bytes(local),'step_page_base.bin':baseblob,'thresholds.bin':threshblob,
           'descriptor.bin':bytes(desc),'records.bin':bytes(records),'bodies.bin':bytes(bodies)}
    for n,b in files.items(): (a.out/n).write_bytes(b)
    sz={n:len(b) for n,b in files.items()}; total=sum(sz.values())
    report={'format':'GG_PROGJOIN_FULL_SPARSE_DIRECT_V1','steps':len(order),'max_page_steps':maxpage,
            'descriptors':ndesc,'semantic_entries':nentry,'unique_tuned_bodies':len(bodyset),
            'body_padding':padding,'body_banks':(len(bodies)+BANK-1)//BANK,'sizes':sz,'total_bytes':total,
            'banks_16k_equivalent':(total+BANK-1)//BANK,'roundtrip':'exact'}
    (a.out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print('=== GG FULL SPARSE DIRECT PACK ===')
    print(f"steps/max-page           {len(order):,} / {maxpage}")
    print(f"descriptors/entries     {ndesc:,} / {nentry:,}")
    print(f"unique tuned bodies      {len(bodyset):,}")
    for n,v in sz.items(): print(f"{n:24s} {v:10,d} bytes")
    print(f"body padding/banks       {padding:,} / {report['body_banks']}")
    print(f"TOTAL                    {total:,} bytes ({report['banks_16k_equivalent']} x 16KiB equivalent)")
    print('SPARSE_DIRECT_EXACT')
    return 0
if __name__=='__main__': raise SystemExit(main())
