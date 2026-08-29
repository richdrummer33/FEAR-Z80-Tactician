#!/usr/bin/env python3
"""Measure the staged Polar wall-depth bake that PROJECT_MEMORY already calls for.

The current runtime computes, per active run:
    wall_d_q4(sid, camera) -> inv_for_dq4(distance) -> endpoint correction

This POC targets the first two operations together. It fits the current
8-bit inverse perpendicular wall distance inside each 4-world-unit cell,
per relevant segment. Endpoint/FOV correction remains unchanged for this stage.

That keeps the established start+step design and avoids duplicating the smooth
corner-bearing coefficients just because reciprocal depth needs more refinement.
"""
from __future__ import annotations
import argparse, pathlib
from collections import Counter
import local_projection_field_poc as lp
import endpoint_depth_field_poc as ep

GRID_W,GRID_H=lp.GRID_W,lp.GRID_H
CELL_Q4=lp.CELL_Q4


def fit_record(d,e,sid,gx,gy,x0,x1,y0,y1):
    span=x1-x0
    if span<=0 or span!=(y1-y0) or (span&(span-1)):return None
    cx=(x0+x1-1)*0.5; cy=(y0+y1-1)*0.5
    wx=gx*CELL_Q4; wy=gy*CELL_Q4

    def f(x,y):
        return float(ep.inv_for_dq4(e,ep.wall_d_q4(d,e,sid,int(round(wx+x)),int(round(wy+y)))))

    f0=f(cx,cy)
    # finite derivatives in local-Q4 coordinate units; the reciprocal field is
    # smooth except at near/far clamp boundaries, where refinement/fallback wins.
    dx=(f(cx+0.5,cy)-f(cx-0.5,cy))
    dy=(f(cx,cy+0.5)-f(cx,cy-0.5))
    base=round(f0+dx*(x0-cx)+dy*(y0-cy))
    sx=round(dx*span); sy=round(dy*span)
    if base<0 or base>255 or sx<-127 or sx>127 or sy<-127 or sy>127:return None
    return int(base),int(sx),int(sy)


def leaf_error(d,e,sid,gx,gy,x0,x1,y0,y1):
    rec=fit_record(d,e,sid,gx,gy,x0,x1,y0,y1)
    if rec is None:return 999,True
    base,sx,sy=rec; span=x1-x0; shift=span.bit_length()-1
    worst=0
    for y in range(y0,y1):
      for x in range(x0,x1):
        pred=base+lp.shr0(sx*(x-x0),shift)+lp.shr0(sy*(y-y0),shift)
        pred=max(0,min(255,pred))
        exact=ep.inv_for_dq4(e,ep.wall_d_q4(d,e,sid,gx*CELL_Q4+x,gy*CELL_Q4+y))
        worst=max(worst,abs(pred-exact))
    return worst,False


def segment_depth(d,e,sid,gx,gy,thr,minq):
    maxdepth=0; size=CELL_Q4
    while size>minq:maxdepth+=1;size//=2
    for dep in range(maxdepth+1):
        n=1<<dep; step=CELL_Q4//n; worst=0; ok=True
        for yy in range(n):
          for xx in range(n):
            er,bad=leaf_error(d,e,sid,gx,gy,xx*step,(xx+1)*step,yy*step,(yy+1)*step)
            worst=max(worst,er)
            if bad or er>thr:
                ok=False;break
          if not ok:break
        if ok:return dep,worst
    return None,999


def analyse(thr,minq):
    d=lp.load();e=ep.load_extra()
    cells=0;segrefs=0;fallback=0;leaves=0;worst=0;hist=Counter()
    cardinal_refs=0
    for gy in range(GRID_H):
      for gx in range(GRID_W):
        _,segs=lp.relevant(d,gx,gy)
        if not segs:continue
        cells+=1
        for sid in segs:
            segrefs+=1
            nx=e["nx"][sid];ny=e["ny"][sid]
            if (ny==0 and abs(nx)==32) or (nx==0 and abs(ny)==32):cardinal_refs+=1
            dep,err=segment_depth(d,e,sid,gx,gy,thr,minq)
            if dep is None:fallback+=1
            else:
                hist[dep]+=1;leaves+=4**dep;worst=max(worst,err)

    # Runtime-friendly straw pack:
    # full uint16 cell offsets + 17-bit/3-byte segment mask per nonempty cell +
    # one depth byte per relevant segment + 3 bytes (base,sx,sy) per leaf.
    pack=GRID_W*GRID_H*2 + cells*3 + segrefs + leaves*3
    return dict(threshold=thr,cells=cells,segrefs=segrefs,fallback=fallback,
                leaves=leaves,worst=worst,hist=dict(sorted(hist.items())),
                pack=pack,cardinal_refs=cardinal_refs)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--thresholds",default="1,2,4")
    ap.add_argument("--min-q4",type=int,default=8)
    a=ap.parse_args()
    print("=== POLAR WALL INVERSE-DEPTH FIELD POC ===")
    print(f"cell={CELL_Q4/16:g} world units; min leaf={a.min_q4/16:g} world")
    for raw in a.thresholds.split(","):
        r=analyse(int(raw),a.min_q4)
        print(f"threshold={r['threshold']} inv-unit: cells={r['cells']} segment-refs={r['segrefs']} cardinal={r['cardinal_refs']}")
        print(f"  depth histogram={r['hist']} leaves={r['leaves']} fallback={r['fallback']} worst={r['worst']}")
        print(f"  runtime-shaped pack≈{r['pack']} bytes")
    print("This stage replaces wall_d_q4 + inv_for_dq4 only; endpoint/FOV correction remains the control path.")

if __name__=="__main__":
    main()
