#!/usr/bin/env python3
"""Bake the exact marked E1M1 Room-1 slice for the latest retained Polar renderer.

Source geometry/floor are the exact generated Room-1 oracle originally derived
from map1.js.  This variant deliberately deletes every vertical special case:
all structural/occluding XY spans become one centered FULL wall, z=0..10.
No windows, stairs, risers, raised/lintel bands, or floor-height changes remain.

The PVS is yaw-aware and conservative: for each 8x8 world cell and 16 yaw bins,
it unions first/second ray hits from several camera samples, authored endpoint
and midpoint event rays, plus a near-wall safety ring.  Runtime therefore pays
only for a compact 30-bit candidate mask, then uses the current renderer's
projection, sorting, retained exact-key gate, and Rung-27 FULL boundary patcher.
"""
from __future__ import annotations
import argparse, math, re
from pathlib import Path

WORLD_MIN_X=16
WORLD_MIN_Y=24
WORLD_MAX_X=112
WORLD_MAX_Y=80
CELL=8
COLS=(WORLD_MAX_X-WORLD_MIN_X)//CELL
ROWS=(WORLD_MAX_Y-WORLD_MIN_Y)//CELL
YAW_BINS=16
MASK_BYTES=4
FOV_HALF=math.radians(45.0)
YAW_HALF_BUCKET=math.pi/16.0
CONE_HALF=FOV_HALF+YAW_HALF_BUCKET+math.radians(2.0)
UNIFORM_RAYS=56
DEPTH_LAYERS=2
NEAR_ALWAYS=4.0
EYE_HEIGHT=5.0
FULL=0
LINTEL=1
RISER=3
WINDOW_SOURCE_IDS=(19,46)
# Source geometry 50..57 are the two closed square pillars. They are interior
# solids (holes in walkable space), so their visible/front side is opposite an
# outer world-boundary ring. The original E1M1 extraction preserved the outer
# winding and therefore made the pillars effectively inside-out to the one-sided
# first-hit renderer.
INTERIOR_SOLID_SOURCE_IDS=frozenset(range(50,58))
INTERIOR_SOLID_AABBS=((56,32,64,40),(56,64,64,72))

def parse_geometry(path: Path):
    text=path.read_text()
    vm=re.search(r"k_e1x_vertices\[[^\]]+\]\s*=\s*\{(.*?)\};",text,re.S)
    sm=re.search(r"k_e1x_segments\[[^\]]+\]\s*=\s*\{(.*?)\};",text,re.S)
    if not vm or not sm:
        raise SystemExit("could not parse exact geometry header")
    verts=[tuple(map(int,m)) for m in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}",vm.group(1))]
    segs=[]
    for m in re.finditer(
        r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*\}",
        sm.group(1)):
        segs.append(tuple(map(int,m.groups())))
    if len(verts)!=44 or len(segs)!=58:
        raise SystemExit(f"unexpected exact geometry: verts={len(verts)} segs={len(segs)}")
    return verts,segs

def parse_floor(path: Path):
    text=path.read_text()
    om=re.search(r"k_e1x_floor_row_off\[[^\]]+\]\s*=\s*\{(.*?)\};",text,re.S)
    rm=re.search(r"k_e1x_floor_runs\[[^\]]+\]\s*=\s*\{(.*?)\};",text,re.S)
    if not om or not rm:
        raise SystemExit("could not parse exact floor header")
    offs=[int(x) for x in re.findall(r"-?\d+",om.group(1))]
    runs=[tuple(map(int,m)) for m in re.findall(
        r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}",rm.group(1))]
    if len(offs)!=58 or len(runs)!=174:
        raise SystemExit(f"unexpected floor data: offsets={len(offs)} runs={len(runs)}")
    return offs,runs

def floor_at(x: float,y: float,offs,runs):
    # Pillars are true filled convex obstacles, not four independent render
    # faces laid over otherwise-walkable floor.
    for x0,y0,x1,y1 in INTERIOR_SOLID_AABBS:
        if x0 <= x <= x1 and y0 <= y <= y1:
            return None
    xi=int(math.floor(x)); yi=int(math.floor(y))
    if xi<WORLD_MIN_X or xi>WORLD_MAX_X or yi<WORLD_MIN_Y or yi>WORLD_MAX_Y:
        return None
    row=yi-WORLD_MIN_Y
    if row<0 or row+1>=len(offs):
        return None
    for i in range(offs[row],offs[row+1]):
        x0,x1,_z=runs[i]
        if x0<=xi<=x1:
            return 0
    return None

def flatten_compact(verts,segs,window_ids=()):
    keep=[]
    used=[]
    for source_sid,(a,b,_z0,_z1,occ,bias) in enumerate(segs):
        if not occ:
            continue
        # Reverse interior-solid rings so the walkable EXTERIOR is their
        # accepted side. This turns each pillar into one closed convex solid.
        if source_sid in INTERIOR_SOLID_SOURCE_IDS:
            a,b=b,a
        if source_sid in window_ids:
            keep.append((source_sid,a,b,bias,LINTEL))
            keep.append((source_sid,a,b,bias,RISER))
        else:
            keep.append((source_sid,a,b,bias,FULL))
        if a not in used: used.append(a)
        if b not in used: used.append(b)
    remap={old:new for new,old in enumerate(used)}
    cv=[verts[i] for i in used]
    cs=[(sid,src,remap[a],remap[b],bias,profile) for sid,(src,a,b,bias,profile) in enumerate(keep)]
    want=30+len(window_ids)
    if len(cv)!=30 or len(cs)!=want:
        raise SystemExit(f"geometry drift: verts={len(cv)} surfaces={len(cs)} expected={want}")
    return cv,cs,used

def angle_delta(a,b):
    return (a-b+math.pi)%(2.0*math.pi)-math.pi

def point_segment_dist(px,py,ax,ay,bx,by):
    vx,vy=bx-ax,by-ay
    vv=vx*vx+vy*vy
    if vv<=1e-12:
        return math.hypot(px-ax,py-ay)
    t=((px-ax)*vx+(py-ay)*vy)/vv
    t=max(0.0,min(1.0,t))
    return math.hypot(px-(ax+t*vx),py-(ay+t*vy))

def intersections(px,py,ang,verts,segs):
    dx,dy=math.cos(ang),math.sin(ang)
    out=[]
    for sid,_src,a,b,_bias,_profile in segs:
        ax,ay=verts[a]; bx,by=verts[b]
        sx,sy=bx-ax,by-ay
        den=dx*sy-dy*sx
        if abs(den)<1e-10:
            continue
        qx,qy=ax-px,ay-py
        t=(qx*sy-qy*sx)/den
        u=(qx*dy-qy*dx)/den
        if t>1e-6 and -1e-8<=u<=1.0+1e-8:
            out.append((t,sid))
    out.sort(key=lambda q:q[0])
    return out

def directed_angles(px,py,yaw,verts,segs):
    angles=[]
    for _sid,_src,a,b,_bias,_profile in segs:
        ax,ay=verts[a]; bx,by=verts[b]
        for sx,sy in ((ax,ay),(bx,by),((ax+bx)*0.5,(ay+by)*0.5)):
            ang=math.atan2(sy-py,sx-px)
            if abs(angle_delta(ang,yaw))<=CONE_HALF+1e-9:
                angles.append(ang)
    return angles

def camera_candidates(px,py,yaw,verts,segs,depth_layers):
    mask=0
    angles=[
        yaw-CONE_HALF+(2.0*CONE_HALF)*(i+0.5)/UNIFORM_RAYS
        for i in range(UNIFORM_RAYS)
    ]
    angles.extend(directed_angles(px,py,yaw,verts,segs))
    uniq={}
    for a in angles:
        uniq[int(round((a%(2.0*math.pi))*65536.0/(2.0*math.pi)))]=a
    for ang in uniq.values():
        hits=intersections(px,py,ang,verts,segs)
        for _t,sid in hits[:depth_layers]:
            mask|=1<<sid
    for sid,_src,a,b,_bias,_profile in segs:
        ax,ay=verts[a]; bx,by=verts[b]
        if point_segment_dist(px,py,ax,ay,bx,by)<=NEAR_ALWAYS:
            mask|=1<<sid
    return mask

def bake_masks(verts,segs,offs,runs,depth_layers):
    masks=[]; counts=[]; empty=0
    for gy in range(ROWS):
        for gx in range(COLS):
            x0=WORLD_MIN_X+gx*CELL
            y0=WORLD_MIN_Y+gy*CELL
            samples=((x0+4.0,y0+4.0),
                     (x0+0.5,y0+0.5),(x0+7.5,y0+0.5),
                     (x0+0.5,y0+7.5),(x0+7.5,y0+7.5))
            cameras=[(px,py) for px,py in samples if floor_at(px,py,offs,runs) is not None]
            for yb in range(YAW_BINS):
                yaw=2.0*math.pi*yb/YAW_BINS
                mask=0
                for px,py in cameras:
                    mask|=camera_candidates(px,py,yaw,verts,segs,depth_layers)
                if not cameras:
                    empty+=1
                masks.append(mask)
                counts.append(mask.bit_count())
    return masks,counts,empty

def q5_normal(a,b,verts):
    ax,ay=verts[a]; bx,by=verts[b]
    dx,dy=bx-ax,by-ay
    nlen=math.hypot(dx,dy)
    if nlen<1e-9: return 0,0
    return max(-32,min(32,round(32*dy/nlen))), max(-32,min(32,round(-32*dx/nlen)))

def extract_array(text,name):
    m=re.search(rf"static const\s+([^;=]+?)\s+{re.escape(name)}\s*\[[^\]]+\]\s*=\s*\{{.*?\}};",text,re.S)
    if not m:
        raise SystemExit(f"could not find baseline array {name}")
    return text[m.start():m.end()]

def emit_arr(ctype,name,vals,per=16):
    out=[f"static const {ctype} {name}[{len(vals)}] = {{"]
    for i in range(0,len(vals),per):
        out.append("    "+", ".join(str(x) for x in vals[i:i+per])+",")
    out.append("};")
    return "\n".join(out)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--repo-root",default=".")
    ap.add_argument("--geometry",default="src/generated/e1m1_room1_exact_geometry.h")
    ap.add_argument("--floor",default="src/generated/e1m1_room1_exact_floor.h")
    ap.add_argument("--out",required=True)
    ap.add_argument("--windows",action="store_true",
                    help="A/B variant: split both source walls 19 and 46 into LINTEL+RISER windows")
    ap.add_argument("--window-source",action="append",type=int,choices=WINDOW_SOURCE_IDS,default=[],
                    help="split one selected source wall into a LINTEL+RISER window; repeat for both")
    args=ap.parse_args()
    root=Path(args.repo_root)
    verts0,segs0=parse_geometry(Path(args.geometry))
    offs,runs=parse_floor(Path(args.floor))
    window_ids=set(WINDOW_SOURCE_IDS if args.windows else args.window_source)
    verts,segs,source_vertices=flatten_compact(verts0,segs0,window_ids)
    depth_layers=3 if window_ids else DEPTH_LAYERS
    masks,counts,empty=bake_masks(verts,segs,offs,runs,depth_layers)

    baseline="\n".join((root/f"src/generated/tilesector_polar_data_part0{i}.inc").read_text() for i in range(5))
    keys=[]; anchors=[]; nx=[]; ny=[]; prof=[]; shade=[]; source_sids=[]
    for sid,src,a,b,bias,profile in segs:
        keys.append(sid | (a<<5) | (b<<10))
        anchors.append(a)
        qx,qy=q5_normal(a,b,verts); nx.append(qx); ny.append(qy)
        prof.append(profile); shade.append(bias); source_sids.append(src)

    pvs=[]
    for m in masks:
        pvs.extend((m>>(8*i))&255 for i in range(MASK_BYTES))

    sections=[
      "/* GENERATED by tools/e1m1_latest_full_bake.py. */",
      "#define TSPF_KEY_COUNT %du"%len(keys),
      "#define TSPF_SELECTOR_COUNT 1u",
      "#define TSPF_BASE_COUNT 1u",
      "#define TSPF_RECIPE_COUNT 1u",
      f"#define E1FULL_WORLD_MIN_X {WORLD_MIN_X}u",
      f"#define E1FULL_WORLD_MIN_Y {WORLD_MIN_Y}u",
      f"#define E1FULL_PVS_COLS {COLS}u",
      f"#define E1FULL_PVS_ROWS {ROWS}u",
      f"#define E1FULL_PVS_YAW_BINS {YAW_BINS}u",
      f"#define E1FULL_PVS_MASK_BYTES {MASK_BYTES}u",
      emit_arr("uint16_t","k_tspf_keys",keys,10),
      emit_arr("int8_t","k_tspf_sel_a",[0]),
      emit_arr("int8_t","k_tspf_sel_b",[0]),
      emit_arr("int16_t","k_tspf_sel_c",[0]),
      emit_arr("uint8_t","k_tspf_sel_inv",[0]),
      emit_arr("uint8_t","k_tspf_ao",[0]*len(verts),20),
      emit_arr("uint16_t","k_tspf_base_off",[0]),
      emit_arr("uint8_t","k_tspf_base_stream",[0]),
      emit_arr("uint16_t","k_tspf_recipe_off",[0]),
      emit_arr("uint8_t","k_tspf_recipe_stream",[0,0]),
      emit_arr("uint8_t","k_tspf_recipe_grid",[255]),
      emit_arr("uint8_t","k_tspf_vx",[x for x,_ in verts],20),
      emit_arr("uint8_t","k_tspf_vy",[y for _,y in verts],20),
      emit_arr("uint8_t","k_tspf_seg_anchor",anchors,20),
      emit_arr("int8_t","k_tspf_nx_q5",nx,20),
      emit_arr("int8_t","k_tspf_ny_q5",ny,20),
      emit_arr("uint8_t","k_tspf_profile",prof,20),
      emit_arr("int8_t","k_tspf_shade_bias",shade,20),
      emit_arr("uint8_t","k_e1full_source_sid",source_sids,20),
      emit_arr("uint8_t","k_e1full_source_vertex",source_vertices,20),
      emit_arr("uint8_t","k_e1full_pvs",pvs,24),
    ]
    for name in ("k_tspf_recip8_q16","k_tspf_atan_q12","k_tspf_angle_x_pos",
                 "k_tspf_sec_q7","k_tspf_sin_q7","k_tspf_invz"):
        sections.append(extract_array(baseline,name))

    out=Path(args.out); out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text("\n\n".join(sections)+"\n")

    full_count=sum(1 for p in prof if p==FULL)
    lintel_count=sum(1 for p in prof if p==LINTEL)
    riser_count=sum(1 for p in prof if p==RISER)
    windows=len(window_ids)
    mode="WINDOW_AB" if windows else "FULL_ONLY"
    print(f"E1FULL_BAKE_PASS mode={mode} source_vertices={len(verts0)} source_segments={len(segs0)} full_vertices={len(verts)} surfaces={len(segs)}")
    print(f"profiles FULL={full_count} LINTEL={lintel_count} RAISED=0 RISER={riser_count} windows={windows} stairs=0 floor_insets=0")
    print(f"pvs cells={COLS*ROWS} yaw_bins={YAW_BINS} bytes={len(pvs)} candidate_mean={sum(counts)/len(counts):.2f} min={min(counts)} max={max(counts)}")
    print(f"empty_unwalkable_masks={empty} depth_layers={depth_layers} uniform_rays={UNIFORM_RAYS}")
    print("source_sids="+",".join(map(str,source_sids)))
    print("source_vertices="+",".join(map(str,source_vertices)))
    pname={FULL:"FULL",LINTEL:"LINTEL",RISER:"RISER"}
    for sid,src,a,b,bias,profile in segs:
        print(f"surface {sid:02d} source_sid={src:02d} v{a}->v{b} xy={verts[a]}->{verts[b]} {pname[profile]} shade={bias}")

if __name__=="__main__":
    main()
