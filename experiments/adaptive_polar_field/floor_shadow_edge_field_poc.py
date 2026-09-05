#!/usr/bin/env python3
"""Measure a Polar-style baked field for static floor-shadow boundaries.

The mature runtime lighting prototype proves the shadow semantics, but still
transforms/clips/projects four polygons every rendered update.  This POC tests
the architecture Polar was built for instead:

  * PC bake absolute endpoint bearing as a cell-local affine field;
  * keep yaw arbitrary (256 steps) by subtracting yaw from absolute bearing;
  * project each straight shadow boundary as a tiny Polar edge run;
  * retain camera-on-boundary/topology changes as explicit fallback cases;
  * quantize mixed 8x8 cells into the EXISTING shade-0 edge vocabulary;
  * measure the existing one-sided ordered-dither penumbra without allocating
    any extra tile-pattern family.

Nine of the thirteen polygon vertices are existing Polar world corners, so the
runtime can reuse g_corner_bearing_q12.  Only four new quarter-unit vertices
need an additional position field.  This script deliberately reports their ROM
cost separately.

This is a representation/oracle experiment.  It does NOT claim Game Gear cycle
performance until the field is linked into the Z80 runtime and profiled there.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np
from PIL import Image, ImageDraw

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import lattice_light_fusion_probe as oracle  # noqa: E402
import local_projection_field_poc as lp      # noqa: E402

QTURN=4096
CELL_Q4=64
FOV_Q12=512
MIN_LEAF_Q4=8
BEARING_THRESHOLD_Q12=4.0

# World-space hard-shadow polygons from the accepted bb691 runtime oracle.
# Existing vertex refs are indices into the mature Polar world-vertex table;
# new refs are quarter-world-unit Q2-derived coordinates expressed exactly in Q4.
@dataclass(frozen=True)
class VRef:
    existing: int
    x_q4: int
    y_q4: int
    name: str


def old(v:int,name:str)->VRef:
    return VRef(v,int(round(float(oracle.VERTS[v,0])*16.0)),int(round(float(oracle.VERTS[v,1])*16.0)),name)

def new(x:float,y:float,name:str)->VRef:
    return VRef(-1,int(round(x*16.0)),int(round(y*16.0)),name)

P0=(old(2,'v2'),old(1,'v1'),new(62.75,16.0,'L0'))
P1=(new(66.25,80.0,'L1'),old(4,'v4'),old(3,'v3'))
P2=(old(10,'v10'),old(8,'v8'),old(6,'v6'),new(145.0,13.0,'L2'))
P3=(old(9,'v9'),new(140.5,84.0,'L3'),old(7,'v7'))
POLYS=((P0,0.0),(P1,0.0),(P2,4.0),(P3,4.0))
NEW_VERTS=tuple(v for p,_ in POLYS for v in p if v.existing<0)
# De-duplicate while retaining authored order.
NEW_VERTS=tuple(dict((v.name,v) for v in NEW_VERTS).values())


def shr0(v:int,n:int)->int:
    return v>>n if v>=0 else -((-v)>>n)


def wrap_q12(v:float)->float:
    return ((v+QTURN/2.0)%QTURN)-QTURN/2.0


def exact_bearing_q12(v:VRef,cx_q4:int,cy_q4:int)->float:
    dx=float(v.x_q4-cx_q4);dy=float(v.y_q4-cy_q4)
    if dx*dx+dy*dy<1e-12:return float('nan')
    return (math.atan2(dy,dx)*QTURN/math.tau)%QTURN

@dataclass(frozen=True)
class LeafRec:
    base:int
    sx:int
    sy:int
    span:int
    x0:int
    y0:int

@dataclass
class FieldDesc:
    depth:Optional[int]
    leaves:Tuple[LeafRec,...]
    worst:float


def fit_point(v:VRef,gx:int,gy:int,x0:int,x1:int,y0:int,y1:int):
    cx=(x0+x1-1)*0.5;cy=(y0+y1-1)*0.5
    X=float(v.x_q4)-(gx*CELL_Q4+cx);Y=float(v.y_q4)-(gy*CELL_Q4+cy);r2=X*X+Y*Y
    if r2<0.25:return None
    base=exact_bearing_q12(v,int(round(gx*CELL_Q4+cx)),int(round(gy*CELL_Q4+cy)))
    sc=QTURN/math.tau
    dx=(Y/r2)*sc;dy=(-X/r2)*sc
    intercept=base+dx*(x0-cx)+dy*(y0-cy)
    span=x1-x0;sx=round(dx*span);sy=round(dy*span)
    if span<=0 or span!=(y1-y0) or (span&(span-1)):return None
    if sx<-127 or sx>127 or sy<-127 or sy>127:return None
    return int(round(intercept))&4095,int(sx),int(sy)


def point_leaf_error(v:VRef,gx:int,gy:int,x0:int,x1:int,y0:int,y1:int)->Tuple[float,bool]:
    q=fit_point(v,gx,gy,x0,x1,y0,y1)
    if q is None:return float('inf'),True
    base,sx,sy=q;span=x1-x0;shift=span.bit_length()-1;worst=0.0
    for y in range(y0,y1):
      for x in range(x0,x1):
        exact=exact_bearing_q12(v,gx*CELL_Q4+x,gy*CELL_Q4+y)
        if math.isnan(exact):return float('inf'),True
        pred=base+shr0(sx*(x-x0),shift)+shr0(sy*(y-y0),shift)
        worst=max(worst,abs(wrap_q12(exact-pred)))
    return worst,False


def build_new_desc(v:VRef,gx:int,gy:int,thr:float,minq:int)->FieldDesc:
    maxdepth=0;size=CELL_Q4
    while size>minq:maxdepth+=1;size//=2
    for dep in range(maxdepth+1):
        n=1<<dep;step=CELL_Q4//n;worst=0.0;recs=[];ok=True
        for yy in range(n):
          for xx in range(n):
            x0=xx*step;y0=yy*step
            er,bad=point_leaf_error(v,gx,gy,x0,x0+step,y0,y0+step)
            worst=max(worst,er)
            q=fit_point(v,gx,gy,x0,x0+step,y0,y0+step)
            if bad or er>thr+1e-9 or q is None:
                ok=False;break
            recs.append(LeafRec(q[0],q[1],q[2],step,x0,y0))
          if not ok:break
        if ok:return FieldDesc(dep,tuple(recs),worst)
    return FieldDesc(None,tuple(),float('inf'))


def eval_desc(desc:FieldDesc,v:VRef,gx:int,gy:int,lx:int,ly:int)->Tuple[int,bool]:
    if desc.depth is None:
        return int(round(exact_bearing_q12(v,gx*CELL_Q4+lx,gy*CELL_Q4+ly)))&4095,True
    n=1<<desc.depth;step=CELL_Q4//n;xx=min(n-1,lx//step);yy=min(n-1,ly//step)
    rec=desc.leaves[yy*n+xx];shift=rec.span.bit_length()-1
    pred=rec.base+shr0(rec.sx*(lx-rec.x0),shift)+shr0(rec.sy*(ly-rec.y0),shift)
    return pred&4095,False


def existing_bearing(d,vid:int,cx_q4:int,cy_q4:int)->Tuple[int,bool]:
    """Re-evaluate the current production local-bearing model for one old corner.

    This uses the same depth selection and four-byte affine record functions as
    local_projection_field_poc.py.  A production fallback remains exact and is
    counted, matching the current renderer's semantics.
    """
    gx=cx_q4>>6;gy=cy_q4>>6;lx=cx_q4&63;ly=cy_q4&63
    dep,_=lp.corner_quant_depth(d,vid,gx,gy,BEARING_THRESHOLD_Q12,MIN_LEAF_Q4)
    if dep is None:
        b=lp.bearing(d.vx[vid],d.vy[vid],cx_q4,cy_q4)
        return int(round(b))&4095,True
    n=1<<dep;step=CELL_Q4//n;xx=min(n-1,lx//step);yy=min(n-1,ly//step)
    x0=xx*step;y0=yy*step
    q=lp.quant_leaf_record(d,vid,gx,gy,x0,x0+step,y0,y0+step)
    if q is None:
        b=lp.bearing(d.vx[vid],d.vy[vid],cx_q4,cy_q4)
        return int(round(b))&4095,True
    base,sx,sy=q;shift=step.bit_length()-1
    return (base+shr0(sx*(lx-x0),shift)+shr0(sy*(ly-y0),shift))&4095,False


def point_on_segment(cx:float,cy:float,a:VRef,b:VRef,eps:float=1e-8)->bool:
    ax=a.x_q4/16.0;ay=a.y_q4/16.0;bx=b.x_q4/16.0;by=b.y_q4/16.0
    dx=bx-ax;dy=by-ay;px=cx-ax;py=cy-ay
    cross=px*dy-py*dx
    if abs(cross)>eps*max(1.0,abs(dx)+abs(dy)):return False
    dot=px*dx+py*dy;return dot>=-eps and dot<=dx*dx+dy*dy+eps


def point_in_poly(cx:float,cy:float,p:Sequence[VRef])->bool:
    pts=[(v.x_q4/16.0,v.y_q4/16.0) for v in p]
    inside=False;j=len(pts)-1
    for i,(xi,yi) in enumerate(pts):
        xj,yj=pts[j]
        if ((yi>cy)!=(yj>cy)) and cx < (xj-xi)*(cy-yi)/(yj-yi+1e-300)+xi:inside=not inside
        j=i
    return inside


def signed_rel(v:int)->int:
    v&=4095
    return v-4096 if v>=2048 else v


def angular_interval(aq:int,bq:int,yawq:int)->Optional[Tuple[float,float]]:
    """Visible minor-arc interval in camera-relative Q12, clipped to +/-45 deg."""
    r0=float(signed_rel(aq-yawq))
    delta=float(signed_rel(bq-aq))
    r1=r0+delta
    lo0=min(r0,r1);hi0=max(r0,r1)
    # Move the unwrapped segment near the current FOV; try neighboring turns to
    # remain robust around the 0/4096 seam.
    best=None
    for k in (-1,0,1):
        lo=lo0+k*QTURN;hi=hi0+k*QTURN
        a=max(lo,-FOV_Q12);b=min(hi,FOV_Q12)
        if b>a and (best is None or b-a>best[1]-best[0]):best=(a,b)
    return best


def rel_q12_for_px(px:float)->float:
    return math.atan((px-80.0)/oracle.FOCAL)*QTURN/math.tau


def edge_py_at_px(a:VRef,b:VRef,floor_z:float,cx:float,cy:float,yaw:int,px:float)->Optional[float]:
    ax=a.x_q4/16.0;ay=a.y_q4/16.0;bx=b.x_q4/16.0;by=b.y_q4/16.0
    # Unnormalised right-hand line normal. Scale cancels in d/den.
    nx=by-ay;ny=ax-bx
    ang=yaw*math.tau/256.0;fx=math.cos(ang);fy=math.sin(ang);rx=-fy;ry=fx
    t=(px-80.0)/oracle.FOCAL
    d=nx*(ax-cx)+ny*(ay-cy)
    den=nx*(fx+rx*t)+ny*(fy+ry*t)
    if abs(den)<1e-12:return None
    depth=d/den
    if depth<=1e-8:return None
    return oracle.HORIZON+(oracle.CAMERA_Z-floor_z)*oracle.FOCAL/depth


def projected_shadow_frame(cx:float,cy:float,yaw:int,bearing:Callable[[VRef],Tuple[int,bool]])->Tuple[np.ndarray,bool,int]:
    """Raster hard shadow from straight edge runs; no polygon clipping/projection.

    Returns (shadow mask, topology_fallback_pose, bearing_fallback_count).
    The fallback flag identifies camera-on-polygon-boundary singularities that a
    production bake should encode as explicit topology leaves/selectors.
    """
    shadow=np.zeros((oracle.H,oracle.W),dtype=bool);topology=False;bf=0
    yawq=yaw<<4
    for p,floor_z in POLYS:
        if any(point_on_segment(cx,cy,p[i],p[(i+1)%len(p)]) for i in range(len(p))):
            topology=True
            continue
        inside=point_in_poly(cx,cy,p)
        intervals=[]
        for i in range(len(p)):
            a=p[i];b=p[(i+1)%len(p)]
            ba,fa=bearing(a);bb,fb=bearing(b);bf+=int(fa)+int(fb)
            iv=angular_interval(ba,bb,yawq)
            if iv is not None:intervals.append((a,b,iv))
        if not intervals:continue
        for sx in range(oracle.W):
            px=sx+0.5;rq=rel_q12_for_px(px);ys=[]
            for a,b,(lo,hi) in intervals:
                if rq<lo-1e-9 or rq>hi+1e-9:continue
                py=edge_py_at_px(a,b,floor_z,cx,cy,yaw,px)
                if py is not None:ys.append(py)
            if inside:
                if not ys:continue
                y0=min(ys)
                for sy in range(73,oracle.H):
                    if sy+0.5>=y0:shadow[sy,sx]=True
            elif len(ys)>=2:
                y0=min(ys);y1=max(ys)
                for sy in range(73,oracle.H):
                    py=sy+0.5
                    if py>=y0 and py<=y1:shadow[sy,sx]=True
    return shadow,topology,bf


def mixed_tile_set(eligible:np.ndarray,mask:np.ndarray)->set:
    out=set()
    for ty in range(9,oracle.ROWS):
      for tx in range(oracle.COLS):
        ys=slice(ty*8,(ty+1)*8);xs=slice(tx*8,(tx+1)*8);e=eligible[ys,xs];m=mask[ys,xs]
        n=int(e.sum());k=int((e&m).sum())
        if n and k and k!=n:out.add((tx,ty))
    return out


def percentile(vals:Sequence[float],p:float)->float:
    return float(np.percentile(np.asarray(vals,dtype=float),p)) if vals else 0.0


def save_contact(rows:Sequence[dict],out:Path,bearing_for_pose):
    imgs=[]
    for r in rows[:8]:
        cx=float(r['x']);cy=float(r['y']);yaw=int(r['yaw'])
        e,exact=oracle.exact_floor_frame(cx,cy,yaw)
        sh,_,_=projected_shadow_frame(cx,cy,yaw,bearing_for_pose(cx,cy))
        lit=e&~sh;q,_=oracle.quantize_frame(e,lit)
        imgs.append(oracle.mask_image(e,exact,q,f"x={cx:g} y={cy:g} yaw={yaw} err={r['quant_mismatch_pct']:.3f}%"))
    if not imgs:return
    sheet=Image.new('RGB',(imgs[0].width*2,imgs[0].height*math.ceil(len(imgs)/2)),(20,20,20))
    for i,img in enumerate(imgs):sheet.paste(img,((i%2)*img.width,(i//2)*img.height))
    sheet.save(out/'worst_field_quantized_contact_sheet.png')


def main()->int:
    ap=argparse.ArgumentParser()
    ap.add_argument('--out',default='build/baked-light-field')
    ap.add_argument('--step',type=int,default=8)
    ap.add_argument('--yaw-step',type=int,default=16)
    ap.add_argument('--bearing-threshold',type=float,default=BEARING_THRESHOLD_Q12)
    ap.add_argument('--min-q4',type=int,default=MIN_LEAF_Q4)
    args=ap.parse_args();out=Path(args.out);out.mkdir(parents=True,exist_ok=True)

    d=lp.load();samples=oracle.traversable_samples(args.step,True)
    sample_cells=sorted({(int(round(x*16))>>6,int(round(y*16))>>6) for x,y in samples})
    recipe_cells=[(gx,gy) for gy in range(lp.GRID_H) for gx in range(lp.GRID_W) if d.grid[gy*lp.GRID_W+gx]!=255]

    # Bake the FOUR added light vertices over the complete non-empty Polar recipe
    # domain. Existing nine vertices already live in the mature projection field.
    new_desc:Dict[Tuple[str,int,int],FieldDesc]={};hist=Counter();fallback=0;leaves=0;worst=0.0
    for gx,gy in recipe_cells:
      for v in NEW_VERTS:
        desc=build_new_desc(v,gx,gy,args.bearing_threshold,args.min_q4)
        new_desc[(v.name,gx,gy)]=desc
        if desc.depth is None:fallback+=1
        else:
            hist[desc.depth]+=1;leaves+=len(desc.leaves);worst=max(worst,desc.worst)
    # Runtime-shaped conservative pack: full 48x24 uint16 offset table, four
    # depth/mode bytes for each nonempty cell, four bytes per affine leaf.
    new_field_bytes=lp.GRID_W*lp.GRID_H*2 + len(recipe_cells)*len(NEW_VERTS) + leaves*4

    existing_cache:Dict[Tuple[int,int,int],Tuple[int,bool]]={}
    def bearing_for_pose(cx:float,cy:float):
        cxq=int(round(cx*16));cyq=int(round(cy*16));gx=cxq>>6;gy=cyq>>6;lx=cxq&63;ly=cyq&63
        def f(v:VRef)->Tuple[int,bool]:
            if v.existing>=0:
                key=(v.existing,cxq,cyq)
                if key not in existing_cache:existing_cache[key]=existing_bearing(d,v.existing,cxq,cyq)
                return existing_cache[key]
            desc=new_desc.get((v.name,gx,gy))
            if desc is None:return int(round(exact_bearing_q12(v,cxq,cyq)))&4095,True
            return eval_desc(desc,v,gx,gy,lx,ly)
        return f

    rows=[];topology_fallback_poses=0;field_bearing_fallback_refs=0
    for cx,cy in samples:
      bf=bearing_for_pose(cx,cy)
      for yaw in range(0,256,args.yaw_step):
        eligible,exact_lit=oracle.exact_floor_frame(cx,cy,yaw)
        shadow,topo,bfc=projected_shadow_frame(cx,cy,yaw,bf)
        field_lit=eligible&~shadow
        denom=max(1,int(eligible.sum()))
        raw_err=int(((field_lit^exact_lit)&eligible).sum())
        q,qm=oracle.quantize_frame(eligible,field_lit)
        quant_err=int(((q^exact_lit)&eligible).sum())
        pen=oracle.apply_penumbra(q,eligible)
        hard_mixed=mixed_tile_set(eligible,q);pen_mixed=mixed_tile_set(eligible,pen)
        padd=int((pen&~q&eligible).sum())
        topology_fallback_poses+=int(topo);field_bearing_fallback_refs+=bfc
        rows.append(dict(
            x=cx,y=cy,yaw=yaw,topology_fallback=int(topo),eligible_pixels=denom,
            raw_mismatch_pixels=raw_err,raw_mismatch_pct=100.0*raw_err/denom,
            quant_mismatch_pixels=quant_err,quant_mismatch_pct=100.0*quant_err/denom,
            mixed_tiles=len(hard_mixed),penumbra_added_pixels=padd,
            penumbra_new_mixed_tiles=len(pen_mixed-hard_mixed),bearing_fallback_refs=bfc,
        ))

    with (out/'pose_metrics.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)

    ordinary=[r for r in rows if not r['topology_fallback']]
    raw=[r['raw_mismatch_pct'] for r in ordinary];quant=[r['quant_mismatch_pct'] for r in ordinary]
    mixed=[r['mixed_tiles'] for r in ordinary];padd=[r['penumbra_added_pixels'] for r in ordinary]
    pen_new=sum(r['penumbra_new_mixed_tiles'] for r in ordinary)
    summary={
        'positions':len(samples),'headings_per_position':256//args.yaw_step,'poses':len(rows),
        'ordinary_poses':len(ordinary),'topology_fallback_poses':topology_fallback_poses,
        'topology_fallback_pct':100.0*topology_fallback_poses/max(1,len(rows)),
        'existing_shadow_vertices_reused':9,'new_shadow_vertices':len(NEW_VERTS),
        'new_vertex_field':{
            'bearing_threshold_q12':args.bearing_threshold,'min_leaf_q4':args.min_q4,
            'recipe_cells':len(recipe_cells),'sample_cells':len(sample_cells),
            'depth_histogram':dict(sorted(hist.items())),'fallback_records':fallback,
            'affine_leaves':leaves,'worst_accepted_q12':worst,'estimated_bytes':new_field_bytes,
        },
        'ordinary_pose_hard_shadow_error_pct':{
            'mean':statistics.mean(raw) if raw else 0.0,'p95':percentile(raw,95),'max':max(raw,default=0.0),
        },
        'ordinary_pose_existing_edge_quantized_error_pct':{
            'mean':statistics.mean(quant) if quant else 0.0,'p95':percentile(quant,95),'max':max(quant,default=0.0),
        },
        'mixed_tiles':{'mean':statistics.mean(mixed) if mixed else 0.0,'p95':percentile(mixed,95),'max':max(mixed,default=0)},
        'penumbra':{
            'mean_added_pixels':statistics.mean(padd) if padd else 0.0,'p95_added_pixels':percentile(padd,95),
            'max_added_pixels':max(padd,default=0),'new_mixed_tiles_total':pen_new,
        },
        'bearing_fallback_refs_observed':field_bearing_fallback_refs,
        'yaw_representation':'absolute Q12 bearing field + arbitrary uint8 yaw subtraction; no yaw bake dimension',
        'runtime_target':'straight shadow edge runs; no per-frame polygon transform or Sutherland-Hodgman clipping',
    }
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    with (out/'SUMMARY.txt').open('w') as f:
        f.write('=== POLAR BAKED FLOOR-SHADOW EDGE FIELD POC ===\n')
        f.write(f"positions={summary['positions']} headings={summary['headings_per_position']} poses={summary['poses']}\n")
        f.write('yaw=absolute Q12 endpoint bearing minus arbitrary 8-bit yaw; NO 16-heading runtime quantization\n')
        f.write('representation=straight floor-plane shadow edge runs; topology singularities remain explicit fallbacks\n')
        f.write(f"vertices=reuse {summary['existing_shadow_vertices_reused']} mature Polar corners + bake {summary['new_shadow_vertices']} new corners\n")
        nf=summary['new_vertex_field']
        f.write(f"new-corner field: recipe-cells={nf['recipe_cells']} leaves={nf['affine_leaves']} fallback-records={nf['fallback_records']} bytes~={nf['estimated_bytes']} hist={nf['depth_histogram']} worst={nf['worst_accepted_q12']:.3f} Q12\n")
        f.write(f"topology-fallback poses={summary['topology_fallback_poses']}/{summary['poses']} ({summary['topology_fallback_pct']:.3f}%)\n")
        re=summary['ordinary_pose_hard_shadow_error_pct'];qe=summary['ordinary_pose_existing_edge_quantized_error_pct']
        f.write(f"ordinary hard-shadow mismatch vs finite-height oracle: mean={re['mean']:.6f}% p95={re['p95']:.6f}% max={re['max']:.6f}%\n")
        f.write(f"after EXISTING 8x8 edge vocabulary: mean={qe['mean']:.6f}% p95={qe['p95']:.6f}% max={qe['max']:.6f}%\n")
        pe=summary['penumbra'];f.write(f"one-sided ordered-dither penumbra: mean-added={pe['mean_added_pixels']:.3f} p95={pe['p95_added_pixels']:.3f} max={pe['max_added_pixels']} NEW-MIXED-TILES={pe['new_mixed_tiles_total']}\n")
        f.write('IMPORTANT: this POC measures representation/ROM pressure, not GG T-states.\n')
        f.write('NEXT: bake topology selectors + four new corner records, link edge-run playback to GG, then cycle-profile against bb691.\n')

    worst_rows=sorted(ordinary,key=lambda r:(r['quant_mismatch_pct'],r['raw_mismatch_pct']),reverse=True)
    save_contact(worst_rows,out,bearing_for_pose)
    print((out/'SUMMARY.txt').read_text())
    return 0

if __name__=='__main__':
    raise SystemExit(main())
