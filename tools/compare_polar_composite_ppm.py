#!/usr/bin/env python3
"""Semantic pixel regression for baked Polar host-vs-Game-Gear proof frames."""
from pathlib import Path
import sys

FRAMES=(120,300,520,760,1000,1113)
HOST={
    (0,0,0):0,(16,16,48):1,(64,64,96):2,
    (96,112,144):3,(144,160,192):4,(208,224,240):5,
}
GG={
    (0,0,0):0,(16,16,49):1,(32,32,49):2,
    (49,68,98):3,(98,117,156):4,(172,186,222):5,
}

def read_ppm(path):
    data=Path(path).read_bytes()
    parts=data.split(b"\n",3)
    if len(parts)!=4 or parts[0]!=b"P6":
        raise ValueError(f"{path}: expected simple P6 PPM")
    w,h=map(int,parts[1].split())
    if parts[2].strip()!=b"255":
        raise ValueError(f"{path}: expected maxval 255")
    pix=parts[3]
    if len(pix)!=w*h*3:
        raise ValueError(f"{path}: pixel payload size mismatch")
    return w,h,pix

def semantics(pix,palette,path):
    out=bytearray(len(pix)//3)
    for i in range(0,len(pix),3):
        rgb=(pix[i],pix[i+1],pix[i+2])
        try: out[i//3]=palette[rgb]
        except KeyError: raise ValueError(f"{path}: unknown RGB {rgb}")
    return out

def main(root):
    root=Path(root)
    total=0
    for frame in FRAMES:
        hp=root/"generated/polar_demo_patch"/f"host_composite_frame_{frame}.ppm"
        gp=root/"composite-shots"/f"patch_{frame}.ppm"
        hw,hh,hpix=read_ppm(hp);gw,gh,gpix=read_ppm(gp)
        if (hw,hh)!=(gw,gh):
            raise SystemExit(f"frame {frame}: dimensions differ")
        hs=semantics(hpix,HOST,hp);gs=semantics(gpix,GG,gp)
        mismatches=sum(a!=b for a,b in zip(hs,gs))
        total+=mismatches
        pct=100.0*mismatches/len(hs)
        print(f"frame={frame} semantic_pixel_mismatch={mismatches}/{len(hs)} ({pct:.6f}%)")
    print(f"POLAR_COMPOSITE_SEMANTIC_PIXEL_EXACT={int(total==0)} total_mismatch={total}")
    return 0 if total==0 else 1

if __name__=="__main__":
    if len(sys.argv)!=2:
        raise SystemExit(f"usage: {sys.argv[0]} ARTIFACT_ROOT")
    raise SystemExit(main(sys.argv[1]))
