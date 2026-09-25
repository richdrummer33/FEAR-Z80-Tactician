#!/usr/bin/env python3
"""Validate direct mixed-cell handoffs against continuous authored geometry."""
from __future__ import annotations
import argparse,csv,math,pathlib,re,statistics
from collections import defaultdict

FOCAL=80.0
W=160.0

def arr(text,name):
    m=re.search(r"static\s+const\s+[^;=]+?\b"+re.escape(name)+r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};",text,re.S)
    if not m: raise SystemExit("missing generated array "+name)
    return [int(x,0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+",m.group(1))]

def pct(v,p):
    if not v:return float("nan")
    q=sorted(v);return q[int((len(q)-1)*p)]

def cross(ax,ay,bx,by):return ax*by-ay*bx

def derive_segments(keys,vx,vy):
    bysid=defaultdict(set)
    for w in keys:
        sid=w&31;v0=(w>>5)&31;v1=(w>>10)&31
        if v0<len(vx) and v1<len(vx) and v0!=v1:
            bysid[sid].add(tuple(sorted((v0,v1))))
    bad={s:p for s,p in bysid.items() if len(p)!=1}
    if bad:raise SystemExit("surface endpoint ambiguity "+repr(bad))
    out=[]
    for sid in sorted(bysid):
        v0,v1=next(iter(bysid[sid]))
        out.append((sid,v0,v1,float(vx[v0]),float(vy[v0]),float(vx[v1]),float(vy[v1])))
    return out

def ray_owner(cx,cy,fx,fy,rx,ry,sx,segs):
    u=(sx-80.0)/FOCAL;dx=fx+u*rx;dy=fy+u*ry
    bt=1e30;bo=None
    for sid,v0,v1,ax,ay,bx,by in segs:
        ex=bx-ax;ey=by-ay;den=cross(dx,dy,ex,ey)
        if abs(den)<1e-10:continue
        qx=ax-cx;qy=ay-cy
        t=cross(qx,qy,ex,ey)/den
        uu=cross(qx,qy,dx,dy)/den
        if t>1e-7 and -1e-9<=uu<=1.0+1e-9 and t<bt:bt=t;bo=sid
    return bo

def project_vertex(vid,cx,cy,yaw,vx,vy):
    dx=float(vx[vid])-cx;dy=float(vy[vid])-cy
    a=(math.atan2(dy,dx)-yaw+math.pi)%(2.0*math.pi)-math.pi
    if math.cos(a)<=0:return None
    return 80.0+FOCAL*math.tan(a)

def oracle_events(fr,vx,vy,segs):
    cx=int(fr["x_q4"])/16.0;cy=int(fr["y_q4"])/16.0
    yaw=int(fr["yaw"])*2.0*math.pi/256.0
    fx,fy=math.cos(yaw),math.sin(yaw);rx,ry=-fy,fx
    step=.125;n=int(W/step)
    runs=[];prev=None;start=0
    for i in range(n):
        o=ray_owner(cx,cy,fx,fy,rx,ry,(i+.5)*step,segs)
        if i==0:prev=o
        elif o!=prev:
            runs.append((prev,start,i-1));prev=o;start=i
    runs.append((prev,start,n-1))
    ends={sid:{v0,v1} for sid,v0,v1,*_ in segs}
    out=[]
    for a,b in zip(runs,runs[1:]):
        lo,_,li=a;ro,ri,_=b
        x=((li+1)*step+ri*step)*.5;vid=None
        if lo is not None and ro is not None:
            common=ends.get(lo,set())&ends.get(ro,set())
            if len(common)==1:
                vid=next(iter(common))
                px=project_vertex(vid,cx,cy,yaw,vx,vy)
                if px is not None:x=px
        if 0.0<=x<160.0 and lo!=ro and abs(x-round(x/8.0)*8.0)>.5:
            out.append({"x":x,"left":lo,"right":ro,"vid":vid})
    return out

def owner(v):
    v=int(v)
    return None if v==255 else (v&31)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--map-inc",required=True)
    ap.add_argument("--frames",required=True)
    ap.add_argument("--events",required=True)
    ap.add_argument("--label",default="trace")
    a=ap.parse_args()
    text=pathlib.Path(a.map_inc).read_text()
    vx=arr(text,"k_tspf_vx");vy=arr(text,"k_tspf_vy");keys=arr(text,"k_tspf_keys")
    segs=derive_segments(keys,vx,vy)
    frames=list(csv.DictReader(open(a.frames,newline="")))
    runtime=defaultdict(list)
    with open(a.events,newline="") as f:
        for r in csv.DictReader(f):
            runtime[int(r["frame"])].append({
                "x":int(r["x"]),"left":owner(r["left"]),"right":owner(r["right"])
            })

    total=matched=missing=extra=reversed_pairs=0
    xerr=[];recall=[];crowd_o=crowd_r=0;worst=[]
    for fi,fr in enumerate(frames):
        ex=oracle_events(fr,vx,vy,segs);rt=runtime.get(fi,[])
        total+=len(ex)
        og=defaultdict(int);rg=defaultdict(int)
        for e in ex:og[int(e["x"])>>3]+=1
        for r in rt:rg[r["x"]>>3]+=1
        crowd_o+=sum(1 for n in og.values() if n>=3)
        crowd_r+=sum(1 for n in rg.values() if n>=3)
        used=set();fm=0
        for e in ex:
            cand=[]
            for j,r in enumerate(rt):
                if j in used:continue
                ordered=r["left"]==e["left"] and r["right"]==e["right"]
                rev=r["left"]==e["right"] and r["right"]==e["left"]
                if not (ordered or rev):continue
                cand.append((0 if ordered else 1,abs(r["x"]-e["x"]),j,r))
            if not cand:
                missing+=1;continue
            order_bad,dx,j,r=min(cand)
            used.add(j);matched+=1;fm+=1;reversed_pairs+=order_bad
            xerr.append(dx);worst.append((dx,fi,e,r))
        extra+=len(rt)-len(used)
        recall.append(100.0*fm/len(ex) if ex else 100.0)

    print(f"DIRECT_MIXED_EVENT_CENSUS label={a.label} frames={len(frames)}")
    print(f"events oracle={total} matched={matched} missing={missing} extra={extra} "
          f"recall={(100.0*matched/total if total else 100.0):.2f}% "
          f"reversed_owner_pairs={reversed_pairs}")
    if xerr:
        print(f"x_abs_error n={len(xerr)} mean={statistics.fmean(xerr):.3f}px "
              f"p50={pct(xerr,.50):.3f}px p95={pct(xerr,.95):.3f}px max={max(xerr):.3f}px")
    print(f"frame_recall mean={statistics.fmean(recall):.2f}% p05={pct(recall,.05):.2f}%")
    print(f"crowded_tiles_3plus oracle={crowd_o} runtime={crowd_r}")
    for dx,fi,e,r in sorted(worst,key=lambda z:z[0],reverse=True)[:12]:
        print(f"XOFFENDER frame={fi} err={dx:.3f}px exact_x={e['x']:.3f} runtime_x={r['x']} "
              f"exact={e['left']}->{e['right']} runtime={r['left']}->{r['right']} "
              f"oracle_vid={e['vid']}")

if __name__=="__main__":
    main()
