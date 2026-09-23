#!/usr/bin/env python3
"""Census exact physical-screen FULL-wall tiles for the shared-corner raster.

This intentionally ignores the existing coarse-column ownership when deciding
which face owns a pixel. The exact-envelope program supplies the angular spans;
the packed center LUT supplies each physical boundary's sub-column X.

For every trace frame we reconstruct the top 72 pixels of the 160x144 FULL-wall
view, including positive-width spans that own no coarse-column centre. Connected
physical corners become vertical black seams beginning at one authoritative
projected Y. We then count the actual 8x8 pixel vocabulary after hardware HFLIP
folding. FULL bottom tiles are the same vocabulary through VFLIP + palette 1.

This is a feasibility census, not runtime code: it answers whether a single
physical-corner raster can replace the coarse-edge + corrective-pass model
inside the Game Gear's 448-pattern budget.
"""
from __future__ import annotations

import argparse
import csv
from collections import Counter, defaultdict
from pathlib import Path

from e1env_subcolumn_census import (
    arr, bearing_q12, parse_center_lut, parse_header, parse_idx, parse_programs,
    signed_q12,
)

EDGES=(-512,-479,-442,-400,-355,-305,-251,-193,-132,-69,-4,
        61,125,187,245,299,350,396,438,476,506)

def shr0(v,n):
    return v>>n if v>=0 else -((-v)>>n)

def physical_x(code):
    return max(0,min(160,(code&31)*8+((code>>5)-4)))

def canon_hflip(p):
    # Row-major 8x8 semantic pixels: 0 outside, 1 black, 2 wall.
    q=tuple(p[y*8+(7-x)] for y in range(8) for x in range(8))
    p=tuple(p)
    return q if q<p else p

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--generated-dir',required=True)
    ap.add_argument('--trace-csv',required=True)
    ap.add_argument('--center-lut',required=True)
    a=ap.parse_args()

    gen=Path(a.generated_dir)
    h=parse_header(gen/'e1env_generated.h')
    idxbanks=parse_idx(gen)
    progs=parse_programs(gen)
    mt=(gen/'optimized_renderer_map_data.inc').read_text()
    vx=arr(mt,'k_tspf_vx')
    vy=arr(mt,'k_tspf_vy')
    nx=arr(mt,'k_tspf_nx_q5')
    ny=arr(mt,'k_tspf_ny_q5')
    anchor=arr(mt,'k_tspf_seg_anchor')
    recip=arr(mt,'k_tspf_recip8_q16')
    atan=arr(mt,'k_tspf_atan_q12')
    sin=arr(mt,'k_tspf_sin_q7')
    sec=arr(mt,'k_tspf_sec_q7')
    invz=arr(mt,'k_tspf_invz')
    lut=parse_center_lut(a.center_lut)

    if len(progs)!=h['E1ENV_PROGRAM_COUNT']:
        raise RuntimeError('program count mismatch')
    if not(len(nx)==len(ny)==len(anchor)):
        raise RuntimeError('surface metadata mismatch')

    # Pixel-centre angular representatives derived from the SAME monotone
    # boundary LUT used by runtime. This avoids inventing a second projection.
    rels=defaultdict(list)
    for rel,code in zip(range(-512,513),lut):
        x=physical_x(code)
        if x<160:
            rels[x].append(rel)
    rel_for_x=[]
    missing=[]
    for x in range(160):
        rs=rels.get(x)
        if not rs:
            missing.append(x)
            rel_for_x.append(0)
        else:
            rel_for_x.append((rs[0]+rs[-1])//2)
    if missing:
        raise RuntimeError(f'physical-X LUT leaves pixel columns unmapped: {missing}')

    def fetch_prog(xq,yq):
        rx=xq-(h['E1ENV_WORLD_MIN_X']<<4)
        ry=yq-(h['E1ENV_WORLD_MIN_Y']<<4)
        if rx<0 or ry<0 or rx>=h['E1ENV_COLS'] or ry>=h['E1ENV_ROWS']:
            return None
        gx=rx
        gy=ry
        local=((gy&7)*h['E1ENV_COLS'])+gx
        band=gy>>3
        if band>=len(idxbanks):
            return None
        ix,d=idxbanks[band]
        di=ix[local]
        if di>=len(d):
            return None
        pid=d[di]
        if pid>=len(progs):
            return None
        return progs[pid]

    def inv_for_abs_q4(ad):
        if ad <= (10<<4):
            return 255
        if ad >= (127<<4):
            return invz[127]
        z=ad>>4
        f=ad&15
        x0=invz[z]
        x1=invz[z+1]
        d=x1-x0
        return x0+shr0(d*f+(8 if d>=0 else -8),4)

    def dq4_for_sid(sid,xq,yq):
        aid=anchor[sid]
        if ny[sid]==0 and abs(nx[sid])==32:
            return (vx[aid]<<4)-xq
        if nx[sid]==0 and abs(ny[sid])==32:
            return (vy[aid]<<4)-yq
        raise RuntimeError(f'non-cardinal surface {sid}: n=({nx[sid]},{ny[sid]})')

    def eval_inv(sid,xq,yq,yaw,rel):
        # Exact generic inv_at_invd() math, evaluated offline at arbitrary
        # sub-column bearing. Sign of a cardinal normal folds under abs(dot).
        invd=inv_for_abs_q4(abs(dq4_for_sid(sid,xq,yq)))
        wb=((yaw<<4)+rel)&4095
        bi=(wb>>4)&255
        if ny[sid]==0:
            dot=abs(sin[(bi+64)&255])
        else:
            dot=abs(sin[bi])
        dot=min(127,dot)
        q=(invd*dot+64)>>7
        sr=sec[abs(rel)]
        out=(q*sr+64)>>7
        return min(255,out)

    def top_for(sid,xq,yq,yaw,rel):
        return 71-(eval_inv(sid,xq,yq,yaw,rel)>>1)

    with open(a.trace_csv,newline='') as f:
        rows=list(csv.DictReader(f))

    raw_patterns=Counter()
    folded_patterns=Counter()
    folded_category=defaultdict(Counter)
    span_widths=Counter()
    seam_local=Counter()
    seam_count_per_col=Counter()
    corner_y_disagree=Counter()
    seam_row_masks=Counter()
    frames_used=0
    total_intervals=0
    positive_sub8=0
    coarse_dropped=0
    coverage_gaps=0
    coverage_conflicts=0
    joined_tile_samples=0
    edge_tile_samples=0
    fill_tile_samples=0
    outside_tile_samples=0
    multi_face_top_tiles=0
    multi_seam_top_tiles=0

    for fi,row in enumerate(rows):
        xq=int(row['x_q4'])
        yq=int(row['y_q4'])
        yaw=int(row['yaw'])
        yawq=yaw<<4
        prog=fetch_prog(xq,yq)
        if not prog:
            continue
        frames_used+=1
        n=len(prog)
        bcache={}
        for vid,_ in prog:
            if vid not in bcache:
                bcache[vid]=bearing_q12((vx[vid]<<4)-xq,(vy[vid]<<4)-yq,recip,atan)

        spans=[None]*n
        for i,(v0,owner) in enumerate(prog):
            if owner==0xff:
                continue
            v1=prog[(i+1)%n][0]
            if v1 not in bcache:
                bcache[v1]=bearing_q12((vx[v1]<<4)-xq,(vy[v1]<<4)-yq,recip,atan)
            a0=bcache[v0]
            a1=bcache[v1]
            ln=(a1-a0)&4095
            if not ln or ln>=2048:
                continue
            st=signed_q12(a0-yawq)
            en=st+ln
            while en < -512:
                st+=4096
                en+=4096
            while st > 512:
                st-=4096
                en-=4096
            lo=max(st,-512)
            hi=min(en,512)
            if hi<=lo:
                continue
            x0=0 if lo<=-512 else physical_x(lut[lo+512])
            x1=160 if hi>=512 else physical_x(lut[hi+512])
            c0=0 if lo<=-512 else (lut[lo+512]&31)
            cend=20 if hi>=512 else (lut[hi+512]&31)
            sid=owner&31
            spans[i]={
                'sid':sid,'owner':owner,'st':st,'en':en,'lo':lo,'hi':hi,
                'x0':x0,'x1':x1,'c0':c0,'cend':cend,
                'left_unclip':st>=-512,'right_unclip':en<=512,
            }
            if x1>x0:
                total_intervals+=1
                w=x1-x0
                span_widths[w]+=1
                if w<8:
                    positive_sub8+=1
                if cend<=c0:
                    coarse_dropped+=1

        # Pixel ownership comes from exact physical span boundaries, not
        # centre-sampled coarse columns. This is the architectural experiment.
        sid_at=[None]*160
        for sp in spans:
            if not sp or sp['x1']<=sp['x0']:
                continue
            for x in range(max(0,sp['x0']),min(160,sp['x1'])):
                if sid_at[x] is not None and sid_at[x]!=sp['sid']:
                    coverage_conflicts+=1
                sid_at[x]=sp['sid']
        coverage_gaps+=sum(s is None for s in sid_at)

        # Connected physical boundaries. The seam owns one authoritative Y:
        # evaluate both faces at the actual shared bearing, then round their
        # quantized inverse-depth estimates together. Their disagreement is
        # reported so a runtime implementation can choose a cheaper side when
        # the answers are already equivalent.
        seams=[]
        for i,sp in enumerate(spans):
            if not sp:
                continue
            j=(i+1)%n
            rp=spans[j]
            if not rp:
                continue
            owner=sp['owner']
            if not (owner&0x80 and owner&0x40 and rp['owner']&0x20):
                continue
            # Shared authored vertex is the right endpoint of span i.
            rel=sp['en']
            if rel < -512 or rel > 512:
                continue
            code=lut[rel+512]
            sx=physical_x(code)
            if sx>=160:
                continue
            il=eval_inv(sp['sid'],xq,yq,yaw,rel)
            ir=eval_inv(rp['sid'],xq,yq,yaw,rel)
            tl=71-(il>>1)
            tr=71-(ir>>1)
            corner_y_disagree[abs(tl-tr)]+=1
            inv=(il+ir+1)>>1
            cy=71-(inv>>1)
            seams.append((sx,cy,sp['sid'],rp['sid']))
            seam_local[sx&7]+=1

        # Exact top silhouette at each physical pixel.
        top=[999]*160
        for x,sid in enumerate(sid_at):
            if sid is not None:
                top[x]=top_for(sid,xq,yq,yaw,rel_for_x[x])

        # Force the shared endpoint itself to the shared authoritative Y.
        for sx,cy,_,_ in seams:
            top[sx]=cy

        seams_by_col=defaultdict(list)
        for seam in seams:
            seams_by_col[seam[0]>>3].append(seam)
        for col,v in seams_by_col.items():
            seam_count_per_col[len(v)]+=1

        # Top 72 pixels only. FULL bottom is the same pattern family through
        # hardware VFLIP + palette 1, exactly as the current fast path does.
        for ty in range(9):
            ybase=ty*8
            for col in range(20):
                xbase=col*8
                local_seams=seams_by_col.get(col,())
                face_ids={sid_at[x] for x in range(xbase,xbase+8) if sid_at[x] is not None}
                pix=[]
                for ly in range(8):
                    yy=ybase+ly
                    for lx in range(8):
                        xx=xbase+lx
                        sid=sid_at[xx]
                        if sid is None or yy<top[xx]:
                            v=0
                        elif yy==top[xx]:
                            v=1
                        else:
                            v=2
                        # Physical corner line begins at the shared top corner
                        # and continues through wall interior toward the mirror.
                        for sx,cy,_,_ in local_seams:
                            if xx==sx and yy>=cy:
                                v=1
                                break
                        pix.append(v)

                raw=tuple(pix)
                folded=canon_hflip(raw)
                raw_patterns[raw]+=1
                folded_patterns[folded]+=1

                has_out=0 in raw
                has_wall=2 in raw
                seam_here=[s for s in local_seams if ybase+7>=s[1]]
                if not has_wall and 1 not in raw:
                    cat='outside'
                    outside_tile_samples+=1
                elif not has_out and not seam_here:
                    cat='fill'
                    fill_tile_samples+=1
                elif len(face_ids)>=2 or seam_here:
                    cat='joined'
                    joined_tile_samples+=1
                else:
                    cat='edge'
                    edge_tile_samples+=1
                folded_category[folded][cat]+=1

                if len(face_ids)>=2 and has_out:
                    multi_face_top_tiles+=1
                if len(seam_here)>=2 and has_out:
                    multi_seam_top_tiles+=1

                # What vertical-line masks are required on rows that contain
                # no ceiling pixels (pure wall interior)?
                if not has_out and seam_here:
                    mask=0
                    for sx,cy,_,_ in seam_here:
                        if cy<=ybase:
                            mask|=1<<(sx&7)
                    if mask:
                        rm=min(mask,int(f'{mask:08b}'[::-1],2))
                        seam_row_masks[rm]+=1

    category_unique=Counter()
    for pat,cats in folded_category.items():
        # Prefer the strongest interpretation when one pattern appears in
        # multiple contexts.
        if cats['joined']:
            category_unique['joined']+=1
        elif cats['edge']:
            category_unique['edge']+=1
        elif cats['fill']:
            category_unique['fill']+=1
        else:
            category_unique['outside']+=1

    unique=len(folded_patterns)
    cap=448
    print('PHYSICAL_TILE_CENSUS')
    print(f'frames={len(rows)} used={frames_used} programs={len(progs)} intervals={total_intervals}')
    print(f'coverage_gaps_px={coverage_gaps} coverage_conflicts_px={coverage_conflicts}')
    print(f'positive_sub8_spans={positive_sub8} coarse_dropped_positive_spans={coarse_dropped}')
    print('span_width_hist='+','.join(f'{k}:{v}' for k,v in sorted(span_widths.items()) if k<=16))
    print('corner_y_disagreement_px='+','.join(f'{k}:{v}' for k,v in sorted(corner_y_disagree.items())))
    print('seam_local_x='+','.join(f'{k}:{v}' for k,v in sorted(seam_local.items())))
    print('seams_per_column='+','.join(f'{k}:{v}' for k,v in sorted(seam_count_per_col.items())))
    print(f'pattern_samples={sum(raw_patterns.values())} raw_unique={len(raw_patterns)} hflip_unique={unique}')
    print('hflip_unique_by_role='+','.join(f'{k}:{v}' for k,v in sorted(category_unique.items())))
    print(f'sample_roles outside={outside_tile_samples} fill={fill_tile_samples} edge={edge_tile_samples} joined={joined_tile_samples}')
    print(f'multi_face_top_tile_samples={multi_face_top_tiles} multi_seam_top_tile_samples={multi_seam_top_tiles}')
    print(f'interior_seam_masks_hflip_unique={len(seam_row_masks)} masks='+','.join(f'{m:02x}:{c}' for m,c in seam_row_masks.most_common()))
    print(f'VDP_PATTERN_BUDGET required_top_family={unique} capacity={cap} headroom={cap-unique} fits={int(unique<=cap)}')
    print('top_folded_pattern_counts='+','.join(str(v) for _,v in folded_patterns.most_common(20)))

if __name__=='__main__':
    main()
