#!/usr/bin/env python3
"""Measure the missing cell-local Polar bearing bake before touching GG runtime.

Consumes the CURRENT generated topology pack. For each 4-world-unit cell, only
corners that can appear in that cell's base/conditional keys are evaluated.

Model:
    bearing ~= base + dx*(local_x-center_x) + dy*(local_y-center_y)

The cell is recursively quartered where that local affine field is not accurate
enough. A singular/min-size leaf is explicitly counted as an exact-fallback
candidate; it is never silently accepted.

Q12: 4096 units/turn. At the +/-45 degree FOV edge, 1 Q12 unit is about
0.245 screen pixels, so 2/4/8 Q12 are roughly 0.5/1/2 pixel worst-case.
"""
from __future__ import annotations
import argparse, math, pathlib, re
from dataclasses import dataclass
from typing import List, Sequence, Set, Tuple

TAU=math.tau
QTURN=4096.0
GRID_W,GRID_H=48,24
CELL_Q4=64
EDGE_PX_PER_Q12=160.0*TAU/4096.0
ROOT=pathlib.Path(__file__).resolve().parents[2]
GEN=ROOT/"src"/"generated"

def arr(text,name):
    m=re.search(r"static\s+const\s+[^;=]+?\b"+re.escape(name)+r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};",text,re.S)
    if not m: raise SystemExit("missing generated array "+name)
    return [int(x,0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+",m.group(1))]

@dataclass
class D:
    keys:Sequence[int]; ro:Sequence[int]; rs:Sequence[int]
    bo:Sequence[int]; bs:Sequence[int]; grid:Sequence[int]
    vx:Sequence[int]; vy:Sequence[int]

def load():
    text="\n".join(p.read_text() for p in sorted(GEN.glob("tilesector_polar_data_part*.inc")))
    return D(arr(text,"k_tspf_keys"),arr(text,"k_tspf_recipe_off"),arr(text,"k_tspf_recipe_stream"),
             arr(text,"k_tspf_base_off"),arr(text,"k_tspf_base_stream"),arr(text,"k_tspf_recipe_grid"),
             arr(text,"k_tspf_vx"),arr(text,"k_tspf_vy"))

def recipe_keys(d,rid):
    o=d.ro[rid]; base=d.rs[o]; nc=d.rs[o+1]
    b=d.bo[base]; n=d.bs[b]; out=set(d.bs[b+1:b+1+n]); p=o+2
    for _ in range(nc):
        out.add(d.rs[p]); p+=2
    return out

def keyparts(w): return w&31,(w>>5)&15,(w>>9)&15

def relevant(d,gx,gy):
    rid=d.grid[gy*GRID_W+gx]
    if rid==255:return (),()
    cs:set[int]=set(); ss:set[int]=set()
    for k in recipe_keys(d,rid):
        sid,a,b=keyparts(d.keys[k]); ss.add(sid);cs.add(a);cs.add(b)
    return tuple(sorted(cs)),tuple(sorted(ss))

def bearing(vx,vy,xq,yq):
    dx=vx*16.0-xq;dy=vy*16.0-yq
    if dx*dx+dy*dy<1e-12:return float("nan")
    return (math.atan2(dy,dx)*QTURN/TAU)%QTURN

def wrap(a): return ((a+QTURN/2)%QTURN)-QTURN/2

@dataclass
class Fit: base:float;dx:float;dy:float;bad:bool

def fit(d,v,gx,gy,x0,x1,y0,y1):
    cx=(x0+x1-1)*.5;cy=(y0+y1-1)*.5
    X=d.vx[v]*16.0-(gx*CELL_Q4+cx);Y=d.vy[v]*16.0-(gy*CELL_Q4+cy);r2=X*X+Y*Y
    if r2<0.25:return Fit(0,0,0,True)
    base=bearing(d.vx[v],d.vy[v],gx*CELL_Q4+cx,gy*CELL_Q4+cy)
    sc=QTURN/TAU
    return Fit(base,(Y/r2)*sc,(-X/r2)*sc,False)

def pts(lo,hi):
    if hi-lo<=5:return list(range(lo,hi))
    z=hi-1
    return sorted({lo,z,(lo+z)//2,(3*lo+z)//4,(lo+3*z)//4})

def sample_err(d,v,gx,gy,x0,x1,y0,y1):
    f=fit(d,v,gx,gy,x0,x1,y0,y1)
    if f.bad:return float("inf"),True
    cx=(x0+x1-1)*.5;cy=(y0+y1-1)*.5;worst=0.0
    for x in pts(x0,x1):
      for y in pts(y0,y1):
        e=bearing(d.vx[v],d.vy[v],gx*CELL_Q4+x,gy*CELL_Q4+y)
        if math.isnan(e):return float("inf"),True
        p=f.base+f.dx*(x-cx)+f.dy*(y-cy)
        worst=max(worst,abs(wrap(e-p)))
    return worst,False

@dataclass
class Leaf:
    gx:int;gy:int;x0:int;x1:int;y0:int;y1:int;corners:Tuple[int,...]
    depth:int;fallback:bool;sample:float

def refine(d,gx,gy,corners,thr,minq):
    out=[]
    def rec(x0,x1,y0,y1,dep):
        worst=0.0;bad=False
        for v in corners:
            e,b=sample_err(d,v,gx,gy,x0,x1,y0,y1);worst=max(worst,e);bad|=b
        can=(x1-x0)>minq and (y1-y0)>minq
        if (bad or worst>thr) and can:
            xm=(x0+x1)//2;ym=(y0+y1)//2
            for xa,xb,ya,yb in ((x0,xm,y0,ym),(xm,x1,y0,ym),(x0,xm,ym,y1),(xm,x1,ym,y1)):
                rec(xa,xb,ya,yb,dep+1)
        else:
            out.append(Leaf(gx,gy,x0,x1,y0,y1,tuple(corners),dep,bool(bad or worst>thr),worst))
    rec(0,CELL_Q4,0,CELL_Q4,0)
    return out

def exhaustive(d,l):
    worst=0.0
    for v in l.corners:
        f=fit(d,v,l.gx,l.gy,l.x0,l.x1,l.y0,l.y1)
        if f.bad:return float("inf"),True
        cx=(l.x0+l.x1-1)*.5;cy=(l.y0+l.y1-1)*.5
        for x in range(l.x0,l.x1):
          for y in range(l.y0,l.y1):
            e=bearing(d.vx[v],d.vy[v],l.gx*CELL_Q4+x,l.gy*CELL_Q4+y)
            if math.isnan(e):return float("inf"),True
            p=f.base+f.dx*(x-cx)+f.dy*(y-cy)
            worst=max(worst,abs(wrap(e-p)))
    return worst,False

def analyse(d,thr,minq,do_ex):
    cells=[]
    for gy in range(GRID_H):
      for gx in range(GRID_W):
        c,s=relevant(d,gx,gy)
        if c:cells.append((gx,gy,c,s))
    leaves=[]
    for gx,gy,c,_ in cells:leaves+=refine(d,gx,gy,c,thr,minq)
    fallback=sum(x.fallback for x in leaves)
    records=sum(len(x.corners) for x in leaves)
    maxdepth=max((x.depth for x in leaves),default=0)
    worst=0.;fails=0
    if do_ex:
      for l in leaves:
        if l.fallback:continue
        e,b=exhaustive(d,l);worst=max(worst,e)
        if b or e>thr+1e-9:fails+=1
    else:worst=max((x.sample for x in leaves if not x.fallback),default=0)
    # Deliberately conservative straw-pack, NOT final format:
    # 2 bytes/leaf + base16/dx8/dy8 (4 bytes) per relevant corner.
    pack=2*len(leaves)+4*records
    return cells,leaves,fallback,records,maxdepth,worst,fails,pack


def corner_uniform_depth(d,v,gx,gy,thr,minq):
    """Choose a simple regular 1x1/2x2/4x4/8x8 coefficient grid for ONE corner.

    This is deliberately different from shared-cell refinement: one nearby
    nonlinear corner must not force all other smooth corners in the cell to
    duplicate their coefficients.
    """
    maxdepth=0
    size=CELL_Q4
    while size>minq:
        maxdepth+=1;size//=2

    for dep in range(maxdepth+1):
        n=1<<dep; step=CELL_Q4//n; sampled_ok=True
        for yy in range(n):
          for xx in range(n):
            e,b=sample_err(d,v,gx,gy,xx*step,(xx+1)*step,yy*step,(yy+1)*step)
            if b or e>thr:
                sampled_ok=False;break
          if not sampled_ok:break
        if not sampled_ok:continue

        # Validate the selected regular depth exhaustively before accepting.
        exhaustive_ok=True; worst=0.0
        for yy in range(n):
          for xx in range(n):
            leaf=Leaf(gx,gy,xx*step,(xx+1)*step,yy*step,(yy+1)*step,(v,),dep,False,0.0)
            e,b=exhaustive(d,leaf);worst=max(worst,e)
            if b or e>thr+1e-9:
                exhaustive_ok=False;break
          if not exhaustive_ok:break
        if exhaustive_ok:return dep,worst
    return None,float("inf")



def shr0(v,shift):
    """Signed power-of-two divide rounded toward zero, matching renderer style."""
    if v>=0:return v>>shift
    return -((-v)>>shift)


def quant_leaf_record(d,v,gx,gy,x0,x1,y0,y1):
    """Return (base_q12, span_dx_q12, span_dy_q12) or None if int8 slope won't fit.

    The slope bytes represent total Q12 change across the power-of-two leaf span.
    Runtime reconstruction is only:
        base + (sx*local_x >> shift) + (sy*local_y >> shift)
    using two signed 8x8 multiplies and shifts; no atan/reciprocal.
    """
    f=fit(d,v,gx,gy,x0,x1,y0,y1)
    if f.bad:return None
    span=x1-x0
    if span<=0 or span!=(y1-y0) or (span & (span-1)):return None
    cx=(x0+x1-1)*.5;cy=(y0+y1-1)*.5
    # Intercept of the center-derived plane at this leaf's local origin.
    intercept=f.base + f.dx*(x0-cx) + f.dy*(y0-cy)
    sx=round(f.dx*span);sy=round(f.dy*span)
    if sx < -127 or sx > 127 or sy < -127 or sy > 127:return None
    return int(round(intercept))&4095,int(sx),int(sy)


def quant_leaf_error(d,v,gx,gy,x0,x1,y0,y1):
    q=quant_leaf_record(d,v,gx,gy,x0,x1,y0,y1)
    if q is None:return float("inf"),True
    base,sx,sy=q;span=x1-x0;shift=span.bit_length()-1;worst=0.0
    for x in range(x0,x1):
      for y in range(y0,y1):
        exact=bearing(d.vx[v],d.vy[v],gx*CELL_Q4+x,gy*CELL_Q4+y)
        if math.isnan(exact):return float("inf"),True
        pred=base+shr0(sx*(x-x0),shift)+shr0(sy*(y-y0),shift)
        worst=max(worst,abs(wrap(exact-pred)))
    return worst,False


def corner_quant_depth(d,v,gx,gy,thr,minq):
    maxdepth=0;size=CELL_Q4
    while size>minq:maxdepth+=1;size//=2
    for dep in range(maxdepth+1):
        n=1<<dep;step=CELL_Q4//n;worst=0.0;ok=True
        for yy in range(n):
          for xx in range(n):
            e,b=quant_leaf_error(d,v,gx,gy,xx*step,(xx+1)*step,yy*step,(yy+1)*step)
            worst=max(worst,e)
            if b or e>thr+1e-9:ok=False;break
          if not ok:break
        if ok:return dep,worst
    return None,float("inf")


def analyse_quantized_pack(d,thr,minq):
    cells=[]
    for gy in range(GRID_H):
      for gx in range(GRID_W):
        c,segs=relevant(d,gx,gy)
        if c:cells.append((gx,gy,c,segs))
    hist={};fallback=0;leaves=0;worst=0.0;corner_count=0
    for gx,gy,corners,_ in cells:
      for v in corners:
        corner_count+=1
        dep,e=corner_quant_depth(d,v,gx,gy,thr,minq)
        if dep is None:fallback+=1;continue
        hist[dep]=hist.get(dep,0)+1
        leaves+=4**dep;worst=max(worst,e)
    # Same concrete pack as the float estimate: cell offsets + 14-bit corner
    # presence mask + one mode byte/corner + one 4-byte quantized leaf record.
    pack=(GRID_W*GRID_H*2)+(len(cells)*2)+corner_count+leaves*4
    return cells,corner_count,fallback,leaves,hist,worst,pack


def analyse_per_corner_uniform(d,thr,minq):
    cells=[]
    for gy in range(GRID_H):
      for gx in range(GRID_W):
        c,segs=relevant(d,gx,gy)
        if c:cells.append((gx,gy,c,segs))

    depth_hist={}
    fallback=0
    leaf_records=0
    worst=0.0
    corner_count=0
    for gx,gy,corners,_ in cells:
      for v in corners:
        corner_count+=1
        dep,e=corner_uniform_depth(d,v,gx,gy,thr,minq)
        if dep is None:
            fallback+=1
            continue
        depth_hist[dep]=depth_hist.get(dep,0)+1
        leaf_records+=4**dep
        worst=max(worst,e)

    # Concrete straw-pack for a runtime-friendly representation:
    # - uint16 cell offset for the complete 48x24 grid,
    # - uint16 present-corner mask for each non-empty cell,
    # - one byte mode/depth for each relevant corner,
    # - 4 bytes per affine coefficient leaf (base16 + dx8 + dy8).
    bytes_est=(GRID_W*GRID_H*2)+(len(cells)*2)+corner_count+(leaf_records*4)
    return cells,corner_count,fallback,leaf_records,depth_hist,worst,bytes_est

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--thresholds",default="2,4,8")
    ap.add_argument("--min-q4",type=int,default=8)
    ap.add_argument("--no-exhaustive",action="store_true")
    a=ap.parse_args();d=load()
    print("=== POLAR LOCAL PROJECTION BAKE POC ===")
    print(f"topology grid={GRID_W}x{GRID_H}; cell={CELL_Q4/16:g} world units; min leaf={a.min_q4/16:g}")
    print(f"1 Q12 bearing ~= {EDGE_PX_PER_Q12:.3f}px at FOV edge")
    print("bearing model: local affine base+dx*x+dy*y; split cell where nonlinear")
    print("range observation: straight-wall plane distance is exactly affine in camera x/y; bearing is the nonlinear field.")
    thresholds=[float(x) for x in a.thresholds.split(",")]
    for t in thresholds:
        cells,leaves,fb,rec,md,worst,fails,pack=analyse(d,t,a.min_q4,not a.no_exhaustive)
        avgc=sum(len(c) for _,_,c,_ in cells)/max(1,len(cells))
        avgl=len(leaves)/max(1,len(cells))
        segrefs=sum(len(ss) for *_,ss in cells)
        print(f"shared-refine threshold={t:g} Q12 (~{t*EDGE_PX_PER_Q12:.2f}px edge): cells={len(cells)} leaves={len(leaves)} avg-leaves/cell={avgl:.2f} max-depth={md} fallback={fb}")
        print(f"  relevant corners/cell={avgc:.2f}; segment-refs={segrefs}; corner-coeff-records={rec}; rough-pack={pack} bytes")
        print(f"  exhaustive worst accepted={worst:.3f} Q12; validation-fail-leaves={fails}")

    # The more hardware-useful pack: refine each corner independently, using an
    # implicit regular subgrid so runtime leaf selection is just local coordinate
    # high bits. Run the middle requested threshold by default.
    target=thresholds[len(thresholds)//2]
    cells,corner_count,fb,leaf_records,hist,worst,pack=analyse_per_corner_uniform(d,target,a.min_q4)
    print()
    print(f"per-corner regular refinement @ {target:g} Q12 (~{target*EDGE_PX_PER_Q12:.2f}px edge):")
    print(f"  cells={len(cells)} relevant-corner records={corner_count} fallback-corners={fb}")
    print(f"  depth histogram={dict(sorted(hist.items()))}; affine leaves={leaf_records}")
    print(f"  concrete straw-pack={pack} bytes (includes full 1152x uint16 cell-offset table)")
    print(f"  exhaustive worst accepted={worst:.3f} Q12")
    print("NOTE: per-corner refinement avoids duplicating nine-ish smooth corners because one nearby corner is nonlinear.")

    qcells,qcorners,qfb,qleaves,qhist,qworst,qpack=analyse_quantized_pack(d,target,a.min_q4)
    print()
    print(f"quantized runtime-shaped pack @ {target:g} Q12:")
    print("  leaf = baseQ12:uint16 + span-dx:int8 + span-dy:int8")
    print("  reconstruction = base + (dx*local_x >> log2(span)) + (dy*local_y >> log2(span))")
    print(f"  cells={len(qcells)} corner-records={qcorners} fallback-corners={qfb}")
    print(f"  depth histogram={dict(sorted(qhist.items()))}; affine leaves={qleaves}; concrete pack={qpack} bytes")
    print(f"  exhaustive worst accepted={qworst:.3f} Q12")
    print("Fallback corner records are exactly where smaller cells or exact special handling belongs; they are not ordinary runtime-general-math justification.")

if __name__=="__main__":main()
