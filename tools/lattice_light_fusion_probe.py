#!/usr/bin/env python3
"""Free-camera lighting-fusion analysis for the two-room Adaptive Polar scene.

This intentionally mirrors the mature host-lighting semantics from the
portal-penumbra branch, but evaluates them over arbitrary camera positions and
headings rather than a scripted patch rail. It is an analysis/oracle tool: it
does not claim the Game Gear runtime already renders these lighting results.
"""
from __future__ import annotations

import argparse
import csv
import math
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon
from PIL import Image, ImageDraw

W, H = 160, 144
HORIZON = 72.0
FOCAL = 80.0
CAMERA_Z = 16.0
CEILING_Z = 32.0
ROOM_B_FLOOR_Z = 4.0
TILE = 8
COLS, ROWS = 20, 18

# Exact authoring data from tools/polar_baked_lighting_data.h at the mature
# portal-penumbra checkpoint. Keep this boring and literal on purpose.
VERTS = [
    (16.0,16.0),(80.0,16.0),(80.0,36.0),(80.0,64.0),(80.0,80.0),(16.0,80.0),(112.0,36.0),
    (112.0,64.0),(112.0,14.0),(112.0,84.0),(136.0,6.0),(154.0,20.0),(176.0,10.0),(176.0,84.0),
]
FULL, LINTEL, RAISED, RISER = 0, 1, 2, 3

@dataclass(frozen=True)
class Seg:
    v0: int
    v1: int
    profile: int
    blocks: bool = True
    light_front: int = 0
    visual_front: int = 0

SEGS = [
    Seg(0,1,FULL), Seg(1,2,FULL), Seg(3,4,FULL), Seg(4,5,FULL), Seg(5,0,FULL), Seg(2,6,FULL), Seg(7,3,FULL),
    Seg(8,6,RAISED), Seg(7,9,RAISED), Seg(8,10,RAISED), Seg(10,11,RAISED), Seg(11,12,RAISED),
    Seg(12,13,RAISED), Seg(13,9,RAISED),
    Seg(2,3,LINTEL,True, 1, 0), Seg(6,7,LINTEL,True,-1, 0), Seg(6,7,RISER, True,-1,-1),
]
LIGHT = (92.0, 50.0, 8.0)
ROOM_A = [0,1,2,3,4,5]
CONNECTOR = [2,6,7,3]
ROOM_B = [8,10,11,12,13,9,7,6]

EDGE_LUT = np.array([
    [0,0,0,0,0,0,0,0], [0,0,0,0,1,1,1,1], [0,0,1,1,1,1,2,2], [0,0,1,1,2,2,3,3],
    [0,1,1,2,2,3,3,4], [0,1,1,2,3,4,4,5], [0,1,2,3,3,4,5,6], [0,1,2,3,4,5,6,7],
], dtype=np.int8)


def profile_z(profile: int) -> tuple[float,float]:
    if profile == RAISED: return 4.0, 32.0
    if profile == LINTEL: return 24.0, 32.0
    if profile == RISER: return 0.0, 4.0
    return 0.0, 32.0


def point_on_edge(x: float, y: float, a: tuple[float,float], b: tuple[float,float]) -> bool:
    dx, dy = b[0]-a[0], b[1]-a[1]
    px, py = x-a[0], y-a[1]
    cross = px*dy-py*dx
    dot = px*dx+py*dy
    return abs(cross) < 1e-7 and dot >= -1e-7 and dot <= dx*dx+dy*dy+1e-7


def in_poly(x: float, y: float, vids: list[int]) -> bool:
    inside = False
    n = len(vids)
    j = n-1
    for i in range(n):
        a, b = VERTS[vids[j]], VERTS[vids[i]]
        if point_on_edge(x,y,a,b):
            return True
        if ((a[1] > y) != (b[1] > y)) and x < ((b[0]-a[0])*(y-a[1])/(b[1]-a[1]) + a[0]):
            inside = not inside
        j = i
    return inside


def in_any(x: float, y: float) -> bool:
    return in_poly(x,y,ROOM_A) or in_poly(x,y,CONNECTOR) or in_poly(x,y,ROOM_B)


def ray_seg(ox: float, oy: float, dx: float, dy: float,
            a: tuple[float,float], b: tuple[float,float]) -> Optional[tuple[float,float]]:
    sx, sy = b[0]-a[0], b[1]-a[1]
    den = dx*sy-dy*sx
    if abs(den) < 1e-10:
        return None
    qx, qy = a[0]-ox, a[1]-oy
    t = (qx*sy-qy*sx)/den
    u = (qx*dy-qy*dx)/den
    return t, u


def world_point_lit(wx: float, wy: float, wz: float, receiver_sid: int = -1) -> bool:
    lx, ly, lz = LIGHT
    dx, dy = wx-lx, wy-ly
    if dx*dx+dy*dy < 1e-10:
        return True
    for sid,s in enumerate(SEGS):
        if not s.blocks or sid == receiver_sid:
            continue
        hit = ray_seg(lx,ly,dx,dy,VERTS[s.v0],VERTS[s.v1])
        if hit is None:
            continue
        t,u = hit
        if t <= 1e-7 or t >= 1.0-1e-7 or u < -1e-7 or u > 1.0+1e-7:
            continue
        z0,z1 = profile_z(s.profile)
        zhit = lz + t*(wz-lz)
        if zhit >= z0-1e-7 and zhit <= z1+1e-7:
            return False
    return True


def camera_basis(yaw: int) -> tuple[float,float,float,float]:
    a = yaw*(2.0*math.pi/256.0)
    fx, fy = math.cos(a), math.sin(a)
    return fx,fy,-fy,fx


def screen_plane_world(cx: float, cy: float, yaw: int, sx: int, sy: int, zplane: float):
    px, py = sx+0.5, sy+0.5
    vz = -(py-HORIZON)/FOCAL
    if abs(vz) < 1e-10:
        return None
    depth = (zplane-CAMERA_Z)/vz
    if depth <= 1e-6:
        return None
    lateral = depth*((px-80.0)/FOCAL)
    fx,fy,rx,ry = camera_basis(yaw)
    return cx+fx*depth+rx*lateral, cy+fy*depth+ry*lateral, depth


def floor_receiver(cx: float, cy: float, yaw: int, sx: int, sy: int):
    if sy <= 72:
        return None
    best = None
    p = screen_plane_world(cx,cy,yaw,sx,sy,ROOM_B_FLOOR_Z)
    if p is not None and in_poly(p[0],p[1],ROOM_B):
        best = (p[2],p[0],p[1],ROOM_B_FLOOR_Z)
    p = screen_plane_world(cx,cy,yaw,sx,sy,0.0)
    if p is not None and (in_poly(p[0],p[1],ROOM_A) or in_poly(p[0],p[1],CONNECTOR)):
        if best is None or p[2] < best[0]:
            best = (p[2],p[0],p[1],0.0)
    return None if best is None else (best[1],best[2],best[3])


def exact_floor_frame(cx: float, cy: float, yaw: int) -> tuple[np.ndarray,np.ndarray]:
    eligible = np.zeros((H,W), dtype=np.bool_)
    lit = np.zeros((H,W), dtype=np.bool_)
    for sy in range(73,H):
        for sx in range(W):
            p = floor_receiver(cx,cy,yaw,sx,sy)
            if p is None:
                continue
            eligible[sy,sx] = True
            lit[sy,sx] = world_point_lit(*p)
    return eligible,lit


def best_edge_mask(eligible: np.ndarray, target: np.ndarray):
    eligible_count = int(eligible.sum())
    target_count = int(np.logical_and(target,eligible).sum())
    if not eligible_count or not target_count or target_count == eligible_count:
        return target.copy(), None, 0
    best = None
    best_cost = 65
    for orient in range(2):
      for si in range(8):
       for mirror in range(2):
        for side in range(2):
         for off in range(-8,16):
            cand = np.zeros((8,8), dtype=np.bool_)
            for y in range(8):
                for x in range(8):
                    a = y if orient else x
                    b = x if orient else y
                    sample = 7-a if mirror else a
                    line = off + int(EDGE_LUT[si,sample])
                    cand[y,x] = b >= line if side else b < line
            cc = int(np.logical_and(cand,eligible).sum())
            if not cc or cc == eligible_count:
                continue
            cost = int(np.logical_and(np.logical_xor(cand,target),eligible).sum())
            if cost < best_cost:
                best_cost = cost
                best = (cand.copy(),(orient,si,mirror,side,off))
                if cost == 0:
                    return best[0],best[1],0
    if best is None:
        return target.copy(), None, 0
    return best[0],best[1],best_cost


def quantize_frame(eligible: np.ndarray, exact: np.ndarray):
    out = exact.copy()
    signatures = []
    mixed = 0
    total_cost = 0
    eligible_boundary_pixels = 0
    for ty in range(9,ROWS):
        for tx in range(COLS):
            ys,xs = slice(ty*8,(ty+1)*8),slice(tx*8,(tx+1)*8)
            e,t = eligible[ys,xs], exact[ys,xs]
            ec = int(e.sum())
            tc = int(np.logical_and(t,e).sum())
            if not ec or not tc or tc == ec:
                continue
            mixed += 1
            fit,sig,cost = best_edge_mask(e,t)
            out[ys,xs] = np.where(e,fit,out[ys,xs])
            eligible_boundary_pixels += ec
            total_cost += cost
            if sig is not None:
                signatures.append(sig)
    mismatch = int(np.logical_and(np.logical_xor(out,exact),eligible).sum())
    return out, {
        'mismatch_pixels': mismatch,
        'mixed_tiles': mixed,
        'fit_cost': total_cost,
        'boundary_eligible_pixels': eligible_boundary_pixels,
        'signatures': signatures,
    }


def apply_penumbra(hard: np.ndarray, eligible: np.ndarray) -> np.ndarray:
    out = hard.copy()
    for ty in range(9,ROWS):
        for tx in range(COLS):
            ys,xs = slice(ty*8,(ty+1)*8),slice(tx*8,(tx+1)*8)
            e = eligible[ys,xs]
            h = hard[ys,xs]
            ec,lc = int(e.sum()),int(np.logical_and(h,e).sum())
            if not ec or not lc or lc == ec:
                continue
            for y in range(8):
                for x in range(8):
                    sy,sx = ty*8+y,tx*8+x
                    if not eligible[sy,sx] or hard[sy,sx]:
                        continue
                    touch=False
                    for ny in (-1,0,1):
                        for nx in (-1,0,1):
                            if not (nx or ny): continue
                            yy,xx=sy+ny,sx+nx
                            if yy<ty*8 or yy>ty*8+7 or xx<tx*8 or xx>tx*8+7: continue
                            if eligible[yy,xx] and hard[yy,xx]: touch=True
                    if touch and ((sx+sy)&1)==0:
                        out[sy,sx]=True
    return out


def traversable_samples(step: int, dense_portal: bool) -> list[tuple[float,float]]:
    xs = set(range(16,177,step))
    ys = set(range(6,85,step))
    if dense_portal:
        for x in (76,78,79,80,81,82,84,108,110,111,112,113,114,116): xs.add(x)
        for y in (32,34,35,36,37,38,40,48,50,56,62,63,64,65,66,68): ys.add(y)
    out=[]
    for y in sorted(ys):
        for x in sorted(xs):
            if in_any(x,y): out.append((float(x),float(y)))
    return out


def draw_world(ax):
    for vids,label in ((ROOM_A,'Room A'),(CONNECTOR,'Portal / connector'),(ROOM_B,'Room B')):
        pts=[VERTS[i] for i in vids]
        ax.add_patch(Polygon(pts,closed=True,fill=False,linewidth=1.5))
        cx=sum(p[0] for p in pts)/len(pts); cy=sum(p[1] for p in pts)/len(pts)
        ax.text(cx,cy,label,ha='center',va='center',fontsize=8)
    ax.scatter([LIGHT[0]],[LIGHT[1]],marker='*',s=120,label='Static light')
    for sid in (14,15,16):
        s=SEGS[sid]; a,b=VERTS[s.v0],VERTS[s.v1]
        ax.plot([a[0],b[0]],[a[1],b[1]],linewidth=2)
    ax.set_aspect('equal','box'); ax.set_xlim(8,184); ax.set_ylim(90,0)
    ax.set_xlabel('world X'); ax.set_ylabel('world Y')


def save_scatter(rows, key, title, path, cbar):
    fig,ax=plt.subplots(figsize=(11,5.5))
    draw_world(ax)
    sc=ax.scatter([r['x'] for r in rows],[r['y'] for r in rows],c=[r[key] for r in rows],s=46,marker='s')
    fig.colorbar(sc,ax=ax,label=cbar)
    ax.set_title(title)
    fig.tight_layout(); fig.savefig(path,dpi=180); plt.close(fig)


def mask_image(eligible, exact, quant, text: str) -> Image.Image:
    arr=np.zeros((H,W,3),dtype=np.uint8); arr[:]=[12,14,18]
    arr[eligible & ~exact]=[48,54,66]
    arr[eligible & exact]=[190,198,215]
    arr[eligible & np.logical_xor(exact,quant)]=[230,80,80]
    img=Image.fromarray(arr,'RGB').resize((W*3,H*3),Image.Resampling.NEAREST)
    d=ImageDraw.Draw(img); d.rectangle((0,0,img.width,22),fill=(0,0,0)); d.text((4,4),text,fill=(255,255,255))
    return img


def write_contact_sheet(worst, outdir: Path):
    imgs=[]
    for r in worst[:8]:
        e,x=exact_floor_frame(r['x'],r['y'],r['yaw']); q,m=quantize_frame(e,x)
        txt=f"x={r['x']:.0f} y={r['y']:.0f} yaw={r['yaw']} err={r['mismatch_pct']:.3f}% mixed={m['mixed_tiles']}"
        imgs.append(mask_image(e,x,q,txt))
    if not imgs: return
    cols=2; rows=math.ceil(len(imgs)/cols)
    sheet=Image.new('RGB',(imgs[0].width*cols,imgs[0].height*rows),(20,20,20))
    for i,img in enumerate(imgs): sheet.paste(img,((i%cols)*img.width,(i//cols)*img.height))
    sheet.save(outdir/'worst_frames_contact_sheet.png')


def path_poses() -> list[tuple[float,float,int]]:
    way=[(40,50,0),(70,50,0),(96,50,0),(122,50,0),(150,50,0),(150,28,64),(150,70,192),(110,50,128),(70,50,128),(40,50,128)]
    out=[]
    for a,b in zip(way,way[1:]):
        for k in range(18):
            t=k/18.0
            x=a[0]+(b[0]-a[0])*t; y=a[1]+(b[1]-a[1])*t
            dy=((b[2]-a[2]+128)%256)-128
            yaw=int(round(a[2]+dy*t))&255
            if in_any(x,y): out.append((x,y,yaw))
    out.append(way[-1])
    return out


def make_video_frames(outdir: Path, baseline_fps: Optional[float]):
    frames=outdir/'video_frames'; frames.mkdir(exist_ok=True)
    for i,(cx,cy,yaw) in enumerate(path_poses()):
        e,x=exact_floor_frame(cx,cy,yaw); q,m=quantize_frame(e,x)
        pct=100.0*m['mismatch_pixels']/max(1,int(e.sum()))
        arr=np.zeros((H,W*2,3),dtype=np.uint8)
        for j,mask in enumerate((x,q)):
            half=np.zeros((H,W,3),dtype=np.uint8); half[:]=[12,14,18]
            half[e & ~mask]=[48,54,66]; half[e & mask]=[190,198,215]
            arr[:,j*W:(j+1)*W]=half
        img=Image.fromarray(arr,'RGB').resize((W*4,H*2),Image.Resampling.NEAREST)
        d=ImageDraw.Draw(img); d.rectangle((0,0,img.width,30),fill=(0,0,0))
        perf='baseline GG lattice perf: pending' if baseline_fps is None else f'baseline GG lattice mean: {baseline_fps:.2f} upd/s'
        d.text((6,4),f'HOST LIGHT ORACLE (left) vs straight-edge quantized (right) | mismatch={pct:.3f}% | {perf}',fill=(255,255,255))
        d.text((6,17),'Lighting is analysis-only here; runtime lighting cost is NOT yet included in the FPS number.',fill=(255,220,120))
        img.save(frames/f'{i:04d}.png')


def parse_baseline_perf(path: Optional[Path]) -> Optional[float]:
    if not path or not path.exists(): return None
    for line in path.read_text(errors='ignore').splitlines():
        if 'representative_manual_mean_effective_updates_per_s=' in line:
            try: return float(line.split('=',1)[1].strip())
            except ValueError: return None
    return None


def main() -> int:
    ap=argparse.ArgumentParser()
    ap.add_argument('--out',default='build/lattice-lighting')
    ap.add_argument('--step',type=int,default=8)
    ap.add_argument('--yaw-step',type=int,default=16)
    ap.add_argument('--baseline-perf')
    args=ap.parse_args()
    out=Path(args.out); out.mkdir(parents=True,exist_ok=True)
    poses=[]; pos_rows=[]; signatures=set(); samples=traversable_samples(args.step,True)
    for cx,cy in samples:
        local=[]
        for yaw in range(0,256,args.yaw_step):
            eligible, exact=exact_floor_frame(cx,cy,yaw); quant,m=quantize_frame(eligible,exact); pen=apply_penumbra(quant,eligible)
            ep=int(eligible.sum()); mismatch_pct=100.0*m['mismatch_pixels']/max(1,ep); pen_added=int(np.logical_and(pen,~quant).sum())
            for s in m['signatures']: signatures.add(s)
            row=dict(x=cx,y=cy,yaw=yaw,eligible_pixels=ep,mismatch_pixels=m['mismatch_pixels'],mismatch_pct=mismatch_pct,
                     mixed_tiles=m['mixed_tiles'],boundary_eligible_pixels=m['boundary_eligible_pixels'],penumbra_added_pixels=pen_added,
                     unique_edge_signatures_frame=len(set(m['signatures'])))
            poses.append(row); local.append(row)
        pos_rows.append(dict(x=cx,y=cy,worst_mismatch_pct=max(r['mismatch_pct'] for r in local),
            mean_mismatch_pct=sum(r['mismatch_pct'] for r in local)/len(local),max_mixed_tiles=max(r['mixed_tiles'] for r in local),
            mean_mixed_tiles=sum(r['mixed_tiles'] for r in local)/len(local),max_penumbra_pixels=max(r['penumbra_added_pixels'] for r in local)))

    with (out/'pose_metrics.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(poses[0])); w.writeheader(); w.writerows(poses)
    with (out/'position_metrics.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(pos_rows[0])); w.writeheader(); w.writerows(pos_rows)
    save_scatter(pos_rows,'worst_mismatch_pct','Floor hard-shadow quantization: worst error by camera position',out/'map_shadow_error.png','worst eligible-pixel mismatch (%)')
    save_scatter(pos_rows,'mean_mixed_tiles','Floor hard-shadow boundary pressure',out/'map_mixed_tiles.png','mean mixed 8x8 receiver tiles / heading')
    save_scatter(pos_rows,'max_penumbra_pixels','One-sided penumbra footprint',out/'map_penumbra_pixels.png','max extra ordered-dither pixels / heading')
    worst=sorted(poses,key=lambda r:(r['mismatch_pct'],r['mixed_tiles']),reverse=True); write_contact_sheet(worst,out)
    baseline=parse_baseline_perf(Path(args.baseline_perf) if args.baseline_perf else None); make_video_frames(out,baseline)
    vals=np.array([r['mismatch_pct'] for r in poses]); mixed=np.array([r['mixed_tiles'] for r in poses])
    with (out/'SUMMARY.txt').open('w') as f:
        f.write('=== FREE-CAMERA LATTICE LIGHTING FUSION PROBE ===\n')
        f.write(f'positions={len(samples)} headings_per_position={256//args.yaw_step} poses={len(poses)}\n')
        f.write('oracle=mature portal-penumbra world/profile blocking model, re-evaluated at arbitrary cameras\n')
        f.write('approximation=existing reusable 8x8 straight-edge family, independent per mixed floor tile\n')
        f.write(f'mismatch_pct mean={vals.mean():.6f} p95={np.percentile(vals,95):.6f} max={vals.max():.6f}\n')
        f.write(f'mixed_tiles mean={mixed.mean():.3f} p95={np.percentile(mixed,95):.3f} max={mixed.max():.0f}\n')
        f.write(f'global_quantized_edge_signatures={len(signatures)}\n')
        f.write(f'baseline_lattice_updates_per_s={"unknown" if baseline is None else f"{baseline:.3f}"}\n')
        f.write('IMPORTANT: baseline rate excludes runtime lighting; this job measures visual/representation pressure first.\n')
        f.write('NEXT: port the measured small boundary vocabulary into live Polar and cycle-profile the added semantic edge pass.\n')
    print((out/'SUMMARY.txt').read_text())
    return 0

if __name__=='__main__':
    raise SystemExit(main())
