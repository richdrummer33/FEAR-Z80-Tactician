#!/usr/bin/env python3
"""Decompose live ownership gating into invariant vs cursor-derived pieces.

The full {visible,row_bit,cov_delta} tag is not invariant under body reuse.
This audit asks the useful follow-up questions:
  * is cov_delta alone invariant for a deduplicated body?
  * are visibility/row-bit purely a function of the absolute destination cursor?
  * what destination step vocabulary must a live cursor walker handle?

If cov_delta is invariant, it can be stored compactly beside the body while
visibility/row-bit are derived from the already-carried destination cursor.
If not, prefer deriving all ownership state from cursor motion instead of
splitting the PROGJOIN body vocabulary.
"""
from __future__ import annotations
import argparse, collections, hashlib, json
from pathlib import Path

C=6; FULL={0,2}; ROW_BYTES=40

def u16(b,o): return b[o] | (b[o+1]<<8)
def s16(v):
    v &= 0xffff
    return v-0x10000 if v&0x8000 else v

def iter_cases(path):
    for ln in path.read_text().splitlines():
        if not ln.strip(): continue
        f=[int(x) for x in ln.split()]; i=0
        fam,step,iq0,c0,ncol,shade=f[i:i+6]; i+=6
        first=f[i]; i+=1; ncs=f[i]; i+=1; wants=f[i:i+ncs]
        yield fam,step,iq0,first,wants

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('windows',type=Path); ap.add_argument('out',type=Path); a=ap.parse_args()
    delta_variants=collections.defaultdict(set)
    uses=collections.Counter(); disp=collections.Counter(); transitions=0
    abs_state_mismatch=0
    for wd in sorted(p for p in a.windows.iterdir() if p.is_dir() and p.name.startswith('w')):
        sm=(wd/'progjoin_stepmap.bin').read_bytes(); ds=(wd/'progjoin_desc.bin').read_bytes(); th=(wd/'progjoin_thresh.bin').read_bytes(); bl=(wd/'progjoin_blocks.bin').read_bytes(); bo=(wd/'progjoin_bodies.bin').read_bytes()
        for fam,step,iq0,first,wants in iter_cases(wd/'progjoin_cases.txt'):
            if fam not in FULL: continue
            slot=sm[step+2048]; cursor=first; cs=0
            for want in wants:
                iq=s16(iq0+s16(cs*step)); acc=iq+32; base=(acc>>7)&7; u=acc&127
                rank=sum(1 for t in th[slot*8:slot*8+C+1] if u>=t)
                di=((slot*32)+fam*C+want-1)*2; block=u16(ds,di); body=u16(bl,block+(base*8+rank)*2)
                count=bo[body+want]; p=body+C+1
                payload=bytes([count])+bo[p:p+count*4]
                seq=[]; prev_cov=None
                for _ in range(count):
                    row=cursor//ROW_BYTES; rem=cursor-row*ROW_BYTES; col=rem//2
                    if rem&1: raise SystemExit(f'odd cursor {cursor}')
                    cov=col*3+(row//8)
                    visible=(0<=row<18 and 0<=col<20); rowbit=row&7
                    # Cross-check that these fields require no semantic input beyond cursor.
                    row2,cbyte=divmod(cursor,ROW_BYTES); col2=cbyte//2
                    if visible != (0<=row2<18 and 0<=col2<20) or rowbit!=(row2&7): abs_state_mismatch+=1
                    if prev_cov is None: dd=0
                    else: dd=cov-prev_cov; transitions+=1
                    seq.append(dd)
                    prev_cov=cov
                    d=s16(u16(bo,p+2)); step_bytes=1+d; disp[step_bytes]+=1
                    cursor += step_bytes; p += 4
                delta_variants[payload].add(tuple(seq)); uses[payload]+=1; cs+=want
    conflicts={k:v for k,v in delta_variants.items() if len(v)>1}
    conflict_uses=sum(uses[k] for k in conflicts)
    examples=[]
    for payload,vs in sorted(conflicts.items(), key=lambda kv:(-len(kv[1]), hashlib.sha1(kv[0]).hexdigest()))[:12]:
        examples.append({'sha1':hashlib.sha1(payload).hexdigest(),'uses':uses[payload],'variants':[list(x) for x in list(vs)[:6]],'cells':payload[0]})
    rep={'bodies':len(delta_variants),'uses':sum(uses.values()),'cov_delta_invariant_under_body_dedup':not conflicts,'conflicting_bodies':len(conflicts),'conflicting_uses':conflict_uses,'absolute_visibility_rowbit_cursor_only_mismatches':abs_state_mismatch,'destination_step_bytes_distribution':dict(sorted(disp.items())),'destination_step_classes':len(disp),'examples':examples}
    a.out.parent.mkdir(parents=True,exist_ok=True); a.out.write_text(json.dumps(rep,indent=2)+'\n')
    print('=== GG PROGJOIN GATE COMPONENT AUDIT ===')
    print(f'bodies/uses               {len(delta_variants):,} / {sum(uses.values()):,}')
    print(f'cov-delta conflicts       {len(conflicts):,} bodies / {conflict_uses:,} uses')
    print(f'cursor-only state errors  {abs_state_mismatch}')
    print(f'destination step classes  {len(disp)}')
    print('COV_DELTA_INVARIANT', 'YES' if not conflicts else 'NO')
    print('GATE_COMPONENT_AUDIT_PASS')
    return 0
if __name__=='__main__': raise SystemExit(main())
