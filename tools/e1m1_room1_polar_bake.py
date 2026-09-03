#!/usr/bin/env python3
"""Bake E1M1 Room 1 data for the mature Adaptive Polar GG renderer.

This is deliberately a host-side topology bake, not another runtime renderer.
For each 8x8 world cell and 16-way yaw bucket we conservatively retain only
near depth layers that can contribute inside the 90-degree camera frustum.
The GG runtime loads one eight-byte mask, projects shared authored corners,
then hands whole runs to the existing retained-name-table Z80 materializer.

The source of truth remains the generated exact E1M1 geometry/floor oracle.
"""
from __future__ import annotations

import argparse
import math
import re
from pathlib import Path

WORLD_MIN_X=16
WORLD_MIN_Y=24
WORLD_MAX_X=112
WORLD_MAX_Y=80
CELL=8
COLS=(WORLD_MAX_X-WORLD_MIN_X)//CELL
ROWS=(WORLD_MAX_Y-WORLD_MIN_Y)//CELL
YAW_BINS=16
MASK_BYTES=8
FOV_HALF=math.radians(45.0)
YAW_HALF_BUCKET=math.pi/16.0
CONE_HALF=FOV_HALF+YAW_HALF_BUCKET+math.radians(2.0)
UNIFORM_RAYS=56
DEPTH_LAYERS=8
NEAR_ALWAYS=4.0
EYE_HEIGHT=5.0
SCREEN_Y=(4.0,20.0,36.0,52.0,68.0,76.0,92.0,116.0,140.0)

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
        raise SystemExit(f"unexpected geometry: verts={len(verts)} segs={len(segs)}")
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
    xi=int(math.floor(x)); yi=int(math.floor(y))
    if xi<WORLD_MIN_X or xi>WORLD_MAX_X or yi<WORLD_MIN_Y or yi>WORLD_MAX_Y:
        return None
    row=yi-WORLD_MIN_Y
    if row<0 or row+1>=len(offs):
        return None
    for i in range(offs[row],offs[row+1]):
        x0,x1,z=runs[i]
        if x0<=xi<=x1:
            return z
    return None

def angle_delta(a: float,b: float)->float:
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
    for sid,(a,b,z0,z1,_occ,_bias) in enumerate(segs):
        ax,ay=verts[a]; bx,by=verts[b]
        sx,sy=bx-ax,by-ay
        den=dx*sy-dy*sx
        if abs(den)<1e-10:
            continue
        qx,qy=ax-px,ay-py
        t=(qx*sy-qy*sx)/den
        u=(qx*dy-qy*dx)/den
        if t>1e-6 and -1e-8<=u<=1.0+1e-8:
            out.append((t,sid,z0,z1))
    out.sort(key=lambda q:q[0])
    return out

def directed_angles(px,py,yaw,verts,segs):
    angles=[]
    for a,b,*_ in segs:
        ax,ay=verts[a]; bx,by=verts[b]
        for sx,sy in ((ax,ay),(bx,by),((ax+bx)*0.5,(ay+by)*0.5)):
            ang=math.atan2(sy-py,sx-px)
            if abs(angle_delta(ang,yaw))<=CONE_HALF+1e-9:
                angles.append(ang)
    return angles

def camera_candidates(px,py,yaw,eye,verts,segs):
    mask=0
    angles=[
        yaw-CONE_HALF+(2.0*CONE_HALF)*(i+0.5)/UNIFORM_RAYS
        for i in range(UNIFORM_RAYS)
    ]
    angles.extend(directed_angles(px,py,yaw,verts,segs))
    # Quantize duplicate event rays enough to keep the bake cheap while still
    # retaining authored endpoint/midpoint directions.
    uniq={}
    for a in angles:
        uniq[int(round((a%(2.0*math.pi))*65536.0/(2.0*math.pi)))]=a

    for ang in uniq.values():
        hits=intersections(px,py,ang,verts,segs)
        # Conservative XY depth layers protect narrow vertical apertures even
        # when the coarse vertical sampling below misses a sub-pixel event.
        for _t,sid,_z0,_z1 in hits[:DEPTH_LAYERS]:
            mask|=1<<sid

        rel=angle_delta(ang,yaw)
        c=max(0.05,math.cos(rel))
        for sy in SCREEN_Y:
            slope=-(sy-72.0)/80.0
            for t,sid,z0,z1 in hits:
                forward=t*c
                z=eye+slope*forward
                if z0-1e-7<=z<=z1+1e-7:
                    mask|=1<<sid
                    break

    for sid,(a,b,*_) in enumerate(segs):
        ax,ay=verts[a]; bx,by=verts[b]
        if point_segment_dist(px,py,ax,ay,bx,by)<=NEAR_ALWAYS:
            mask|=1<<sid
    return mask

def bake_masks(verts,segs,offs,runs):
    masks=[]
    counts=[]
    empty=0
    for gy in range(ROWS):
        for gx in range(COLS):
            x0=WORLD_MIN_X+gx*CELL
            y0=WORLD_MIN_Y+gy*CELL
            samples=((x0+4.0,y0+4.0),
                     (x0+0.5,y0+0.5),(x0+7.5,y0+0.5),
                     (x0+0.5,y0+7.5),(x0+7.5,y0+7.5))
            cameras=[]
            for px,py in samples:
                f=floor_at(px,py,offs,runs)
                if f is not None:
                    cameras.append((px,py,float(f)+EYE_HEIGHT))
            for yb in range(YAW_BINS):
                yaw=2.0*math.pi*yb/YAW_BINS
                mask=0
                for px,py,eye in cameras:
                    mask|=camera_candidates(px,py,yaw,eye,verts,segs)
                if not cameras:
                    empty+=1
                masks.append(mask)
                counts.append(mask.bit_count())
    return masks,counts,empty

def q5_normal(a,b,verts):
    ax,ay=verts[a]; bx,by=verts[b]
    dx,dy=bx-ax,by-ay
    nlen=math.hypot(dx,dy)
    if nlen<1e-9:
        return 0,0
    nx=int(round(32.0*dy/nlen))
    ny=int(round(-32.0*dx/nlen))
    nx=max(-32,min(32,nx)); ny=max(-32,min(32,ny))
    return nx,ny

def emit_list(f,ctype,name,vals,per=16):
    f.write(f"static const {ctype} {name}[{len(vals)}] = {{\n")
    for i in range(0,len(vals),per):
        f.write("    "+",".join(str(v) for v in vals[i:i+per])+",\n")
    f.write("};\n\n")

def emit_meta(path,verts,segs):
    v0=[s[0] for s in segs]; v1=[s[1] for s in segs]
    z0=[s[2] for s in segs]; z1=[s[3] for s in segs]
    bias=[s[5] for s in segs]
    normals=[q5_normal(a,b,verts) for a,b,*_ in segs]
    nx=[q[0] for q in normals]; ny=[q[1] for q in normals]
    with path.open("w") as f:
        f.write("/* GENERATED by tools/e1m1_room1_polar_bake.py. */\n")
        f.write("#ifndef E1M1_ROOM1_POLAR_META_H\n#define E1M1_ROOM1_POLAR_META_H\n")
        f.write("#include <stdint.h>\n")
        f.write("#define E1PF_VERTEX_COUNT 44u\n#define E1PF_SEGMENT_COUNT 58u\n")
        emit_list(f,"uint8_t","k_e1pf_seg_v0",v0)
        emit_list(f,"uint8_t","k_e1pf_seg_v1",v1)
        emit_list(f,"uint8_t","k_e1pf_seg_anchor",v0)
        emit_list(f,"int8_t","k_e1pf_nx_q5",nx)
        emit_list(f,"int8_t","k_e1pf_ny_q5",ny)
        emit_list(f,"int8_t","k_e1pf_z0",z0)
        emit_list(f,"int8_t","k_e1pf_z1",z1)
        emit_list(f,"int8_t","k_e1pf_shade_bias",bias)
        f.write("#endif\n")

def emit_pvs(header,source,masks):
    flat=[]
    for m in masks:
        flat.extend((m>>(8*i))&255 for i in range(MASK_BYTES))
    with header.open("w") as f:
        f.write("/* GENERATED by tools/e1m1_room1_polar_bake.py. */\n")
        f.write("#ifndef E1M1_ROOM1_POLAR_PVS_H\n#define E1M1_ROOM1_POLAR_PVS_H\n")
        f.write("#include <stdint.h>\n")
        f.write(f"#define E1PF_PVS_COLS {COLS}u\n#define E1PF_PVS_ROWS {ROWS}u\n")
        f.write(f"#define E1PF_PVS_YAW_BINS {YAW_BINS}u\n#define E1PF_PVS_MASK_BYTES {MASK_BYTES}u\n")
        f.write("void e1pf_load_pvs(uint8_t gx,uint8_t gy,uint8_t yaw_bin,uint8_t out[8]);\n")
        f.write("#endif\n")
    with source.open("w") as f:
        f.write("/* GENERATED by tools/e1m1_room1_polar_bake.py. */\n")
        f.write("#include <stdint.h>\n#include <gbdk/platform.h>\n")
        f.write('#include "e1m1_room1_polar_pvs.h"\n')
        f.write("#if defined(__SDCC)\n#pragma bank 20\nBANKREF(e1m1_room1_polar_pvs_bank)\n#endif\n")
        emit_list(f,"uint8_t","k_e1pf_pvs",flat,24)
        f.write("void e1pf_load_pvs(uint8_t gx,uint8_t gy,uint8_t yaw_bin,uint8_t out[8]) BANKED {\n")
        f.write("    uint8_t i; uint16_t cell,off;\n")
        f.write("    if(gx>=E1PF_PVS_COLS||gy>=E1PF_PVS_ROWS){for(i=0u;i<8u;++i)out[i]=0u;return;}\n")
        f.write("    cell=(uint16_t)((uint16_t)gy*E1PF_PVS_COLS+gx);\n")
        f.write("    off=(uint16_t)((cell<<7)+((uint16_t)(yaw_bin&15u)<<3));\n")
        f.write("    for(i=0u;i<8u;++i)out[i]=k_e1pf_pvs[off+i];\n")
        f.write("}\n")

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--geometry",default="src/generated/e1m1_room1_exact_geometry.h")
    ap.add_argument("--floor",default="src/generated/e1m1_room1_exact_floor.h")
    ap.add_argument("--out-dir",required=True)
    args=ap.parse_args()

    verts,segs=parse_geometry(Path(args.geometry))
    offs,runs=parse_floor(Path(args.floor))
    masks,counts,empty=bake_masks(verts,segs,offs,runs)

    out=Path(args.out_dir)
    out.mkdir(parents=True,exist_ok=True)
    emit_meta(out/"e1m1_room1_polar_meta.h",verts,segs)
    emit_pvs(out/"e1m1_room1_polar_pvs.h",out/"e1m1_room1_polar_pvs.c",masks)

    print(f"E1PF_BAKE_PASS cells={COLS*ROWS} yaw_bins={YAW_BINS} masks={len(masks)}")
    print(f"candidate_count mean={sum(counts)/len(counts):.2f} min={min(counts)} max={max(counts)}")
    print(f"pvs_bytes={len(masks)*MASK_BYTES} empty_unwalkable_masks={empty}")
    print(f"depth_layers={DEPTH_LAYERS} uniform_rays={UNIFORM_RAYS} cone_half_deg={math.degrees(CONE_HALF):.2f}")

if __name__=="__main__":
    main()
