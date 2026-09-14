#!/usr/bin/env python3
"""Measure whether live GG ownership/clipping metadata fits in one byte/cell.

The research program needs only {word, name-table delta}. The playable GG path
also has to reject cells already owned by nearer geometry, and unlike the flat
research harness it need not spend +560 B WRAM on a guard-band name table.

For every selected FULL program in every emitted corpus case this reconstructs
its actual destination stream, classifies visible/offscreen cells, and measures
transitions in the playable renderer's coverage coordinate:

    cov_index = column*3 + row//8, mask_bit = row&7

If the visible-to-visible cov-index delta is small enough, a byte can carry both
that delta and mask_bit, with one reserved value for offscreen/skip.
"""
from __future__ import annotations
import argparse, collections, json
from pathlib import Path

C=6; FULL={0,2}; COLS=20

def u16(b,o): return b[o] | (b[o+1]<<8)
def s16(v):
    v &= 0xffff
    return v-0x10000 if v&0x8000 else v

def cases(path):
    for ln in path.read_text().splitlines():
        if not ln.strip(): continue
        f=[int(x) for x in ln.split()]; i=0
        fam,step,iq0,c0,ncol,shade=f[i:i+6]; i+=6
        first=f[i]; i+=1; ncs=f[i]; i+=1; wants=f[i:i+ncs]; i+=ncs
        nc=f[i]; i+=1
        yield fam,step,iq0,c0,ncol,first,wants

def floor_div40(d):
    # Python // is the signed floor we want, matching row*40 addressing.
    return d//40

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('windows',type=Path); ap.add_argument('out',type=Path)
    a=ap.parse_args()
    delta=collections.Counter(); bit=collections.Counter(); rows=collections.Counter(); cols=collections.Counter()
    visible=off=badcol=trans=0; dmin=999; dmax=-999
    for wd in sorted(p for p in a.windows.iterdir() if p.is_dir() and p.name.startswith('w')):
        sm=(wd/'progjoin_stepmap.bin').read_bytes(); ds=(wd/'progjoin_desc.bin').read_bytes(); th=(wd/'progjoin_thresh.bin').read_bytes(); bl=(wd/'progjoin_blocks.bin').read_bytes(); bo=(wd/'progjoin_bodies.bin').read_bytes()
        for fam,step,iq0,c0,ncol,first,wants in cases(wd/'progjoin_cases.txt'):
            if fam not in FULL: continue
            slot=sm[step+2048]; cs=0; cursor=first; prev_cov=None
            for want in wants:
                iq=s16(iq0+s16(cs*step)); acc=iq+32; base=(acc>>7)&7; u=acc&127
                rank=sum(1 for t in th[slot*8:slot*8+C+1] if u>=t)
                di=((slot*32)+fam*C+want-1)*2; block=u16(ds,di); body=u16(bl,block+(base*8+rank)*2)
                count=bo[body+want]; p=body+C+1
                for k in range(count):
                    row=floor_div40(cursor); rem=cursor-row*40
                    if rem&1: badcol+=1
                    col=rem//2
                    rows[row]+=1; cols[col]+=1
                    if 0<=row<18 and 0<=col<20:
                        visible+=1
                        cov=col*3+(row>>3); rb=row&7; bit[rb]+=1
                        if prev_cov is not None:
                            dd=cov-prev_cov; delta[dd]+=1; trans+=1; dmin=min(dmin,dd); dmax=max(dmax,dd)
                        prev_cov=cov
                    else:
                        off+=1
                        # An offscreen cell breaks a simple carried coverage
                        # cursor unless its tag also describes the next absolute
                        # visible position; record that by clearing the carry.
                        prev_cov=None
                    dd=s16(u16(bo,p+2)); cursor += 1+dd; p+=4
                cs+=C
    # Candidate byte: top bit offscreen; otherwise 3 bits row-mask index and
    # four-bit signed cov delta (-8..+7). First visible after reset needs an
    # absolute seed, so count resets separately.
    fit = (dmin>=-8 and dmax<=7 and badcol==0)
    report={'visible_cells':visible,'offscreen_cells':off,'odd_destination_errors':badcol,
            'visible_transition_count':trans,'cov_delta_min':dmin,'cov_delta_max':dmax,
            'cov_delta_distribution':dict(sorted(delta.items())),
            'row_bit_distribution':dict(sorted(bit.items())),
            'one_byte_delta3bit_fit_minus8_plus7':fit,
            'note':'offscreen cells reset carried coverage cursor; first following visible cell still needs seed/absolute tag'}
    a.out.parent.mkdir(parents=True,exist_ok=True); a.out.write_text(json.dumps(report,indent=2)+'\n')
    print('=== GG PROGJOIN OWNERSHIP TAG CENSUS ===')
    print(f'visible/offscreen cells   {visible:,} / {off:,}')
    print(f'coverage delta range      {dmin} .. {dmax}')
    print(f'odd destination errors    {badcol}')
    print(f'one-byte delta+rowbit fit {"YES" if fit else "NO"}')
    print('GATE_CENSUS_PASS')
    return 0
if __name__=='__main__': raise SystemExit(main())
