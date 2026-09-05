#!/usr/bin/env python3
"""Emulate the Game Gear screen-affine floor-light kernel at tile granularity.

This is intentionally shaped like src/tilesector_polar_floor_light_affine.inc:
11 Q4 world half-planes, Q9 yaw, one affine seed per line, 20x9 tile-centre
recurrence, whole-tile shadow fills, and the existing shade-0 8x8 edge family
for the four internal cast boundaries. It measures the approximation added by
the cheap tile materializer separately from the underlying screen-affine math.
"""
from __future__ import annotations
import argparse,csv,json,math,sys
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
import lattice_light_fusion_probe as oracle  # noqa:E402

S=511;F=80
# A,B,C,h in full-Q4 world. Interior is >=0 for all authored polygons.
LINES=(
 (-1,0,1280,16),(0,1,-256,16),(80,-69,-62656,16),(0,-1,1280,16),(64,55,-138240,16),
 (1,3,-2464,12),(1,0,-1792,12),(-23,-33,60224,12),(-7,9,14368,12),(0,-1,1344,12),(-40,57,13312,12),
)
CAST=(2,4,7,10)

def setup(cx,cy,yaw):
    sn=round(math.sin(yaw*math.tau/256)*S);cs=round(math.cos(yaw*math.tau/256)*S)
    cxq=round(cx*16);cyq=round(cy*16);row=[];dx=[];dy=[]
    for A,B,C,h in LINES:
        lc=A*cxq+B*cyq+C;nf=A*cs+B*sn;ns=-A*sn+B*cs;qlc=S*lc
        row.append(9*qlc + nf*(32*h*F) - ns*(16*h*151))
        dx.append(ns*(256*h));dy.append(qlc*16)
    return row,dx,dy

def cross_threshold(e0,dy):
    if dy>0:
        if e0>=0:return 0
        for k in range(1,8):
            e0+=dy
            if e0>=0:return k
        return 8
    if dy<0:
        if e0<0:return 0
        for k in range(1,8):
            e0+=dy
            if e0<0:return k
        return 8
    return 8 if e0>=0 else 0

def edge_mask(li,centre,dx,dy):
    dxp=int(dx[li]/8);dyp=int(dy[li]/8)
    if not dyp:return None
    e00=centre-dxp*4-dyp*4;e70=e00+dxp*7
    l0=cross_threshold(e00,dyp);l1=cross_threshold(e70,dyp)
    if (l0==0 and l1==0) or (l0==8 and l1==8):return None
    ds=max(-7,min(7,l1-l0));mag=abs(ds);off=l0
    mirror=ds<0
    if mirror:off-=mag
    off=max(-7,min(8,off));lut=oracle.EDGE_LUT[mag]
    yy,xx=np.mgrid[0:8,0:8];sample=7-xx if mirror else xx;line=off+lut[sample]
    # Q9 affine derivative tells which pattern side is shadow.
    return yy>=line if dyp>0 else yy<line

def frame(cx,cy,yaw):
    row,dx,dy=setup(cx,cy,yaw);pred_shadow=np.zeros((oracle.H,oracle.W),bool);written=np.zeros_like(pred_shadow)
    currow=row[:]
    for ty in range(9,oracle.ROWS):
        cur=currow[:]
        for tx in range(oracle.COLS):
            o0=cur[0]>=0 and cur[1]>=0;o1=cur[3]>=0 and cur[0]>=0
            o2=cur[5]>=0 and cur[6]>=0 and cur[8]>=0;o3=cur[9]>=0 and cur[6]>=0
            ps=(o0 and cur[2]>=0) or (o1 and cur[4]>=0) or (o2 and cur[7]>=0) or (o3 and cur[10]>=0)
            em=None
            for ok,li in ((o0,2),(o1,4),(o2,7),(o3,10)):
                if ok:
                    em=edge_mask(li,cur[li],dx,dy)
                    if em is not None:break
            ys=slice(ty*8,(ty+1)*8);xs=slice(tx*8,(tx+1)*8)
            if em is not None:pred_shadow[ys,xs]|=em;written[ys,xs]=True
            elif ps:pred_shadow[ys,xs]=True;written[ys,xs]=True
            for i in range(len(cur)):cur[i]+=dx[i]
        for i in range(len(currow)):currow[i]+=dy[i]
    return pred_shadow,written

def stats(v):
    a=np.asarray(v,float);return {'mean':float(a.mean()),'p95':float(np.percentile(a,95)),'max':float(a.max())}

def contact(path,items):
    panels=[]
    for title,exact_e,exact_l,pred in items:
        a=np.zeros((oracle.H,oracle.W,3),np.uint8);a[exact_e]=[80,80,80];a[exact_e&~exact_l]=[22,22,35]
        b=np.zeros_like(a);b[exact_e]=[80,80,80];b[exact_e&~pred]=[22,22,35]
        d=np.zeros_like(a);bad=(exact_l^pred)&exact_e;d[exact_e]=[45,45,45];d[bad]=[240,240,240]
        im=Image.new('RGB',(oracle.W*3,oracle.H+20));im.paste(Image.fromarray(a),(0,20));im.paste(Image.fromarray(b),(oracle.W,20));im.paste(Image.fromarray(d),(oracle.W*2,20));dr=ImageDraw.Draw(im);dr.text((2,2),title+' exact | affine-tile | diff',fill='white');panels.append(im)
    out=Image.new('RGB',(oracle.W*3,max(1,len(panels))*(oracle.H+20)))
    for i,p in enumerate(panels):out.paste(p,(0,i*(oracle.H+20)))
    out.save(path)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--out',default='build/affine-tile-oracle');ap.add_argument('--step',type=int,default=8);ap.add_argument('--yaw-step',type=int,default=16);a=ap.parse_args();out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
    rows=[];worst=[]
    for cx,cy in oracle.traversable_samples(a.step,True):
        for yaw in range(0,256,a.yaw_step):
            e,l=oracle.exact_floor_frame(cx,cy,yaw);sh,w=frame(cx,cy,yaw);pred=e&~sh;den=max(1,int(e.sum()));mis=int(((pred^l)&e).sum());outside=int((w&~e).sum())
            r={'x':cx,'y':cy,'yaw':yaw,'eligible_pixels':int(e.sum()),'mismatch_pixels':mis,'mismatch_pct':100*mis/den,'written_outside_eligible_pixels':outside};rows.append(r)
            if len(worst)<8 or r['mismatch_pct']>worst[0][0]:
                worst.append((r['mismatch_pct'],cx,cy,yaw,e,l,pred));worst=sorted(worst,key=lambda z:z[0])[-8:]
    with (out/'pose_metrics.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=rows[0].keys());w.writeheader();w.writerows(rows)
    st=stats([r['mismatch_pct'] for r in rows]);outs=stats([r['written_outside_eligible_pixels'] for r in rows]);summary={'positions':len(oracle.traversable_samples(a.step,True)),'poses':len(rows),'trig_bits':9,'yaw_lut_bytes':512,'position_field_bytes':0,'reciprocal_bytes':0,'mismatch_pct':st,'outside_eligible_written_pixels':outs}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    items=[]
    for p,cx,cy,y,e,l,pred in sorted(worst,key=lambda z:z[0],reverse=True)[:6]:items.append((f'x={cx:g} y={cy:g} yaw={y} err={p:.3f}%',e,l,pred))
    contact(out/'worst-contact-sheet.png',items)
    print('=== AFFINE TILE RUNTIME ORACLE ===');print(f"poses={len(rows)} mismatch mean={st['mean']:.6f}% p95={st['p95']:.6f}% max={st['max']:.6f}%");print(f"outside-written mean={outs['mean']:.3f}px p95={outs['p95']:.3f}px max={outs['max']:.0f}px")
if __name__=='__main__':main()
