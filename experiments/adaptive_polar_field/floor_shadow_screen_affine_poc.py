#!/usr/bin/env python3
"""Screen-affine half-plane representation for static floor shadows.

For a fixed horizontal receiver plane, evaluating a static world-space line at
the camera ray's floor intersection does NOT require projection or reciprocal.
Let the world line in quarter-unit coordinates be:

    L = A*x_q2 + B*y_q2 + C

and v = screen_y - horizon (>0 below the horizon). With camera height h,
focal length F, forward/side line coefficients nf/ns, multiplication by the
positive floor-ray denominator gives an equivalent screen-space half-plane:

    E = S*L(camera)*v + 4*h*F*nf + 4*h*ns*(screen_x-80)

where nf/ns use sin/cos scaled by S. E is AFFINE in screen x and y. Therefore a
convex shadow polygon is just the AND of 3-4 tiny affine inequalities. No world
vertex transform, endpoint bearing, clipping, reciprocal, division, or per-yaw
view bake is required.

This POC classifies every floor pixel using those inequalities and compares to
the mature finite-height oracle. --trig-bits controls the only approximation
in the line transform (Q5/Q6/Q7 etc camera sin/cos). It also feeds the result
through the existing 8x8 edge vocabulary and the one-sided penumbra test.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import lattice_light_fusion_probe as oracle  # noqa: E402
import floor_shadow_edge_field_poc as src    # noqa: E402

@dataclass(frozen=True)
class Line:
    A:int;B:int;C:int;inside:int


def q2(v):
    assert (v.x_q4&3)==0,(v.name,v.x_q4,v.y_q4)
    assert (v.y_q4&3)==0,(v.name,v.x_q4,v.y_q4)
    return v.x_q4//4,v.y_q4//4


def polygon_lines(poly):
    pts=[q2(v) for v in poly];cx=sum(x for x,_ in pts)/len(pts);cy=sum(y for _,y in pts)/len(pts);out=[]
    for i in range(len(pts)):
        ax,ay=pts[i];bx,by=pts[(i+1)%len(pts)];dx=bx-ax;dy=by-ay;g=math.gcd(abs(dx),abs(dy)) or 1
        A=dy//g;B=-dx//g;C=-(A*ax+B*ay);s=A*cx+B*cy+C
        if abs(s)<1e-12:raise RuntimeError('centroid lies on polygon edge')
        out.append(Line(A,B,C,1 if s>0 else -1))
    return tuple(out)

POLY_LINES=tuple(polygon_lines(poly) for poly,_ in src.POLYS)


def frame(cx,cy,yaw,trig_bits):
    S=(1<<trig_bits)-1;ang=yaw*math.tau/256.0
    sn=int(round(math.sin(ang)*S));cs=int(round(math.cos(ang)*S))
    cxq=int(round(cx*4.0));cyq=int(round(cy*4.0))
    yy,xx=np.mgrid[73:oracle.H,0:oracle.W]
    v2=(2*yy+1)-144          # 2*(pixel-centre y - horizon)
    dx2=(2*xx+1)-160         # 2*(pixel-centre x - 80)

    wx4,wy4,d4=oracle.camera_floor_arrays(cx,cy,yaw,oracle.ROOM_B_FLOOR_Z)
    room4=oracle.points_in_poly(wx4,wy4,oracle.ROOM_B)&(d4>0)
    wx0,wy0,d0=oracle.camera_floor_arrays(cx,cy,yaw,0.0)
    e0=(oracle.points_in_poly(wx0,wy0,oracle.ROOM_A)|oracle.points_in_poly(wx0,wy0,oracle.CONNECTOR))&(d0>0)
    use4=room4&(~e0|(d4<d0));use0=e0&~use4

    sh=np.zeros_like(use0)
    for pi,(lines) in enumerate(POLY_LINES):
        h=16 if pi<2 else 12
        inside=np.ones_like(use0)
        for ln in lines:
            Lc=ln.A*cxq+ln.B*cyq+ln.C
            nf=ln.A*cs+ln.B*sn
            ns=ln.A*(-sn)+ln.B*cs
            # Twice the affine expression so pixel centres stay integer.
            E2=(S*Lc)*v2 + (8*h*int(oracle.FOCAL))*nf + (4*h*ns)*dx2
            inside &= (ln.inside*E2)>=0
        inside &= use0 if pi<2 else use4
        sh |= inside
    full_e=np.zeros((oracle.H,oracle.W),bool);full_s=np.zeros_like(full_e)
    full_e[73:]=use0|use4;full_s[73:]=sh
    return full_e,full_s


def pct_stats(vals):
    a=np.asarray(vals,float);return dict(mean=float(a.mean()),p95=float(np.percentile(a,95)),max=float(a.max()))


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--out',default='build/screen-affine');ap.add_argument('--step',type=int,default=8);ap.add_argument('--yaw-step',type=int,default=16);ap.add_argument('--trig-bits',type=int,default=5);a=ap.parse_args()
    out=Path(a.out);out.mkdir(parents=True,exist_ok=True);samples=oracle.traversable_samples(a.step,True);rows=[]
    for cx,cy in samples:
      for yaw in range(0,256,a.yaw_step):
        e,exact_lit=oracle.exact_floor_frame(cx,cy,yaw);he,shadow=frame(cx,cy,yaw,a.trig_bits)
        if not np.array_equal(e,he):raise RuntimeError('receiver eligibility reproduction changed')
        lit=e&~shadow;den=max(1,int(e.sum()));raw=int(((lit^exact_lit)&e).sum())
        q,qm=oracle.quantize_frame(e,lit);qe=int(((q^exact_lit)&e).sum());p=oracle.apply_penumbra(q,e)
        hard=src.mixed_tile_set(e,q);soft=src.mixed_tile_set(e,p)
        rows.append(dict(x=cx,y=cy,yaw=yaw,eligible_pixels=int(e.sum()),raw_mismatch_pixels=raw,raw_mismatch_pct=100.0*raw/den,quant_mismatch_pixels=qe,quant_mismatch_pct=100.0*qe/den,mixed_tiles=len(hard),penumbra_added_pixels=int((p&~q&e).sum()),penumbra_new_mixed_tiles=len(soft-hard)))
    with (out/'pose_metrics.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    raw=pct_stats([r['raw_mismatch_pct'] for r in rows]);quant=pct_stats([r['quant_mismatch_pct'] for r in rows]);pen=sum(r['penumbra_new_mixed_tiles'] for r in rows)
    unique={}
    for lines in POLY_LINES:
      for ln in lines:unique[(ln.A,ln.B,ln.C)]=ln
    summary=dict(positions=len(samples),poses=len(rows),trig_bits=a.trig_bits,trig_scale=(1<<a.trig_bits)-1,polygon_edge_refs=sum(len(x) for x in POLY_LINES),unique_world_lines=len(unique),raw_error_pct=raw,edge_quantized_error_pct=quant,penumbra_new_mixed_tiles_total=pen,field_bytes=0,bearing_vertices=0,reciprocal_bytes=0,world_lines=[dict(A=A,B=B,C=C) for A,B,C in unique])
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('=== SCREEN-AFFINE FLOOR SHADOW POC ===')
    print(f"positions={len(samples)} poses={len(rows)} trig=Q{a.trig_bits} scale={summary['trig_scale']}")
    print(f"polygon edge refs={summary['polygon_edge_refs']} unique static lines={summary['unique_world_lines']}")
    print('bearing field bytes=0; reciprocal bytes=0; yaw view-bake bytes=0')
    print(f"raw mismatch mean={raw['mean']:.6f}% p95={raw['p95']:.6f}% max={raw['max']:.6f}%")
    print(f"existing-edge mismatch mean={quant['mean']:.6f}% p95={quant['p95']:.6f}% max={quant['max']:.6f}%")
    print(f"penumbra NEW-MIXED-TILES={pen}")
    return 0

if __name__=='__main__':raise SystemExit(main())
