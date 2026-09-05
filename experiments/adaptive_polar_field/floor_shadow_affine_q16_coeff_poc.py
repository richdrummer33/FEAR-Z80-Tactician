#!/usr/bin/env python3
"""Probe a Z80-shaped 16-bit baked-coefficient form of screen-affine shadows.

The rejected first GG implementation kept eleven 32-bit affine values and let
SDCC perform generic long arithmetic. This model asks the more relevant Polar
question: after a fixed per-world-line power-of-two normalization, can we bake
the yaw-dependent horizontal coefficient/bias into ROM and keep runtime camera
translation plus screen recurrence entirely signed-16?

Per line runtime state is:
  q = trunc((511 * (A*x_q4+B*y_q4+C)) / 2**shift)
  e(x=0,y=73) = 3*q + baked_bias[yaw]
  dx_pixel = baked_dx[yaw]
  dy_pixel = 2*q

Only q depends on camera position. The two yaw values are ROM. No reciprocal,
projection, clipping, or runtime trig remains. Camera x/y are still full Q4.
"""
from __future__ import annotations
import argparse,csv,json,math,sys
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
import lattice_light_fusion_probe as oracle  # noqa:E402
import floor_shadow_screen_affine_poc as affine  # noqa:E402

S=511
# Conservative fixed shifts chosen so all tile-grid values remain signed-16.
# They are per static world line, not per camera/yaw.
SHIFTS=(13,12,19,12,19,14,12,18,16,12,18)


def tz(v,sh):
    return (v>>sh) if v>=0 else -((-v)>>sh)


def line_rom(yaw):
    ang=yaw*math.tau/256.0;sn=round(math.sin(ang)*S);cs=round(math.cos(ang)*S);out=[]
    for li,ln in enumerate(affine.POLY_LINES[0]+affine.POLY_LINES[1]+affine.POLY_LINES[2]+affine.POLY_LINES[3]):
        pass
    # Use unique lines in the same stable order as the GG kernel.
    defs=(
      (-1,0,1280,16),(0,1,-256,16),(80,-69,-62656,16),(0,-1,1280,16),(64,55,-138240,16),
      (1,3,-2464,12),(1,0,-1792,12),(-23,-33,60224,12),(-7,9,14368,12),(0,-1,1344,12),(-40,57,13312,12),
    )
    for li,(A,B,C,h) in enumerate(defs):
        nf=A*cs+B*sn;ns=-A*sn+B*cs;sh=SHIFTS[li]
        # x=0 pixel centre has dx2=-159. Moving one pixel changes dx2 by 2.
        bias=tz(nf*(32*h*int(oracle.FOCAL))-ns*(16*h*159),sh)
        dx=tz(ns*(32*h),sh)
        out.append((bias,dx))
    return out

DEFS=(
  (-1,0,1280,16),(0,1,-256,16),(80,-69,-62656,16),(0,-1,1280,16),(64,55,-138240,16),
  (1,3,-2464,12),(1,0,-1792,12),(-23,-33,60224,12),(-7,9,14368,12),(0,-1,1344,12),(-40,57,13312,12),
)


def frame(cx,cy,yaw):
    cxq=round(cx*16);cyq=round(cy*16);rom=line_rom(yaw)
    vals=[]
    for li,(A,B,C,h) in enumerate(DEFS):
        q=tz(S*(A*cxq+B*cyq+C),SHIFTS[li]);bias,dx=rom[li]
        # y=73 pixel centre: v2=3. Use int32 numpy container, but every scalar
        # recurrence coefficient/state is range-checked as signed-16 below.
        base=3*q+bias;dy=2*q
        if not (-32768<=q<=32767 and -32768<=base<=32767 and -32768<=dx<=32767 and -32768<=dy<=32767):
            raise RuntimeError(f'16-bit coefficient overflow line={li} yaw={yaw}')
        yy,xx=np.mgrid[0:oracle.H-73,0:oracle.W]
        v=base+xx*dx+yy*dy
        if int(v.min()) < -32768 or int(v.max()) > 32767:
            raise RuntimeError(f'16-bit screen recurrence overflow line={li} yaw={yaw} min={v.min()} max={v.max()}')
        vals.append(v>=0)
    # Same polygon topology as the GG kernel.
    sh=(vals[0]&vals[1]&vals[2]) | (vals[3]&vals[0]&vals[4]) | (vals[5]&vals[6]&vals[7]&vals[8]) | (vals[9]&vals[10]&vals[6])
    full=np.zeros((oracle.H,oracle.W),bool);full[73:]=sh;return full


def stats(v):
    a=np.asarray(v,float);return {'mean':float(a.mean()),'p95':float(np.percentile(a,95)),'max':float(a.max())}


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--out',default='build/affine-q16');ap.add_argument('--step',type=int,default=8);ap.add_argument('--yaw-step',type=int,default=16);ap.add_argument('--fractional-q4-stress',action='store_true');a=ap.parse_args();out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
    samples=oracle.traversable_samples(a.step,True)
    if a.fractional_q4_stress:samples=affine.stress_samples(samples)
    rows=[]
    for cx,cy in samples:
      for yaw in range(0,256,a.yaw_step):
        e,l=oracle.exact_floor_frame(cx,cy,yaw);sh=frame(cx,cy,yaw);pred=e&~sh;den=max(1,int(e.sum()));mis=int(((pred^l)&e).sum());rows.append({'x':cx,'y':cy,'yaw':yaw,'eligible_pixels':int(e.sum()),'mismatch_pixels':mis,'mismatch_pct':100*mis/den})
    with (out/'pose_metrics.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=rows[0].keys());w.writeheader();w.writerows(rows)
    st=stats([r['mismatch_pct'] for r in rows]);summary={'positions':len(samples),'poses':len(rows),'fractional_q4_stress':a.fractional_q4_stress,'shifts':SHIFTS,'yaw_records':256,'lines':11,'rom_bytes_bias_dx':256*11*4,'runtime_position_field_bytes':0,'runtime_reciprocal_bytes':0,'mismatch_pct':st}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('=== Q16 BAKED AFFINE COEFFICIENT POC ===');print(f"positions={len(samples)} poses={len(rows)} fractional_q4={a.fractional_q4_stress}");print(f"ROM bias+dx={summary['rom_bytes_bias_dx']} bytes; position field=0; reciprocal=0");print(f"mismatch mean={st['mean']:.6f}% p95={st['p95']:.6f}% max={st['max']:.6f}%")
if __name__=='__main__':main()
