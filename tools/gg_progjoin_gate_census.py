#!/usr/bin/env python3
"""Measure a one-byte live GG ownership/clipping tag for compiled edge cells.

The playable renderer needs more than the research program's {word, NT delta}:
it must reject cells already owned by nearer geometry.  We measure the renderer's
coverage coordinate continuously even through offscreen guard cells:

    cov_index = column*3 + floor(row/8)
    row_bit   = row & 7

Offscreen cells never dereference cov_index, but carrying the integer cursor
through them may avoid reset/absolute tags.  If every consecutive cell has a
tiny cov-index delta, one byte can encode {visible, row_bit, cov_delta} for each
cell and the run-edge needs only one initial coverage seed.
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

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('windows',type=Path); ap.add_argument('out',type=Path)
    a=ap.parse_args()
    vis_delta=collections.Counter(); all_delta=collections.Counter(); bit=collections.Counter()
    visible=off=badcol=vis_trans=all_trans=0
    vmin=999; vmax=-999; amin=999; amax=-999; seed_min=999; seed_max=-999
    for wd in sorted(p for p in a.windows.iterdir() if p.is_dir() and p.name.startswith('w')):
        sm=(wd/'progjoin_stepmap.bin').read_bytes(); ds=(wd/'progjoin_desc.bin').read_bytes(); th=(wd/'progjoin_thresh.bin').read_bytes(); bl=(wd/'progjoin_blocks.bin').read_bytes(); bo=(wd/'progjoin_bodies.bin').read_bytes()
        for fam,step,iq0,c0,ncol,first,wants in cases(wd/'progjoin_cases.txt'):
            if fam not in FULL: continue
            slot=sm[step+2048]; cs=0; cursor=first; prev_cov=None; prev_vis_cov=None
            first_cell=True
            for want in wants:
                iq=s16(iq0+s16(cs*step)); acc=iq+32; base=(acc>>7)&7; u=acc&127
                rank=sum(1 for t in th[slot*8:slot*8+C+1] if u>=t)
                di=((slot*32)+fam*C+want-1)*2; block=u16(ds,di); body=u16(bl,block+(base*8+rank)*2)
                count=bo[body+want]; p=body+C+1
                for _ in range(count):
                    row=cursor//40; rem=cursor-row*40
                    if rem&1: badcol+=1
                    col=rem//2
                    cov=col*3+(row//8)
                    rb=row&7
                    if first_cell:
                        seed_min=min(seed_min,cov); seed_max=max(seed_max,cov); first_cell=False
                    if prev_cov is not None:
                        dd=cov-prev_cov; all_delta[dd]+=1; all_trans+=1; amin=min(amin,dd); amax=max(amax,dd)
                    prev_cov=cov
                    if 0<=row<18 and 0<=col<20:
                        visible+=1; bit[rb]+=1
                        if prev_vis_cov is not None:
                            dd=cov-prev_vis_cov; vis_delta[dd]+=1; vis_trans+=1; vmin=min(vmin,dd); vmax=max(vmax,dd)
                        prev_vis_cov=cov
                    else:
                        off+=1
                    dd=s16(u16(bo,p+2)); cursor += 1+dd; p+=4
                cs+=C
    # Proposed tag: bit7 visible, bits6..4 row_bit (3), bits1..0 cov delta (0..3).
    # Two spare bits remain. One signed/biased seed byte per run-edge is enough
    # if the measured seed range fits 8 bits.
    continuous_fit=(amin>=0 and amax<=3 and badcol==0)
    seed_fit=(seed_min>=-128 and seed_max<=127)
    report={
        'visible_cells':visible,'offscreen_cells':off,'odd_destination_errors':badcol,
        'visible_transition_count':vis_trans,'visible_cov_delta_min':vmin,'visible_cov_delta_max':vmax,
        'visible_cov_delta_distribution':dict(sorted(vis_delta.items())),
        'all_transition_count':all_trans,'continuous_cov_delta_min':amin,'continuous_cov_delta_max':amax,
        'continuous_cov_delta_distribution':dict(sorted(all_delta.items())),
        'first_cell_cov_seed_min':seed_min,'first_cell_cov_seed_max':seed_max,
        'row_bit_distribution':dict(sorted(bit.items())),
        'one_byte_per_cell_continuous_fit':continuous_fit,
        'one_byte_run_seed_fit':seed_fit,
        'proposed_tag':'bit7 visible; bits6..4 row_bit; bits1..0 cov_delta; bits3..2 spare',
        'note':'offscreen cells carry cov cursor arithmetically but never dereference coverage RAM'
    }
    a.out.parent.mkdir(parents=True,exist_ok=True); a.out.write_text(json.dumps(report,indent=2)+'\n')
    print('=== GG PROGJOIN OWNERSHIP TAG CENSUS ===')
    print(f'visible/offscreen cells    {visible:,} / {off:,}')
    print(f'visible delta range        {vmin} .. {vmax}')
    print(f'continuous delta range     {amin} .. {amax}')
    print(f'first-cell seed range      {seed_min} .. {seed_max}')
    print(f'odd destination errors     {badcol}')
    print(f'1-byte cell tag continuous {"YES" if continuous_fit else "NO"}')
    print(f'1-byte run seed            {"YES" if seed_fit else "NO"}')
    print('GATE_CENSUS_PASS')
    return 0
if __name__=='__main__': raise SystemExit(main())
