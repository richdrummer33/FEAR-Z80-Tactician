#!/usr/bin/env python3
"""Trace dominant axis-aligned walls from the extracted Kleiner-lab OBJ."""
import argparse
import json
import math
from pathlib import Path

def parse_obj(path):
    verts=[]
    faces=[]
    mat="UNKNOWN"
    for raw in Path(path).read_text().splitlines():
        if raw.startswith("v "):
            _,x,y,z=raw.split()
            verts.append((float(x),float(y),float(z)))
        elif raw.startswith("usemtl "):
            mat=raw.split(None,1)[1]
        elif raw.startswith("f "):
            idx=[int(tok.split("/")[0])-1 for tok in raw.split()[1:]]
            if len(idx)>=3:
                for k in range(1,len(idx)-1):
                    faces.append((idx[0],idx[k],idx[k+1],mat))
    return verts,faces

def lerp(a,b,t):
    return a+(b-a)*t

def triangle_slice(v0,v1,v2,z):
    pts=[v0,v1,v2]
    dz=[p[2]-z for p in pts]
    if min(dz)>1e-8 or max(dz)<-1e-8:
        return None
    hit=[]
    for i,p in enumerate(pts):
        if abs(dz[i])<=1e-8:
            hit.append((p[0],p[1]))
    for i,j in ((0,1),(1,2),(2,0)):
        if dz[i]*dz[j] < -1e-12:
            t=(z-pts[i][2])/(pts[j][2]-pts[i][2])
            hit.append((lerp(pts[i][0],pts[j][0],t),
                        lerp(pts[i][1],pts[j][1],t)))
    uniq=[]
    for p in hit:
        if not any(math.hypot(p[0]-q[0],p[1]-q[1])<1e-6 for q in uniq):
            uniq.append(p)
    if len(uniq)<2:
        return None
    best=None
    best_d=-1.0
    for i,a in enumerate(uniq):
        for b in uniq[i+1:]:
            d=math.hypot(a[0]-b[0],a[1]-b[1])
            if d>best_d:
                best=(a,b)
                best_d=d
    return best

def merge_spans(items,gap):
    out=[]
    for lo,hi in sorted((min(a,b),max(a,b)) for a,b in items):
        if not out or lo>out[-1][1]+gap:
            out.append([lo,hi])
        else:
            out[-1][1]=max(out[-1][1],hi)
    return out

def trace(verts,faces,z,tol,gap,min_len,ox,oy):
    xplanes={}
    yplanes={}
    for ia,ib,ic,_mat in faces:
        seg=triangle_slice(verts[ia],verts[ib],verts[ic],z)
        if not seg:
            continue
        a,b=seg
        dx=abs(a[0]-b[0])
        dy=abs(a[1]-b[1])
        if dx<=tol and dy>=min_len:
            key=round((a[0]+b[0])*0.5,3)
            xplanes.setdefault(key,[]).append((a[1],b[1]))
        elif dy<=tol and dx>=min_len:
            key=round((a[1]+b[1])*0.5,3)
            yplanes.setdefault(key,[]).append((a[0],b[0]))

    rows=[]
    for x,spans in xplanes.items():
        for lo,hi in merge_spans(spans,gap):
            if hi-lo<min_len: continue
            rows.append({"axis":"x","plane_obj":x,"span_obj":[lo,hi],
                         "plane_world":x+ox,"span_world":[lo+oy,hi+oy],
                         "length":hi-lo})
    for y,spans in yplanes.items():
        for lo,hi in merge_spans(spans,gap):
            if hi-lo<min_len: continue
            rows.append({"axis":"y","plane_obj":y,"span_obj":[lo,hi],
                         "plane_world":y+oy,"span_world":[lo+ox,hi+ox],
                         "length":hi-lo})
    rows.sort(key=lambda r:(r["axis"],-r["length"],r["plane_obj"]))
    return rows

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("--z",type=float,action="append",default=[])
    ap.add_argument("--offset-x",type=float,default=36.0)
    ap.add_argument("--offset-y",type=float,default=-16.0)
    ap.add_argument("--axis-tol",type=float,default=0.03)
    ap.add_argument("--merge-gap",type=float,default=0.08)
    ap.add_argument("--min-span",type=float,default=3.0)
    ap.add_argument("--json")
    args=ap.parse_args()

    verts,faces=parse_obj(args.obj)
    zs=args.z or [6.0,16.0,28.0]
    result={}
    for z in zs:
        rows=trace(verts,faces,z,args.axis_tol,args.merge_gap,args.min_span,
                   args.offset_x,args.offset_y)
        result[str(z)]=rows
        print(f"SLICE z={z:g}")
        for r in rows:
            print(f"  {r['axis']}={r['plane_obj']:.3f} "
                  f"span={r['span_obj'][0]:.3f}..{r['span_obj'][1]:.3f} "
                  f"-> world {r['axis']}={r['plane_world']:.3f} "
                  f"span={r['span_world'][0]:.3f}..{r['span_world'][1]:.3f} "
                  f"len={r['length']:.3f}")
    if args.json:
        Path(args.json).write_text(json.dumps(result,indent=2)+"\n")
    print(f"HL2_LAB_TRACE_PASS vertices={len(verts)} faces={len(faces)} slices={len(zs)}")

if __name__=="__main__":
    main()
