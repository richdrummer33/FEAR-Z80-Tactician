#!/usr/bin/env python3
"""POC for the already-planned second Polar corner field: endpoint depth.

This extends the existing cell-local corner bearing bake with an inverse radial
range value for the same physical endpoint. For ordinary (non-near-clipped)
wall endpoints:

    inverse camera depth = inverse endpoint range * sec(relative bearing)

That is the established endpoint representation. The point of this POC is not
to redesign it, but to answer the implementation questions:
  * can range share the bearing field's adaptive leaves?
  * how many extra ROM bytes does that cost?
  * how much additional refinement is needed?
  * how closely does the direct endpoint form reproduce the current
    wall-plane -> reciprocal -> endpoint-correction integer path?

Near-plane-clamped endpoints are reported separately and remain eligible for
the existing exact path / denser special regions.
"""
from __future__ import annotations

import argparse
import math
import pathlib
import statistics
from collections import Counter

import local_projection_field_poc as lp

ROOT=pathlib.Path(__file__).resolve().parents[2]
GEN=ROOT/"src"/"generated"
CELL_Q4=lp.CELL_Q4
GRID_W,GRID_H=lp.GRID_W,lp.GRID_H
NEAR_Q4=10*16
FAR_Q4=127*16


def all_text():
    return "\n".join(p.read_text() for p in sorted(GEN.glob("tilesector_polar_data_part*.inc")))


def shr0(v,n):
    return v>>n if v>=0 else -((-v)>>n)


def load_extra():
    t=all_text()
    return {
        "nx":lp.arr(t,"k_tspf_nx_q5"),
        "ny":lp.arr(t,"k_tspf_ny_q5"),
        "anchor":lp.arr(t,"k_tspf_seg_anchor"),
        "invz":lp.arr(t,"k_tspf_invz"),
        "sec":lp.arr(t,"k_tspf_sec_q7"),
    }


def wall_d_q4(d,e,sid,xq,yq):
    a=e["anchor"][sid]
    nx=e["nx"][sid]; ny=e["ny"][sid]
    if ny==0 and nx in (32,-32):
        wall=d.vx[a]*16
        return wall-xq if nx>0 else xq-wall
    if nx==0 and ny in (32,-32):
        wall=d.vy[a]*16
        return wall-yq if ny>0 else yq-wall
    xi=xq>>4; yi=yq>>4; fx=xq&15; fy=yq&15
    dx=d.vx[a]-xi; dy=d.vy[a]-yi
    whole=nx*dx+ny*dy
    frac=nx*fx+ny*fy
    return shr0(whole,1)-shr0(frac,5)


def inv_for_dq4(e,dq4):
    a=abs(int(dq4))
    if a<=NEAR_Q4:return 255
    if a>=FAR_Q4:return e["invz"][127]
    z=a>>4; f=a&15
    x0=e["invz"][z]; x1=e["invz"][z+1]; dd=x1-x0
    return x0+shr0(dd*f+(8 if dd>=0 else -8),4)


def endpoint_range_q4(d,v,xq,yq):
    dx=d.vx[v]*16-xq; dy=d.vy[v]*16-yq
    return math.hypot(dx,dy)


def inv_range_target(d,e,v,xq,yq):
    # Same 2560/world-distance reciprocal scale, but radial distance must NOT
    # inherit inv_for_dq4()'s far clamp. The production path clamps the
    # perpendicular wall distance before multiplying by ray/normal alignment;
    # clamping radial distance first is a different operation off-axis.
    r=endpoint_range_q4(d,v,xq,yq)
    if r<=0.0:return 255
    return max(0,min(255,int(round(40960.0/r))))


def range_continuous(d,v,xq,yq):
    r=endpoint_range_q4(d,v,xq,yq)
    if r<=0.0:return 255.0
    return min(255.0,40960.0/r)  # 2560/world-distance, no radial far clamp.


def fit_range_record(d,e,v,gx,gy,x0,x1,y0,y1):
    span=x1-x0
    cx=(x0+x1-1)*0.5; cy=(y0+y1-1)*0.5
    wx=gx*CELL_Q4; wy=gy*CELL_Q4
    f0=range_continuous(d,v,wx+cx,wy+cy)
    # Symmetric finite derivative in Q4 local-coordinate units.
    dx=(range_continuous(d,v,wx+cx+0.5,wy+cy)-
        range_continuous(d,v,wx+cx-0.5,wy+cy))
    dy=(range_continuous(d,v,wx+cx,wy+cy+0.5)-
        range_continuous(d,v,wx+cx,wy+cy-0.5))
    base=round(f0+dx*(x0-cx)+dy*(y0-cy))
    sx=round(dx*span); sy=round(dy*span)
    if base<0 or base>255 or sx<-127 or sx>127 or sy<-127 or sy>127:
        return None
    return int(base),int(sx),int(sy)


def range_leaf_error(d,e,v,gx,gy,x0,x1,y0,y1):
    rec=fit_range_record(d,e,v,gx,gy,x0,x1,y0,y1)
    if rec is None:return float("inf"),True
    base,sx,sy=rec; span=x1-x0; shift=span.bit_length()-1
    worst=0
    for y in range(y0,y1):
      for x in range(x0,x1):
        pred=base+shr0(sx*(x-x0),shift)+shr0(sy*(y-y0),shift)
        pred=max(0,min(255,pred))
        exact=inv_range_target(d,e,v,gx*CELL_Q4+x,gy*CELL_Q4+y)
        worst=max(worst,abs(pred-exact))
    return worst,False


def combined_depth(d,e,v,gx,gy,bearing_thr,range_thr,minq):
    maxdepth=0; size=CELL_Q4
    while size>minq:
        maxdepth+=1; size//=2
    for dep in range(maxdepth+1):
        n=1<<dep; step=CELL_Q4//n; worst_b=0.0; worst_r=0; ok=True
        for yy in range(n):
          for xx in range(n):
            x0=xx*step; x1=(xx+1)*step; y0=yy*step; y1=(yy+1)*step
            eb,bb=lp.quant_leaf_error(d,v,gx,gy,x0,x1,y0,y1)
            er,br=range_leaf_error(d,e,v,gx,gy,x0,x1,y0,y1)
            worst_b=max(worst_b,eb); worst_r=max(worst_r,er)
            if bb or br or eb>bearing_thr+1e-9 or er>range_thr:
                ok=False; break
          if not ok:break
        if ok:return dep,worst_b,worst_r
    return None,float("inf"),float("inf")


def proposed_endpoint_inv(d,e,sid,v,xq,yq,yaw):
    # Returns None when the current renderer is in near-plane clamp territory;
    # that region stays on exact/special handling in the first implementation.
    wd=wall_d_q4(d,e,sid,xq,yq)
    if abs(wd)<=NEAR_Q4:
        return None
    br=lp.bearing(d.vx[v],d.vy[v],xq,yq)
    if math.isnan(br):return None
    bq=int(round(br))&4095
    rel=((bq-(yaw<<4)+2048)&4095)-2048
    if abs(rel)>512:return None
    ir=inv_range_target(d,e,v,xq,yq)
    sec=e["sec"][abs(rel)]
    return min(255,(ir*sec+64)>>7)


def current_endpoint_inv(d,e,sid,v,xq,yq,yaw):
    wd=wall_d_q4(d,e,sid,xq,yq)
    invd=inv_for_dq4(e,wd)
    br=lp.bearing(d.vx[v],d.vy[v],xq,yq)
    if math.isnan(br):return None
    bq=int(round(br))&4095
    rel=((bq-(yaw<<4)+2048)&4095)-2048
    if abs(rel)>512:return None
    bi=(bq>>4)&255
    sn=round(math.sin(bi*math.tau/256)*127)
    cs=round(math.cos(bi*math.tau/256)*127)
    nx=e["nx"][sid]; ny=e["ny"][sid]
    if ny==0 and nx in (32,-32): dot=cs
    elif nx==0 and ny in (32,-32): dot=sn
    else: dot=shr0(nx*cs+ny*sn,5)
    dot=min(127,abs(dot))
    q=(invd*dot+64)>>7
    sec=e["sec"][abs(rel)]
    return min(255,(q*sec+64)>>7)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--bearing-threshold",type=float,default=4.0)
    ap.add_argument("--range-threshold",type=int,default=1)
    ap.add_argument("--min-q4",type=int,default=8)
    a=ap.parse_args()

    d=lp.load(); e=load_extra()
    records=0; leaves=0; fallback=0; hist=Counter()
    worst_b=0.0; worst_r=0
    bearing_only_leaves=0
    for gy in range(GRID_H):
      for gx in range(GRID_W):
        corners,segs=lp.relevant(d,gx,gy)
        for v in corners:
            records+=1
            bd,_=lp.corner_quant_depth(d,v,gx,gy,a.bearing_threshold,a.min_q4)
            if bd is not None:bearing_only_leaves+=4**bd
            dep,eb,er=combined_depth(d,e,v,gx,gy,a.bearing_threshold,a.range_threshold,a.min_q4)
            if dep is None:
                fallback+=1
            else:
                hist[dep]+=1; leaves+=4**dep
                worst_b=max(worst_b,eb); worst_r=max(worst_r,er)

    # Existing format = cell offsets + masks + mode bytes + 4-byte bearing leaves.
    existing=(GRID_W*GRID_H*2)+(466*2)+records+bearing_only_leaves*4
    # Combined leaf adds three bytes: inv-range base8 + sx8 + sy8.
    combined=(GRID_W*GRID_H*2)+(466*2)+records+leaves*7

    # Compare the direct endpoint identity against the current integer wall path
    # over representative local positions and all segment endpoints that can occur.
    diffs=[]; near=0; tested=0
    sample_xy=(4,12,20,28,36,44,52,60)
    sample_yaws=range(0,256,8)
    for gy in range(GRID_H):
      for gx in range(GRID_W):
        _,segs=lp.relevant(d,gx,gy)
        for sid in segs:
            # Endpoints for all keys using this segment in the cell.
            vids=set()
            rid=d.grid[gy*GRID_W+gx]
            if rid==255:continue
            for keyid in lp.recipe_keys(d,rid):
                ks,a0,a1=lp.keyparts(d.keys[keyid])
                if ks==sid:vids.update((a0,a1))
            for lx in sample_xy:
              for ly in sample_xy:
                xq=gx*CELL_Q4+lx; yq=gy*CELL_Q4+ly
                for v in vids:
                  for yaw in sample_yaws:
                    cur=current_endpoint_inv(d,e,sid,v,xq,yq,yaw)
                    if cur is None:continue
                    tested+=1
                    prop=proposed_endpoint_inv(d,e,sid,v,xq,yq,yaw)
                    if prop is None:
                        near+=1; continue
                    diffs.append(abs(prop-cur))

    print("=== POLAR ENDPOINT DEPTH FIELD POC ===")
    print(f"bearing threshold={a.bearing_threshold:g} Q12; range threshold={a.range_threshold} inv-unit; min leaf={a.min_q4/16:g} world")
    print(f"corner records={records}; combined depth histogram={dict(sorted(hist.items()))}; fallback={fallback}")
    print(f"bearing-only leaves={bearing_only_leaves}; combined leaves={leaves}")
    print(f"worst accepted bearing error={worst_b:.3f} Q12; inverse-range error={worst_r}")
    print(f"existing bearing pack approx={existing} bytes")
    print(f"bearing+inverse-range same-leaf pack approx={combined} bytes; delta={combined-existing} bytes")
    if diffs:
        diffs.sort()
        p95=diffs[int(.95*(len(diffs)-1))]
        p99=diffs[int(.99*(len(diffs)-1))]
        print(f"endpoint identity samples={len(diffs)}; near/special fallbacks={near}/{tested} ({100*near/max(1,tested):.2f}%)")
        print(f"direct endpoint-vs-current integer inv error: mean={statistics.mean(diffs):.3f} p95={p95} p99={p99} max={max(diffs)}")
        exact=sum(x==0 for x in diffs)
        within1=sum(x<=1 for x in diffs)
        print(f"  exact={100*exact/len(diffs):.2f}% within1={100*within1/len(diffs):.2f}%")
    print("Interpretation: if storage/refinement stays sane, the GG path can reuse the current cell/leaf decode,")
    print("produce bearing + inverse endpoint range together, then use one secant correction per real endpoint.")
    print("Near-plane/singular regions remain explicit adaptive/special cases rather than forcing the general path.")

if __name__=="__main__":
    main()
