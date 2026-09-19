#!/usr/bin/env python3
"""Bake the FULL-only scale-shock maze: 32 vertices, 28 walls, two diagonals."""
from __future__ import annotations
import argparse, collections, math, re
from pathlib import Path

WORLD_MIN_X,WORLD_MIN_Y,WORLD_MAX_X,WORLD_MAX_Y=8,8,144,104
CELL=8
COLS=(WORLD_MAX_X-WORLD_MIN_X)//CELL
ROWS=(WORLD_MAX_Y-WORLD_MIN_Y)//CELL
YAW_BINS=16
MASK_BYTES=4
FOV_HALF=math.radians(45.0)
CONE_HALF=FOV_HALF+math.pi/16.0+math.radians(2.0)
UNIFORM_RAYS=56
DEPTH_LAYERS=2
NEAR_ALWAYS=4.0
WALL_BLOCK_RADIUS=0.80
PLAYER_RADIUS_Q4=4
SPAWN_X,SPAWN_Y=18,92
FULL=0

# Four perimeter corners; two open snakes; a closed pentagonal island; two
# short doglegs. Only the island contributes non-orthogonal spans.
VERTS=[
 (8,8),(144,8),(144,104),(8,104),
 (28,14),(28,30),(46,30),(46,18),(76,18),(76,40),(60,40),
 (14,54),(36,54),(36,72),(56,72),(56,58),(78,58),(78,88),(92,88),
 (92,34),(116,34),(116,62),(102,62),(88,50),
 (124,14),(124,26),(102,26),(102,32),
 (138,78),(122,78),(122,94),(100,94),
]
SEGS=[
 (0,1),(1,2),(2,3),(3,0),
 (4,5),(5,6),(6,7),(7,8),(8,9),(9,10),
 (11,12),(12,13),(13,14),(14,15),(15,16),(16,17),(17,18),
 (19,20),(20,21),(21,22),(22,23),(23,19),
 (24,25),(25,26),(26,27),
 (28,29),(29,30),(30,31),
]
SHADE_BIAS=[0]*len(SEGS)

def pseg(px,py,ax,ay,bx,by):
    vx,vy=bx-ax,by-ay
    vv=vx*vx+vy*vy
    if vv<=1e-12:return math.hypot(px-ax,py-ay)
    t=max(0.0,min(1.0,((px-ax)*vx+(py-ay)*vy)/vv))
    return math.hypot(px-(ax+t*vx),py-(ay+t*vy))

def walkable():
    w=WORLD_MAX_X-WORLD_MIN_X+1; h=WORLD_MAX_Y-WORLD_MIN_Y+1
    raw=[[True]*w for _ in range(h)]
    for j,y in enumerate(range(WORLD_MIN_Y,WORLD_MAX_Y+1)):
        for i,x in enumerate(range(WORLD_MIN_X,WORLD_MAX_X+1)):
            if x<=WORLD_MIN_X or x>=WORLD_MAX_X or y<=WORLD_MIN_Y or y>=WORLD_MAX_Y:
                raw[j][i]=False; continue
            for a,b in SEGS:
                ax,ay=VERTS[a]; bx,by=VERTS[b]
                if pseg(x,y,ax,ay,bx,by)<=WALL_BLOCK_RADIUS:
                    raw[j][i]=False; break
    sx,sy=SPAWN_X-WORLD_MIN_X,SPAWN_Y-WORLD_MIN_Y
    if not raw[sy][sx]:raise SystemExit("spawn is blocked")
    seen=[[False]*w for _ in range(h)]
    q=collections.deque([(sx,sy)]); seen[sy][sx]=True
    while q:
        x,y=q.popleft()
        for nx,ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)):
            if 0<=nx<w and 0<=ny<h and raw[ny][nx] and not seen[ny][nx]:
                seen[ny][nx]=True; q.append((nx,ny))
    return seen

def floor_runs(walk):
    offs=[0]; runs=[]
    for row in walk:
        x=0
        while x<len(row):
            if not row[x]:x+=1;continue
            x0=x
            while x+1<len(row) and row[x+1]:x+=1
            runs.append((WORLD_MIN_X+x0,WORLD_MIN_X+x,0));x+=1
        offs.append(len(runs))
    return offs,runs

def floor_at(x,y,walk):
    xi=int(math.floor(x)); yi=int(math.floor(y))
    return (WORLD_MIN_X<=xi<=WORLD_MAX_X and WORLD_MIN_Y<=yi<=WORLD_MAX_Y
            and walk[yi-WORLD_MIN_Y][xi-WORLD_MIN_X])

def adelta(a,b):return (a-b+math.pi)%(2.0*math.pi)-math.pi

def hits(px,py,ang):
    dx,dy=math.cos(ang),math.sin(ang); out=[]
    for sid,(a,b) in enumerate(SEGS):
        ax,ay=VERTS[a]; bx,by=VERTS[b]
        sx,sy=bx-ax,by-ay; den=dx*sy-dy*sx
        if abs(den)<1e-10:continue
        qx,qy=ax-px,ay-py
        t=(qx*sy-qy*sx)/den; u=(qx*dy-qy*dx)/den
        if t>1e-6 and -1e-8<=u<=1.0+1e-8:out.append((t,sid))
    out.sort(key=lambda q:q[0]); return out

def event_angles(px,py,yaw):
    out=[]
    for a,b in SEGS:
        ax,ay=VERTS[a]; bx,by=VERTS[b]
        for sx,sy in ((ax,ay),(bx,by),((ax+bx)*.5,(ay+by)*.5)):
            ang=math.atan2(sy-py,sx-px)
            if abs(adelta(ang,yaw))<=CONE_HALF+1e-9:out.append(ang)
    return out

def candidates(px,py,yaw):
    mask=0
    angles=[yaw-CONE_HALF+2*CONE_HALF*(i+.5)/UNIFORM_RAYS for i in range(UNIFORM_RAYS)]
    angles.extend(event_angles(px,py,yaw))
    uniq={int(round((a%(2*math.pi))*65536/(2*math.pi))):a for a in angles}
    for ang in uniq.values():
        for _t,sid in hits(px,py,ang)[:DEPTH_LAYERS]:mask|=1<<sid
    for sid,(a,b) in enumerate(SEGS):
        ax,ay=VERTS[a]; bx,by=VERTS[b]
        if pseg(px,py,ax,ay,bx,by)<=NEAR_ALWAYS:mask|=1<<sid
    return mask

def bake_pvs(walk):
    masks=[]; counts=[]; empty=0
    for gy in range(ROWS):
      for gx in range(COLS):
        x0=WORLD_MIN_X+gx*CELL; y0=WORLD_MIN_Y+gy*CELL
        pts=((x0+4,y0+4),(x0+.5,y0+.5),(x0+7.5,y0+.5),(x0+.5,y0+7.5),(x0+7.5,y0+7.5))
        cams=[p for p in pts if floor_at(*p,walk)]
        for yb in range(YAW_BINS):
            yaw=2*math.pi*yb/YAW_BINS; mask=0
            for px,py in cams:mask|=candidates(px,py,yaw)
            if not cams:empty+=1
            masks.append(mask);counts.append(mask.bit_count())
    return masks,counts,empty

def q5_normal(a,b):
    ax,ay=VERTS[a]; bx,by=VERTS[b]; dx,dy=bx-ax,by-ay
    n=math.hypot(dx,dy)
    if n<1e-9:return 0,0
    return max(-32,min(32,round(32*dy/n))),max(-32,min(32,round(-32*dx/n)))

def extract(text,name):
    m=re.search(rf"static const\s+([^;=]+?)\s+{re.escape(name)}\s*\[[^\]]+\]\s*=\s*\{{.*?\}};",text,re.S)
    if not m:raise SystemExit(f"missing baseline array {name}")
    return text[m.start():m.end()]

def arr(ctype,name,vals,per=16):
    out=[f"static const {ctype} {name}[{len(vals)}] = {{"]
    for i in range(0,len(vals),per):out.append("    "+", ".join(map(str,vals[i:i+per]))+",")
    out.append("};");return "\n".join(out)

def write_floor(path,offs,runs):
    lines=["/* GENERATED by tools/full_maze_bake.py. */","#ifndef FULL_MAZE_FLOOR_H","#define FULL_MAZE_FLOOR_H",
      "#include <stdint.h>",f"#define E1X_WORLD_MIN_X {WORLD_MIN_X}",f"#define E1X_WORLD_MIN_Y {WORLD_MIN_Y}",
      f"#define E1X_WORLD_MAX_X {WORLD_MAX_X}",f"#define E1X_WORLD_MAX_Y {WORLD_MAX_Y}",
      f"#define E1X_PLAYER_RADIUS_Q4 {PLAYER_RADIUS_Q4}",
      "typedef struct E1XFloorRun { uint8_t x0,x1; int8_t z; } E1XFloorRun;",
      arr("uint16_t","k_e1x_floor_row_off",offs)]
    lines.append(f"static const E1XFloorRun k_e1x_floor_runs[{len(runs)}] = {{")
    for i in range(0,len(runs),8):lines.append("    "+", ".join("{%d,%d,%d}"%r for r in runs[i:i+8])+",")
    lines+=["};","#endif",""];path.parent.mkdir(parents=True,exist_ok=True);path.write_text("\n".join(lines))

def write_svg(path):
    sc=6;pad=6
    w=(WORLD_MAX_X-WORLD_MIN_X)*sc+2*pad;h=(WORLD_MAX_Y-WORLD_MIN_Y)*sc+2*pad
    px=lambda x:pad+(x-WORLD_MIN_X)*sc; py=lambda y:pad+(WORLD_MAX_Y-y)*sc
    s=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">',
       '<rect width="100%" height="100%" fill="#111"/>','<g stroke="#eee" stroke-width="5" fill="none">']
    for a,b in SEGS:
        ax,ay=VERTS[a];bx,by=VERTS[b];s.append(f'<line x1="{px(ax)}" y1="{py(ay)}" x2="{px(bx)}" y2="{py(by)}"/>')
    s+=['</g>',f'<circle cx="{px(SPAWN_X)}" cy="{py(SPAWN_Y)}" r="7" fill="#5dd6ff"/>',
        f'<line x1="{px(SPAWN_X)}" y1="{py(SPAWN_Y)}" x2="{px(SPAWN_X+7)}" y2="{py(SPAWN_Y)}" stroke="#5dd6ff" stroke-width="3"/>','</svg>']
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text("\n".join(s)+"\n")

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--repo-root",default=".")
    ap.add_argument("--out",required=True);ap.add_argument("--floor-out",required=True);ap.add_argument("--preview-out")
    a=ap.parse_args()
    if len(VERTS)>32 or len(SEGS)>32:raise SystemExit("5-bit geometry budget exceeded")
    walk=walkable();offs,runs=floor_runs(walk);write_floor(Path(a.floor_out),offs,runs)
    if a.preview_out:write_svg(Path(a.preview_out))
    masks,counts,empty=bake_pvs(walk)
    root=Path(a.repo_root)
    base="\n".join((root/f"src/generated/tilesector_polar_data_part0{i}.inc").read_text() for i in range(5))
    keys=[];anchors=[];nx=[];ny=[]
    for sid,(v0,v1) in enumerate(SEGS):
        keys.append(sid|(v0<<5)|(v1<<10));anchors.append(v0)
        qx,qy=q5_normal(v0,v1);nx.append(qx);ny.append(qy)
    pvs=[]
    for m in masks:pvs.extend((m>>(8*i))&255 for i in range(MASK_BYTES))
    sections=["/* GENERATED by tools/full_maze_bake.py. */",
      f"#define TSPF_KEY_COUNT {len(keys)}u","#define TSPF_SELECTOR_COUNT 1u","#define TSPF_BASE_COUNT 1u","#define TSPF_RECIPE_COUNT 1u",
      f"#define FULLMAZE_WORLD_MIN_X {WORLD_MIN_X}u",f"#define FULLMAZE_WORLD_MIN_Y {WORLD_MIN_Y}u",
      f"#define FULLMAZE_PVS_COLS {COLS}u",f"#define FULLMAZE_PVS_ROWS {ROWS}u",f"#define FULLMAZE_PVS_YAW_BINS {YAW_BINS}u",f"#define FULLMAZE_PVS_MASK_BYTES {MASK_BYTES}u",
      arr("uint16_t","k_tspf_keys",keys,10),arr("int8_t","k_tspf_sel_a",[0]),arr("int8_t","k_tspf_sel_b",[0]),
      arr("int16_t","k_tspf_sel_c",[0]),arr("uint8_t","k_tspf_sel_inv",[0]),arr("uint8_t","k_tspf_ao",[0]*len(VERTS),20),
      arr("uint16_t","k_tspf_base_off",[0]),arr("uint8_t","k_tspf_base_stream",[0]),arr("uint16_t","k_tspf_recipe_off",[0]),
      arr("uint8_t","k_tspf_recipe_stream",[0,0]),arr("uint8_t","k_tspf_recipe_grid",[255]),
      arr("uint8_t","k_tspf_vx",[x for x,_ in VERTS],20),arr("uint8_t","k_tspf_vy",[y for _,y in VERTS],20),
      arr("uint8_t","k_tspf_seg_anchor",anchors,20),arr("int8_t","k_tspf_nx_q5",nx,20),arr("int8_t","k_tspf_ny_q5",ny,20),
      arr("uint8_t","k_tspf_profile",[FULL]*len(SEGS),20),arr("int8_t","k_tspf_shade_bias",SHADE_BIAS,20),arr("uint8_t","k_fullmaze_pvs",pvs,24)]
    for name in ("k_tspf_recip8_q16","k_tspf_atan_q12","k_tspf_angle_x_pos","k_tspf_sec_q7","k_tspf_sin_q7","k_tspf_invz"):
        sections.append(extract(base,name))
    out=Path(a.out);out.parent.mkdir(parents=True,exist_ok=True);out.write_text("\n\n".join(sections)+"\n")
    cs=sorted(counts);p95=cs[(len(cs)*95)//100]
    wc=sum(sum(row) for row in walk)
    print(f"FULL_MAZE_BAKE_PASS vertices={len(VERTS)} surfaces={len(SEGS)} FULL={len(SEGS)} diagonals=2")
    print(f"world={WORLD_MIN_X},{WORLD_MIN_Y}..{WORLD_MAX_X},{WORLD_MAX_Y} spawn={SPAWN_X},{SPAWN_Y} walkable_cells={wc} floor_runs={len(runs)}")
    print(f"pvs cells={COLS*ROWS} yaw_bins={YAW_BINS} bytes={len(pvs)} candidate_mean={sum(counts)/len(counts):.2f} p95={p95} min={min(counts)} max={max(counts)} empty={empty}")
    for sid,(v0,v1) in enumerate(SEGS):print(f"surface {sid:02d} v{v0}->v{v1} xy={VERTS[v0]}->{VERTS[v1]} FULL")
if __name__=="__main__":main()
