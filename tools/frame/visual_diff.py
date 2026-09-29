#!/usr/bin/env python3
"""Pixel-level A/B for same-source coarse vs direct-mixed Game Gear frames.

Inputs are settled 160x144 PPM captures matched by exact camera state. Output:
  * machine/LLM-readable visual-diff.json
  * per-pose reference|direct|difference triptych PNG
  * one stacked montage PNG
The difference panel is dim grayscale where identical, magenta where pixels
changed, with yellow boxes around the largest 4-connected change components.
"""
from __future__ import annotations
import argparse,csv,json,os,struct,zlib
from collections import Counter,deque
from pathlib import Path

def read_ppm(path: Path):
    with path.open("rb") as f:
        if f.readline().strip()!=b"P6":
            raise ValueError(f"{path}: not P6")
        line=f.readline()
        while line.startswith(b"#"):
            line=f.readline()
        w,h=map(int,line.split())
        maxv=int(f.readline())
        if maxv!=255: raise ValueError(f"{path}: maxval {maxv}")
        data=f.read()
    if len(data)!=w*h*3:
        raise ValueError(f"{path}: expected {w*h*3} RGB bytes, got {len(data)}")
    return w,h,data

def write_png(path: Path,w:int,h:int,rgb:bytes):
    if len(rgb)!=w*h*3: raise ValueError("bad RGB length")
    raw=b"".join(b"\x00"+rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(tag,data):
        return struct.pack(">I",len(data))+tag+data+struct.pack(">I",zlib.crc32(tag+data)&0xffffffff)
    png=(b"\x89PNG\r\n\x1a\n"+
         chunk(b"IHDR",struct.pack(">IIBBBBB",w,h,8,2,0,0,0))+
         chunk(b"IDAT",zlib.compress(raw,9))+chunk(b"IEND",b""))
    path.write_bytes(png)

def components(mask,w,h):
    seen=bytearray(w*h); out=[]
    for seed,v in enumerate(mask):
        if not v or seen[seed]: continue
        q=[seed]; seen[seed]=1; n=0
        minx=maxx=seed%w; miny=maxy=seed//w
        while q:
            p=q.pop(); n+=1; x=p%w; y=p//w
            minx=min(minx,x);maxx=max(maxx,x);miny=min(miny,y);maxy=max(maxy,y)
            if x and mask[p-1] and not seen[p-1]: seen[p-1]=1;q.append(p-1)
            if x+1<w and mask[p+1] and not seen[p+1]: seen[p+1]=1;q.append(p+1)
            if y and mask[p-w] and not seen[p-w]: seen[p-w]=1;q.append(p-w)
            if y+1<h and mask[p+w] and not seen[p+w]: seen[p+w]=1;q.append(p+w)
        bw=maxx-minx+1; bh=maxy-miny+1
        if bw<=3 and bh>=max(4,bw*2): shape="vertical"
        elif bh<=3 and bw>=max(4,bh*2): shape="horizontal"
        elif bw<=4 and bh<=4: shape="point"
        else: shape="region"
        out.append(dict(pixels=n,bbox=[minx,miny,maxx,maxy],width=bw,height=bh,shape=shape))
    out.sort(key=lambda c:c["pixels"],reverse=True)
    return out

def draw_box(buf,w,h,bbox,color=(255,255,0)):
    x0,y0,x1,y1=bbox
    for x in range(max(0,x0),min(w,x1+1)):
        for y in (y0,y1):
            if 0<=y<h:
                i=(y*w+x)*3;buf[i:i+3]=bytes(color)
    for y in range(max(0,y0),min(h,y1+1)):
        for x in (x0,x1):
            if 0<=x<w:
                i=(y*w+x)*3;buf[i:i+3]=bytes(color)

def triptych(ref,direct,w,h,comps):
    sep=2; tw=w*3+sep*2
    out=bytearray(tw*h*3)
    # reference and direct panels
    for y in range(h):
        for panel,src in ((0,ref),(1,direct)):
            dstx=panel*(w+sep)
            a=y*w*3;b=(y+1)*w*3
            o=(y*tw+dstx)*3
            out[o:o+w*3]=src[a:b]
    # difference panel: dim grayscale unchanged, magenta changed.
    dx0=2*(w+sep)
    mask=bytearray(w*h)
    for p in range(w*h):
        i=p*3
        changed=ref[i:i+3]!=direct[i:i+3]
        mask[p]=1 if changed else 0
        if changed: rgb=(255,0,255)
        else:
            v=(int(ref[i])+int(ref[i+1])+int(ref[i+2]))//6
            rgb=(v,v,v)
        x=p%w;y=p//w;o=(y*tw+dx0+x)*3
        out[o:o+3]=bytes(rgb)
    # yellow component boxes on diff panel
    panel=bytearray(w*h*3)
    for y in range(h):
        a=(y*tw+dx0)*3
        panel[y*w*3:(y+1)*w*3]=out[a:a+w*3]
    for c in comps[:12]: draw_box(panel,w,h,c["bbox"])
    for y in range(h):
        a=y*w*3;b=(y+1)*w*3;o=(y*tw+dx0)*3
        out[o:o+w*3]=panel[a:b]
    return tw,h,bytes(out)

def load_meta(d:Path):
    rows=list(csv.DictReader((d/"poses.csv").open(newline="")))
    by={}
    for r in rows:
        key=(int(r["x_q4"]),int(r["y_q4"]),int(r["z_q4"]),int(r["yaw"]))
        by[key]=r
    return rows,by

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--reference",required=True)
    ap.add_argument("--direct",required=True)
    ap.add_argument("--out",required=True)
    ap.add_argument("--min-matches",type=int,default=1)
    a=ap.parse_args()
    refd=Path(a.reference);dird=Path(a.direct);outd=Path(a.out);outd.mkdir(parents=True,exist_ok=True)
    refrows,ref=load_meta(refd);_,direct=load_meta(dird)
    keys=[]
    for r in refrows:
        k=(int(r["x_q4"]),int(r["y_q4"]),int(r["z_q4"]),int(r["yaw"]))
        if k in direct and k not in keys: keys.append(k)
    if len(keys)<a.min_matches:
        raise SystemExit(f"VISUAL_DIFF insufficient matched settled poses {len(keys)} < {a.min_matches}")

    report={"reference":str(refd),"direct":str(dird),"matched_poses":len(keys),"poses":[]}
    montage=[]; mw=mh=None
    total_changed=0;total_pixels=0
    ref_geometry_pixels=0;direct_geometry_pixels=0
    # Geometry-only palette background: ceiling, floor and black horizon/edges.
    # Any other RGB is wall material. This is a sanity gate, not the visual
    # oracle itself: a completely background-only "reference" must never make
    # a large A/B diff look like an exact-X correctness result.
    background={(17,17,51),(34,34,51),(0,0,0)}
    for pi,k in enumerate(keys):
        rr=ref[k];dr=direct[k]
        w,h,rgb0=read_ppm(refd/rr["file"]);w1,h1,rgb1=read_ppm(dird/dr["file"])
        if (w,h)!=(w1,h1): raise SystemExit("capture dimensions differ")
        mask=bytearray(w*h); changed=[]
        pairs=Counter();cols=Counter();rows=Counter();xmod=Counter();ymod=Counter()
        pose_ref_geom=pose_direct_geom=0
        for p in range(w*h):
            i=p*3
            a0=tuple(rgb0[i:i+3]);b0=tuple(rgb1[i:i+3])
            pose_ref_geom += int(a0 not in background)
            pose_direct_geom += int(b0 not in background)
            if a0!=b0:
                mask[p]=1;changed.append(p);pairs[(a0,b0)]+=1
                x=p%w;y=p//w;cols[x]+=1;rows[y]+=1;xmod[x&7]+=1;ymod[y&7]+=1
        comps=components(mask,w,h)
        bbox=None
        if changed:
            xs=[p%w for p in changed];ys=[p//w for p in changed]
            bbox=[min(xs),min(ys),max(xs),max(ys)]
        total_changed+=len(changed);total_pixels+=w*h
        ref_geometry_pixels+=pose_ref_geom
        direct_geometry_pixels+=pose_direct_geom
        rec={
          "index":pi,"state":{"x_q4":k[0],"y_q4":k[1],"z_q4":k[2],"yaw":k[3]},
          "reference_file":rr["file"],"direct_file":dr["file"],
          "changed_pixels":len(changed),"total_pixels":w*h,
          "changed_pct":round(100.0*len(changed)/(w*h),4),
          "reference_geometry_pixels":pose_ref_geom,
          "direct_geometry_pixels":pose_direct_geom,
          "bbox":bbox,
          "components":comps[:30],
          "component_count":len(comps),
          "largest_component_pixels":comps[0]["pixels"] if comps else 0,
          "top_changed_columns":cols.most_common(12),
          "top_changed_rows":rows.most_common(12),
          "x_mod_8_hist":[xmod[i] for i in range(8)],
          "y_mod_8_hist":[ymod[i] for i in range(8)],
          "top_rgb_changes":[
             {"reference":list(aa),"direct":list(bb),"pixels":n}
             for (aa,bb),n in pairs.most_common(16)
          ]
        }
        tw,th,img=triptych(rgb0,rgb1,w,h,comps)
        png=f"pose-{pi:02d}-triptych.png";write_png(outd/png,tw,th,img)
        rec["triptych"]=png
        report["poses"].append(rec)
        montage.append(img);mw=tw;mh=th
        print(f"VISUAL_POSE index={pi} state={k} changed={len(changed)}/{w*h} "
              f"({rec['changed_pct']:.3f}%) ref_geom={pose_ref_geom} direct_geom={pose_direct_geom} "
              f"components={len(comps)} bbox={bbox} largest={rec['largest_component_pixels']}")

    spacer=4
    H=len(montage)*mh+(len(montage)-1)*spacer
    mb=bytearray(mw*H*3); y0=0
    for img in montage:
        for y in range(mh):
            mb[(y0+y)*mw*3:(y0+y+1)*mw*3]=img[y*mw*3:(y+1)*mw*3]
        y0+=mh+spacer
    write_png(outd/"visual-diff-montage.png",mw,H,bytes(mb))
    report["total_changed_pixels"]=total_changed
    report["total_compared_pixels"]=total_pixels
    report["reference_geometry_pixels"]=ref_geometry_pixels
    report["direct_geometry_pixels"]=direct_geometry_pixels
    if ref_geometry_pixels==0:
        raise SystemExit("VISUAL_DIFF invalid reference: all matched poses are background-only")
    if direct_geometry_pixels==0:
        raise SystemExit("VISUAL_DIFF invalid direct build: all matched poses are background-only")
    report["overall_changed_pct"]=round(100.0*total_changed/total_pixels,4)
    report["montage"]="visual-diff-montage.png"
    (outd/"visual-diff.json").write_text(json.dumps(report,indent=2))
    print(f"VISUAL_DIFF matched={len(keys)} total_changed={total_changed}/{total_pixels} "
          f"({report['overall_changed_pct']:.4f}%) json={outd/'visual-diff.json'} "
          f"montage={outd/'visual-diff-montage.png'}")

if __name__=="__main__":
    main()
