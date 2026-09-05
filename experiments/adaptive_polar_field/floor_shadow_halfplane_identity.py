#!/usr/bin/env python3
"""Prove whether the four baked floor-shadow polygons reduce to four half-planes.

Inspection of the accepted shadow polygons shows one interior cast edge each;
all remaining edges lie on an existing receiver-room boundary. If true, the
shadow representation does not need polygon endpoint bearings at runtime at all:

    shadow = receiver_room AND signed_side(static_cast_line)

This script checks that identity against the mature finite-height light oracle
for every sampled free-camera pose. Receiver ownership remains explicit:
P0/P1 belong to Room A floor z=0, P2/P3 to Room B floor z=4. The connector is
not silently folded into Room A.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path

import numpy as np

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import lattice_light_fusion_probe as oracle  # noqa: E402
import floor_shadow_edge_field_poc as field  # noqa: E402

# One interior cast edge per accepted polygon, using local polygon indices.
CAST=((2,0),(2,0),(2,3),(1,2))


def xy(v):return v.x_q4/16.0,v.y_q4/16.0


def side_coeff(poly,edge):
    a=poly[edge[0]];b=poly[edge[1]]
    ax,ay=xy(a);bx,by=xy(b)
    # cross((b-a),(p-a)) = A*x + B*y + C
    A=-(by-ay);B=(bx-ax);C=-A*ax-B*ay
    cx=sum(xy(v)[0] for v in poly)/len(poly)
    cy=sum(xy(v)[1] for v in poly)/len(poly)
    s=A*cx+B*cy+C
    if abs(s)<1e-12:raise RuntimeError('polygon centroid on cast line')
    sign=1.0 if s>0 else -1.0
    return A,B,C,sign

LINES=tuple(side_coeff(poly,CAST[i]) for i,(poly,_) in enumerate(field.POLYS))


def halfplane_frame(cx,cy,yaw):
    full_e=np.zeros((oracle.H,oracle.W),bool)
    shadow=np.zeros((oracle.H,oracle.W),bool)
    wx4,wy4,d4=oracle.camera_floor_arrays(cx,cy,yaw,oracle.ROOM_B_FLOOR_Z)
    room4=oracle.points_in_poly(wx4,wy4,oracle.ROOM_B)&(d4>0)
    wx0,wy0,d0=oracle.camera_floor_arrays(cx,cy,yaw,0.0)
    roomA=oracle.points_in_poly(wx0,wy0,oracle.ROOM_A)&(d0>0)
    connector=oracle.points_in_poly(wx0,wy0,oracle.CONNECTOR)&(d0>0)
    e0=roomA|connector
    use4=room4&(~e0|(d4<d0));use0=e0&~use4
    full_e[73:]=use4|use0

    sh0=np.zeros_like(roomA);sh4=np.zeros_like(room4)
    for i,(A,B,C,sign) in enumerate(LINES):
        if i<2:
            sh0 |= roomA & (sign*(A*wx0+B*wy0+C)>=-1e-9)
        else:
            sh4 |= room4 & (sign*(A*wx4+B*wy4+C)>=-1e-9)
    shadow[73:]=(sh0&use0)|(sh4&use4)
    return full_e,shadow


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--out',default='build/halfplane-identity');ap.add_argument('--step',type=int,default=8);ap.add_argument('--yaw-step',type=int,default=16);a=ap.parse_args()
    out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
    samples=oracle.traversable_samples(a.step,True);rows=[]
    for cx,cy in samples:
      for yaw in range(0,256,a.yaw_step):
        e,exact_lit=oracle.exact_floor_frame(cx,cy,yaw)
        he,shadow=halfplane_frame(cx,cy,yaw)
        if not np.array_equal(e,he):raise RuntimeError('receiver eligibility reproduction changed')
        exact_shadow=e&~exact_lit
        diff=(shadow^exact_shadow)&e;den=max(1,int(e.sum()));n=int(diff.sum())
        rows.append(dict(x=cx,y=cy,yaw=yaw,eligible_pixels=int(e.sum()),mismatch_pixels=n,mismatch_pct=100.0*n/den))
    vals=np.asarray([r['mismatch_pct'] for r in rows],float)
    pix=sum(r['mismatch_pixels'] for r in rows)
    with (out/'pose_metrics.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    summary=dict(positions=len(samples),poses=len(rows),mismatch_pixels_total=pix,mean_pct=float(vals.mean()),p95_pct=float(np.percentile(vals,95)),max_pct=float(vals.max()),lines=[dict(A=A,B=B,C=C,shadow_sign=s) for A,B,C,s in LINES])
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('=== FLOOR SHADOW HALF-PLANE IDENTITY ===')
    print(f"positions={len(samples)} poses={len(rows)}")
    for i,q in enumerate(summary['lines']):print(f"line{i}: A={q['A']:.6g} B={q['B']:.6g} C={q['C']:.6g} shadow_sign={q['shadow_sign']:+.0f}")
    print(f"mismatch total pixels={pix} mean={summary['mean_pct']:.9f}% p95={summary['p95_pct']:.9f}% max={summary['max_pct']:.9f}%")
    if pix==0:print('HALFPLANE_IDENTITY=EXACT')
    else:print('HALFPLANE_IDENTITY=APPROXIMATE')
    return 0

if __name__=='__main__':raise SystemExit(main())
