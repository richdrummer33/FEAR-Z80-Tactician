#!/usr/bin/env python3
"""Fast free-camera lighting-fusion analysis for the two-room Adaptive Polar scene.

The world/light/blocker data and hard-shadow semantics mirror the mature
portal-penumbra host compositor, but the camera is sampled freely over the
traversable two-room region.  This is an oracle/representation probe: no result
from this file is counted as Game Gear runtime lighting performance.
"""
from __future__ import annotations

import argparse, csv, math
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon
from PIL import Image, ImageDraw

W,H=160,144
HORIZON=72.0
FOCAL=80.0
CAMERA_Z=16.0
ROOM_B_FLOOR_Z=4.0
COLS,ROWS=20,18

VERTS=np.array([
 (16.,16.),(80.,16.),(80.,36.),(80.,64.),(80.,80.),(16.,80.),(112.,36.),
 (112.,64.),(112.,14.),(112.,84.),(136.,6.),(154.,20.),(176.,10.),(176.,84.)
],dtype=np.float64)
FULL,LINTEL,RAISED,RISER=0,1,2,3

@dataclass(frozen=True)
class Seg:
    v0:int; v1:int; profile:int; blocks:bool=True; light_front:int=0; visual_front:int=0

SEGS=[
 Seg(0,1,FULL),Seg(1,2,FULL),Seg(3,4,FULL),Seg(4,5,FULL),Seg(5,0,FULL),Seg(2,6,FULL),Seg(7,3,FULL),
 Seg(8,6,RAISED),Seg(7,9,RAISED),Seg(8,10,RAISED),Seg(10,11,RAISED),Seg(11,12,RAISED),Seg(12,13,RAISED),Seg(13,9,RAISED),
 Seg(2,3,LINTEL,True,1,0),Seg(6,7,LINTEL,True,-1,0),Seg(6,7,RISER,True,-1,-1)
]
LIGHT=(92.,50.,8.)
ROOM_A=[0,1,2,3,4,5]
CONNECTOR=[2,6,7,3]
ROOM_B=[8,10,11,12,13,9,7,6]
EDGE_LUT=np.array([
 [0,0,0,0,0,0,0,0],[0,0,0,0,1,1,1,1],[0,0,1,1,1,1,2,2],[0,0,1,1,2,2,3,3],
 [0,1,1,2,2,3,3,4],[0,1,1,2,3,4,4,5],[0,1,2,3,3,4,5,6],[0,1,2,3,4,5,6,7]
],dtype=np.int8)

def profile_z(p:int)->tuple[float,float]:
    return (4.,32.) if p==RAISED else (24.,32.) if p==LINTEL else (0.,4.) if p==RISER else (0.,32.)

def points_in_poly(x:np.ndarray,y:np.ndarray,vids:list[int])->np.ndarray:
    out=np.zeros(np.broadcast(x,y).shape,dtype=bool); p=VERTS[vids]; j=len(p)-1
    for i in range(len(p)):
        xi,yi=p[i]; xj,yj=p[j]
        cross=(x-xj)*(yi-yj)-(y-yj)*(xi-xj)
        on=(np.abs(cross)<1e-7)&((x-xj)*(xi-xj)+(y-yj)*(yi-yj)>=-1e-7)&((x-xi)*(xj-xi)+(y-yi)*(yj-yi)>=-1e-7)
        cond=((yi>y)!=(yj>y))&(x<(xj-xi)*(y-yi)/(yj-yi+1e-300)+xi)
        out^=cond; out|=on; j=i
    return out

def point_in_any(x:float,y:float)->bool:
    a=np.array([x]); b=np.array([y])
    return bool(points_in_poly(a,b,ROOM_A)[0] or points_in_poly(a,b,CONNECTOR)[0] or points_in_poly(a,b,ROOM_B)[0])

def camera_floor_arrays(cx:float,cy:float,yaw:int,zplane:float):
    sy=np.arange(73,H,dtype=np.float64)+0.5; sx=np.arange(W,dtype=np.float64)+0.5
    py,px=np.meshgrid(sy,sx,indexing='ij'); vz=-(py-HORIZON)/FOCAL; depth=(zplane-CAMERA_Z)/vz; lateral=depth*((px-80.)/FOCAL)
    a=yaw*(2.*math.pi/256.); fx,fy=math.cos(a),math.sin(a); rx,ry=-fy,fx
    return cx+fx*depth+rx*lateral,cy+fy*depth+ry*lateral,depth

def vector_lit(wx:np.ndarray,wy:np.ndarray,wz:float,eligible:np.ndarray)->np.ndarray:
    lx,ly,lz=LIGHT; dx=wx-lx; dy=wy-ly; lit=eligible.copy()
    for s in SEGS:
        if not s.blocks: continue
        a=VERTS[s.v0]; b=VERTS[s.v1]; sx,sy=b-a; den=dx*sy-dy*sx; valid=np.abs(den)>1e-10; qx,qy=a[0]-lx,a[1]-ly
        t=np.zeros_like(wx); u=np.zeros_like(wx); t[valid]=(qx*sy-qy*sx)/den[valid]; u[valid]=(qx*dy[valid]-qy*dx[valid])/den[valid]
        z0,z1=profile_z(s.profile); zhit=lz+t*(wz-lz)
        block=eligible&valid&(t>1e-7)&(t<1.-1e-7)&(u>=-1e-7)&(u<=1.+1e-7)&(zhit>=z0-1e-7)&(zhit<=z1+1e-7); lit&=~block
    return lit

def exact_floor_frame(cx:float,cy:float,yaw:int)->tuple[np.ndarray,np.ndarray]:
    full_e=np.zeros((H,W),bool); full_l=np.zeros((H,W),bool)
    wx4,wy4,d4=camera_floor_arrays(cx,cy,yaw,ROOM_B_FLOOR_Z); e4=points_in_poly(wx4,wy4,ROOM_B)&(d4>0)
    wx0,wy0,d0=camera_floor_arrays(cx,cy,yaw,0.); e0=(points_in_poly(wx0,wy0,ROOM_A)|points_in_poly(wx0,wy0,CONNECTOR))&(d0>0)
    use4=e4&(~e0|(d4<d0)); use0=e0&~use4; lit=np.zeros_like(e4)
    if use4.any():lit|=vector_lit(wx4,wy4,ROOM_B_FLOOR_Z,use4)
    if use0.any():lit|=vector_lit(wx0,wy0,0.,use0)
    full_e[73:]=use4|use0; full_l[73:]=lit; return full_e,full_l

def build_edge_bank():
    masks=[]; sig=[]; yy,xx=np.mgrid[0:8,0:8]
    for orient in range(2):
      for si in range(8):
       for mirror in range(2):
        a=yy if orient else xx; b=xx if orient else yy; sample=7-a if mirror else a; base=EDGE_LUT[si][sample]
        for side in range(2):
         for off in range(-8,16):
            line=off+base; masks.append(((b>=line) if side else (b<line)).reshape(-1)); sig.append((orient,si,mirror,side,off))
    return np.asarray(masks,dtype=bool),sig
EDGE_MASKS,EDGE_SIGS=build_edge_bank()

def best_edge_mask(eligible:np.ndarray,target:np.ndarray):
    e=eligible.reshape(-1); t=target.reshape(-1); ec=int(e.sum()); tc=int((t&e).sum())
    if not ec or not tc or tc==ec:return target.copy(),None,0
    active=(EDGE_MASKS&e).sum(axis=1); valid=(active>0)&(active<ec); costs=np.where(valid,((EDGE_MASKS^t)&e).sum(axis=1),999)
    i=int(np.argmin(costs)); cost=int(costs[i]); return (target.copy(),None,0) if cost>=999 else (EDGE_MASKS[i].reshape(8,8).copy(),EDGE_SIGS[i],cost)

def quantize_frame(eligible:np.ndarray,exact:np.ndarray):
    out=exact.copy(); sigs=[]; mixed=cost=bpix=0
    for ty in range(9,ROWS):
      for tx in range(COLS):
        ys=slice(ty*8,(ty+1)*8); xs=slice(tx*8,(tx+1)*8); e=eligible[ys,xs]; t=exact[ys,xs]; ec=int(e.sum()); tc=int((e&t).sum())
        if not ec or not tc or tc==ec:continue
        mixed+=1; fit,sig,c=best_edge_mask(e,t); out[ys,xs]=np.where(e,fit,out[ys,xs]); cost+=c; bpix+=ec
        if sig is not None:sigs.append(sig)
    return out,dict(mismatch_pixels=int(((out^exact)&eligible).sum()),mixed_tiles=mixed,fit_cost=cost,boundary_eligible_pixels=bpix,signatures=sigs)

def apply_penumbra(hard:np.ndarray,eligible:np.ndarray)->np.ndarray:
    out=hard.copy()
    for ty in range(9,ROWS):
      for tx in range(COLS):
        y0,x0=ty*8,tx*8; e=eligible[y0:y0+8,x0:x0+8]; h=hard[y0:y0+8,x0:x0+8]; ec=int(e.sum()); lc=int((e&h).sum())
        if not ec or not lc or lc==ec:continue
        pad=np.pad(h,1); touch=np.zeros_like(h)
        for dy in range(3):
          for dx in range(3):
            if dx!=1 or dy!=1:touch|=pad[dy:dy+8,dx:dx+8]
        yy,xx=np.mgrid[y0:y0+8,x0:x0+8]; out[y0:y0+8,x0:x0+8]|=e&~h&touch&(((xx+yy)&1)==0)
    return out

def traversable_samples(step:int,dense_portal:bool)->list[tuple[float,float]]:
    pts=set()
    for y in range(10,85,step):
      for x in range(16,177,step):
        if point_in_any(x,y):pts.add((float(x),float(y)))
    if dense_portal:
        xs=(72,76,78,79,80,81,82,84,88,104,108,110,111,112,113,114,116,120); ys=(32,34,35,36,37,38,40,48,50,56,62,63,64,65,66,68)
        for x in xs:
          for y in (34,36,38,48,50,62,64,66):
            if point_in_any(x,y):pts.add((float(x),float(y)))
        for y in ys:
          for x in (76,80,84,96,108,112,116):
            if point_in_any(x,y):pts.add((float(x),float(y)))
    return sorted(pts,key=lambda p:(p[1],p[0]))

def draw_world(ax):
    for vids,label in ((ROOM_A,'Room A'),(CONNECTOR,'Portal / connector'),(ROOM_B,'Room B')):
        pts=VERTS[vids]; ax.add_patch(Polygon(pts,closed=True,fill=False,linewidth=1.5)); ax.text(float(pts[:,0].mean()),float(pts[:,1].mean()),label,ha='center',va='center',fontsize=8)
    ax.scatter([LIGHT[0]],[LIGHT[1]],marker='*',s=120,label='Static light')
    for sid in (14,15,16):
        s=SEGS[sid]; a,b=VERTS[s.v0],VERTS[s.v1]; ax.plot([a[0],b[0]],[a[1],b[1]],linewidth=2)
    ax.set_aspect('equal','box');ax.set_xlim(8,184);ax.set_ylim(90,0);ax.set_xlabel('world X');ax.set_ylabel('world Y')

def save_scatter(rows,key,title,path,cbar):
    fig,ax=plt.subplots(figsize=(11,5.5));draw_world(ax);sc=ax.scatter([r['x'] for r in rows],[r['y'] for r in rows],c=[r[key] for r in rows],s=48,marker='s');fig.colorbar(sc,ax=ax,label=cbar);ax.set_title(title);fig.tight_layout();fig.savefig(path,dpi=180);plt.close(fig)

def mask_image(e,exact,quant,text):
    a=np.zeros((H,W,3),np.uint8);a[:]=[12,14,18];a[e&~exact]=[48,54,66];a[e&exact]=[190,198,215];a[e&(exact^quant)]=[230,80,80]
    img=Image.fromarray(a,'RGB').resize((W*3,H*3),Image.Resampling.NEAREST);d=ImageDraw.Draw(img);d.rectangle((0,0,img.width,22),fill=(0,0,0));d.text((4,4),text,fill=(255,255,255));return img

def write_contact_sheet(worst,outdir):
    imgs=[]
    for r in worst[:8]:
        e,x=exact_floor_frame(r['x'],r['y'],r['yaw']);q,m=quantize_frame(e,x);imgs.append(mask_image(e,x,q,f"x={r['x']:.0f} y={r['y']:.0f} yaw={r['yaw']} err={r['mismatch_pct']:.3f}% mixed={m['mixed_tiles']}"))
    if not imgs:return
    sheet=Image.new('RGB',(imgs[0].width*2,imgs[0].height*math.ceil(len(imgs)/2)),(20,20,20))
    for i,img in enumerate(imgs):sheet.paste(img,((i%2)*img.width,(i//2)*img.height))
    sheet.save(outdir/'worst_frames_contact_sheet.png')

def path_poses():
    way=[(40,50,0),(72,50,0),(96,50,0),(124,50,0),(150,50,0),(150,30,64),(150,70,192),(112,50,128),(72,50,128),(40,50,128)];out=[]
    for a,b in zip(way,way[1:]):
      for k in range(12):
        t=k/12.;dy=((b[2]-a[2]+128)%256)-128;p=(a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,int(round(a[2]+dy*t))&255)
        if point_in_any(p[0],p[1]):out.append(p)
    return out

def make_video_frames(outdir,baseline):
    frames=outdir/'video_frames';frames.mkdir(exist_ok=True)
    for i,(cx,cy,yaw) in enumerate(path_poses()):
        e,x=exact_floor_frame(cx,cy,yaw);q,m=quantize_frame(e,x);pct=100.*m['mismatch_pixels']/max(1,int(e.sum()));arr=np.zeros((H,W*2,3),np.uint8)
        for j,mask in enumerate((x,q)):
            h=np.zeros((H,W,3),np.uint8);h[:]=[12,14,18];h[e&~mask]=[48,54,66];h[e&mask]=[190,198,215];arr[:,j*W:(j+1)*W]=h
        img=Image.fromarray(arr,'RGB').resize((W*4,H*2),Image.Resampling.NEAREST);d=ImageDraw.Draw(img);d.rectangle((0,0,img.width,32),fill=(0,0,0));perf='baseline pending' if baseline is None else f'baseline GG lattice mean={baseline:.2f} upd/s';d.text((6,4),f'LIGHT ORACLE vs EDGE QUANTIZED | mismatch={pct:.3f}% | {perf}',fill=(255,255,255));d.text((6,18),'Analysis-only lighting: runtime light cost not included in FPS.',fill=(255,220,120));img.save(frames/f'{i:04d}.png')

def parse_baseline(path:Optional[Path])->Optional[float]:
    if not path or not path.exists():return None
    for line in path.read_text(errors='ignore').splitlines():
        if 'representative_manual_mean_effective_updates_per_s=' in line:
            try:return float(line.split('=',1)[1])
            except ValueError:return None
    return None

def main()->int:
    ap=argparse.ArgumentParser();ap.add_argument('--out',default='build/lattice-lighting');ap.add_argument('--step',type=int,default=12);ap.add_argument('--yaw-step',type=int,default=16);ap.add_argument('--baseline-perf');args=ap.parse_args();out=Path(args.out);out.mkdir(parents=True,exist_ok=True);samples=traversable_samples(args.step,True);poses=[];pos=[];global_sigs=set()
    for cx,cy in samples:
        local=[]
        for yaw in range(0,256,args.yaw_step):
            e,x=exact_floor_frame(cx,cy,yaw);q,m=quantize_frame(e,x);p=apply_penumbra(q,e);ep=int(e.sum());pct=100.*m['mismatch_pixels']/max(1,ep);padd=int((p&~q).sum());global_sigs.update(m['signatures']);r=dict(x=cx,y=cy,yaw=yaw,eligible_pixels=ep,mismatch_pixels=m['mismatch_pixels'],mismatch_pct=pct,mixed_tiles=m['mixed_tiles'],boundary_eligible_pixels=m['boundary_eligible_pixels'],penumbra_added_pixels=padd,unique_edge_signatures_frame=len(set(m['signatures'])));poses.append(r);local.append(r)
        pos.append(dict(x=cx,y=cy,worst_mismatch_pct=max(r['mismatch_pct'] for r in local),mean_mismatch_pct=sum(r['mismatch_pct'] for r in local)/len(local),max_mixed_tiles=max(r['mixed_tiles'] for r in local),mean_mixed_tiles=sum(r['mixed_tiles'] for r in local)/len(local),max_penumbra_pixels=max(r['penumbra_added_pixels'] for r in local)))
    with (out/'pose_metrics.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=list(poses[0]));w.writeheader();w.writerows(poses)
    with (out/'position_metrics.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=list(pos[0]));w.writeheader();w.writerows(pos)
    save_scatter(pos,'worst_mismatch_pct','Floor hard-shadow quantization: worst error by camera position',out/'map_shadow_error.png','worst eligible-pixel mismatch (%)');save_scatter(pos,'mean_mixed_tiles','Floor hard-shadow boundary pressure',out/'map_mixed_tiles.png','mean mixed 8x8 receiver tiles / heading');save_scatter(pos,'max_penumbra_pixels','One-sided penumbra footprint',out/'map_penumbra_pixels.png','max extra ordered-dither pixels / heading');worst=sorted(poses,key=lambda r:(r['mismatch_pct'],r['mixed_tiles']),reverse=True);write_contact_sheet(worst,out);baseline=parse_baseline(Path(args.baseline_perf) if args.baseline_perf else None);make_video_frames(out,baseline);vals=np.asarray([r['mismatch_pct'] for r in poses]);mixed=np.asarray([r['mixed_tiles'] for r in poses])
    with (out/'SUMMARY.txt').open('w') as f:
        f.write('=== FREE-CAMERA LATTICE LIGHTING FUSION PROBE ===\n');f.write(f'positions={len(samples)} headings_per_position={256//args.yaw_step} poses={len(poses)}\n');f.write('oracle=mature portal-penumbra finite-height blocking semantics, arbitrary cameras\n');f.write('approximation=reusable 8x8 straight-edge family per mixed floor tile\n');f.write(f'mismatch_pct mean={vals.mean():.6f} p95={np.percentile(vals,95):.6f} max={vals.max():.6f}\n');f.write(f'mixed_tiles mean={mixed.mean():.3f} p95={np.percentile(mixed,95):.3f} max={mixed.max():.0f}\n');f.write(f'global_quantized_edge_signatures={len(global_sigs)}\n');f.write(f'baseline_lattice_updates_per_s={"unknown" if baseline is None else f"{baseline:.3f}"}\n');f.write('IMPORTANT: baseline rate excludes runtime lighting; this phase measures visual/representation pressure.\n');f.write('NEXT: port only the measured small boundary vocabulary into live Polar and cycle-profile the semantic light-edge pass.\n')
    print((out/'SUMMARY.txt').read_text());return 0

if __name__=='__main__':raise SystemExit(main())
