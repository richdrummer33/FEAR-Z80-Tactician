#!/usr/bin/env python3
"""Compare the GG thin-face result with an independent continuous 2.5D oracle.

The oracle does not use coarse column ownership, the envelope bake, inverse-Z
tables, or retained renderer state. It ray/segment intersects the authored 2D
walls directly, projects shared physical vertices with tan(), and projects FULL
wall height as 1280 / forward_depth. That gives us a useful control group for
the remaining one-cell accordion/flicker artifacts.
"""
from __future__ import annotations
import argparse, csv, math, pathlib, re, statistics
from collections import Counter, defaultdict

COLS, ROWS = 20, 18
W, H = 160.0, 144.0
FOCAL = 80.0
TILE_MASK = 0x01ff
HFLIP = 0x0200
SEAM_BASE = 412
SEAM_COUNT = 20

def arr(text: str, name: str):
    m=re.search(r"static\s+const\s+[^;=]+?\b"+re.escape(name)+r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};",text,re.S)
    if not m:
        raise SystemExit("missing generated array "+name)
    return [int(x,0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+",m.group(1))]

def pct(v,p):
    if not v: return float("nan")
    q=sorted(v)
    return q[int((len(q)-1)*p)]

def stats(name,v,unit=""):
    if not v:
        print(f"{name}: n=0")
        return
    print(f"{name}: n={len(v)} mean={statistics.fmean(v):.3f}{unit} "
          f"p50={pct(v,.50):.3f}{unit} p95={pct(v,.95):.3f}{unit} max={max(v):.3f}{unit}")

def cross(ax,ay,bx,by):
    return ax*by-ay*bx

def derive_segments(keys,vx,vy):
    bysid=defaultdict(set)
    for w in keys:
        sid=w&31
        v0=(w>>5)&31
        v1=(w>>10)&31
        if v0>=len(vx) or v1>=len(vx) or v0==v1:
            continue
        bysid[sid].add(tuple(sorted((v0,v1))))
    conflicts={sid:p for sid,p in bysid.items() if len(p)!=1}
    if conflicts:
        raise SystemExit("surface endpoint ambiguity: "+repr(conflicts))
    seg=[]
    for sid in sorted(bysid):
        v0,v1=next(iter(bysid[sid]))
        seg.append((sid,v0,v1,float(vx[v0]),float(vy[v0]),float(vx[v1]),float(vy[v1])))
    return seg

def ray_owner(camx,camy,fx,fy,rx,ry,screen_x,segs):
    u=(screen_x-80.0)/FOCAL
    dx=fx+u*rx
    dy=fy+u*ry
    best_t=1e30
    best=None
    for sid,v0,v1,ax,ay,bx,by in segs:
        sx=bx-ax; sy=by-ay
        den=cross(dx,dy,sx,sy)
        if abs(den)<1e-10: continue
        qx=ax-camx; qy=ay-camy
        t=cross(qx,qy,sx,sy)/den
        uu=cross(qx,qy,dx,dy)/den
        if t>1e-7 and uu>=-1e-9 and uu<=1.0+1e-9 and t<best_t:
            best_t=t; best=sid
    return best

def project_vertex(vid,camx,camy,yaw,vx,vy):
    dx=float(vx[vid])-camx
    dy=float(vy[vid])-camy
    ang=math.atan2(dy,dx)-yaw
    ang=(ang+math.pi)%(2.0*math.pi)-math.pi
    c=math.cos(ang)
    if c<=0.0: return None
    x=80.0+FOCAL*math.tan(ang)
    forward=math.hypot(dx,dy)*c
    return x,forward

def exact_chain(frame,vx,vy,segs):
    camx=int(frame["x_q4"])/16.0
    camy=int(frame["y_q4"])/16.0
    yaw=int(frame["yaw"])*2.0*math.pi/256.0
    fx,fy=math.cos(yaw),math.sin(yaw)
    rx,ry=-fy,fx
    step=.125
    samples=int(W/step)
    owners=[]
    prev=None
    start=0
    for i in range(samples):
        x=(i+.5)*step
        o=ray_owner(camx,camy,fx,fy,rx,ry,x,segs)
        if i==0:
            prev=o; start=0
        elif o!=prev:
            owners.append((prev,start,i-1))
            prev=o; start=i
    owners.append((prev,start,samples-1))

    endpoints={sid:{v0,v1} for sid,v0,v1,*_ in segs}
    events=[]
    for a,b in zip(owners,owners[1:]):
        ls,_,li=a; rs,ri,_=b
        approx=((li+1)*step+ri*step)*.5
        vid=None; x=approx; depth=None
        if ls is not None and rs is not None:
            common=endpoints.get(ls,set()) & endpoints.get(rs,set())
            if len(common)==1:
                vid=next(iter(common))
                p=project_vertex(vid,camx,camy,yaw,vx,vy)
                if p is not None:
                    x,depth=p
        if -1.0<=x<=161.0:
            events.append({"x":x,"vid":vid,"left":ls,"right":rs,"depth":depth})

    # Remove sampling chatter at exact ties, retaining the physical ordering.
    clean=[]
    for e in events:
        if clean and abs(e["x"]-clean[-1]["x"])<.20 and e["left"]==clean[-1]["left"] and e["right"]==clean[-1]["right"]:
            continue
        clean.append(e)
    return clean

def seam_mask_from_code(code):
    if code<4: return 1<<code
    if code<=10:
        y=code-3; return (1<<0)|(1<<y)
    if code<=15:
        y=code-9; return (1<<1)|(1<<y)
    if code<=18:
        y=code-13; return (1<<2)|(1<<y)
    if code==19: return (1<<3)|(1<<4)
    return 0

def map_lines(buf,fi):
    base=fi*COLS*ROWS*2
    support=Counter()
    for row in range(ROWS):
        for col in range(COLS):
            o=base+2*(row*COLS+col)
            w=buf[o] | (buf[o+1]<<8)
            tid=w&TILE_MASK
            mask=0
            if 3<=tid<7:
                border=tid-3
                if border&1: mask|=0x01
                if border&2: mask|=0x80
            elif SEAM_BASE<=tid<SEAM_BASE+SEAM_COUNT:
                mask=seam_mask_from_code(tid-SEAM_BASE)
                if w&HFLIP:
                    rm=0
                    for b in range(8):
                        if mask&(1<<b): rm|=1<<(7-b)
                    mask=rm
            for b in range(8):
                if mask&(1<<b):
                    support[col*8+b]+=1
    return support

def interior_rows(half):
    # Exact copy of the current overlay's FULL-interior eligibility.
    top=71-int(half)
    top_tile=math.floor(top/8)
    first=max(0,top_tile+1)
    last=min(17,16-top_tile)
    return max(0,last-first+1)

def depth_bin(z):
    if z is None: return "unknown"
    if z<16: return "<16"
    if z<32: return "16-32"
    if z<64: return "32-64"
    if z<96: return "64-96"
    return ">=96"

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--map-inc",required=True)
    ap.add_argument("--frames",required=True)
    ap.add_argument("--seams",required=True)
    ap.add_argument("--map-bin",required=True)
    ap.add_argument("--label",default="trace")
    a=ap.parse_args()

    text=pathlib.Path(a.map_inc).read_text()
    vx=arr(text,"k_tspf_vx")
    vy=arr(text,"k_tspf_vy")
    keys=arr(text,"k_tspf_keys")
    segs=derive_segments(keys,vx,vy)
    frames=list(csv.DictReader(open(a.frames,newline="")))
    seamrows=defaultdict(list)
    with open(a.seams,newline="") as f:
        for r in csv.DictReader(f):
            seamrows[int(r["frame"])].append({
                "x":int(r["x"]),"vid":int(r["vid"]),"half":int(r["half"])
            })
    mb=pathlib.Path(a.map_bin).read_bytes()
    expect=len(frames)*COLS*ROWS*2
    if len(mb)<expect:
        raise SystemExit(f"short map dump: {len(mb)} < {expect}")

    print(f"EXACT_PROJECTION_CENSUS label={a.label} frames={len(frames)} vertices={len(vx)} surfaces={len(segs)}")
    xerr=[]; yerr=[]; ypixerr=[]
    exact_events=matched=missing=extra=0
    errors_by_depth=defaultdict(list)
    missing_by_depth=Counter(); events_by_depth=Counter()
    no_interior=one_interior=descriptor_total=descriptor_draw_miss=0
    crowded_tiles=crowded_frames=0
    ghost_lines=ghost_coarse=strong_lines=0
    width_err=[]; narrow_err=[]; narrow_total=narrow_missing=0
    temporal=[]; opposite=coarse_jump=0
    prev_widths={}

    for fi,fr in enumerate(frames):
        ev=exact_chain(fr,vx,vy,segs)
        exact_x=[e["x"] for e in ev]
        cur=seamrows.get(fi,[])
        support=map_lines(mb,fi)

        # Tile crowding in the descriptors before the two-line tile encoder.
        groups=defaultdict(set)
        for s in cur:
            groups[s["x"]>>3].add(s["x"]&7)
            descriptor_total+=1
            ir=interior_rows(s["half"])
            if ir==0: no_interior+=1
            elif ir==1: one_interior+=1
            if support.get(s["x"],0)==0: descriptor_draw_miss+=1
        bad=sum(1 for bits in groups.values() if len(bits)>2)
        crowded_tiles+=bad
        crowded_frames+=int(bad>0)

        # Decoded vertical line signal: anything strong but away from a true
        # visibility transition is a ghost/snap line.
        for x,nrows in support.items():
            if nrows<2: continue
            strong_lines+=1
            if not exact_x or min(abs(x-ex) for ex in exact_x)>1.25:
                ghost_lines+=1
                if (x&7) in (0,7): ghost_coarse+=1

        # Shared physical corners are the apples-to-apples event set.
        shared=[e for e in ev if e["vid"] is not None and e["depth"] is not None and 0.0<=e["x"]<160.0]
        used=set()
        match_by_vid={}
        camx=int(fr["x_q4"])/16.0; camy=int(fr["y_q4"])/16.0
        yaw=int(fr["yaw"])*2.0*math.pi/256.0
        for e in shared:
            exact_events+=1
            db=depth_bin(e["depth"]); events_by_depth[db]+=1
            candidates=[(abs(s["x"]-e["x"]),j,s) for j,s in enumerate(cur)
                        if j not in used and s["vid"]==e["vid"]]
            if not candidates:
                missing+=1; missing_by_depth[db]+=1
                continue
            dx,j,s=min(candidates)
            used.add(j); matched+=1
            match_by_vid[e["vid"]]=(float(s["x"]),e["x"])
            xerr.append(dx); errors_by_depth[db].append(dx)
            # Continuous vertical reference at the same physical vertex.
            half_exact=1280.0/e["depth"]
            top_exact=71.5-half_exact
            top_cur=71.0-float(s["half"])
            yerr.append(abs(top_cur-top_exact))
            ypixerr.append(abs(top_cur-round(top_exact)))
        extra += max(0,len(cur)-len(used))

        # Visible segment lengths between adjacent connected-corner events.
        widths={}
        for le,re in zip(shared,shared[1:]):
            sid=le["right"]
            if sid is None or sid!=re["left"]: continue
            if le["vid"]==re["vid"]: continue
            ew=re["x"]-le["x"]
            if ew<=0: continue
            key=(sid,le["vid"],re["vid"])
            lm=match_by_vid.get(le["vid"]); rm=match_by_vid.get(re["vid"])
            if lm is None or rm is None:
                if ew<8.0:
                    narrow_total+=1; narrow_missing+=1
                continue
            cw=rm[0]-lm[0]
            if cw<0: continue
            width_err.append(abs(cw-ew))
            widths[key]=(ew,cw)
            if ew<8.0:
                narrow_total+=1
                narrow_err.append(abs(cw-ew))

        for key,(ew,cw) in widths.items():
            if key in prev_widths:
                pe,pc=prev_widths[key]
                de=ew-pe; dc=cw-pc
                temporal.append(abs(dc-de))
                if abs(de)>.20 and de*dc<0: opposite+=1
                if abs(dc)>=4.0 and abs(de)<2.0: coarse_jump+=1
        prev_widths=widths

    print(f"corner_events exact={exact_events} matched={matched} missing={missing} recall={(100*matched/exact_events if exact_events else 0):.2f}% extra_descriptors={extra}")
    stats("corner_x_abs_error",xerr,"px")
    stats("corner_top_abs_error_continuous",yerr,"px")
    stats("corner_top_error_vs_nearest_pixel",ypixerr,"px")
    for db in ("<16","16-32","32-64","64-96",">=96"):
        n=events_by_depth[db]; m=missing_by_depth[db]
        if n:
            print(f"depth {db}: events={n} missing={m} recall={100*(n-m)/n:.2f}% x_p95={pct(errors_by_depth[db],.95):.3f}px")

    print(f"overlay_descriptor_rows total={descriptor_total} no_FULL_interior={no_interior} ({100*no_interior/descriptor_total if descriptor_total else 0:.2f}%) "
          f"one_FULL_row={one_interior} draw_miss={descriptor_draw_miss} ({100*descriptor_draw_miss/descriptor_total if descriptor_total else 0:.2f}%)")
    print(f"tile_capacity crowded_tiles_3plus={crowded_tiles} crowded_frames={crowded_frames}/{len(frames)}")
    print(f"decoded_vertical_lines strong={strong_lines} ghosts={ghost_lines} ({100*ghost_lines/strong_lines if strong_lines else 0:.2f}%) "
          f"ghosts_on_tile_edges={ghost_coarse}")

    stats("segment_width_abs_error",width_err,"px")
    print(f"narrow_spans_lt8 exact={narrow_total} missing={narrow_missing} ({100*narrow_missing/narrow_total if narrow_total else 0:.2f}%)")
    stats("narrow_span_width_abs_error",narrow_err,"px")
    stats("temporal_width_delta_error",temporal,"px/update")
    print(f"temporal_width opposite_direction={opposite} coarse_jump_ge4_when_exact_lt2={coarse_jump}")

if __name__=="__main__":
    main()
