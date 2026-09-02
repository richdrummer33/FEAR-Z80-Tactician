#!/usr/bin/env python3
"""Extract a geometry-only Kleiner-lab reference crop from a Source 1 BSP.

This is an authoring aid, not part of the Game Gear runtime and not a Valve
asset redistributor. Point it at a locally owned/provided d1_trainstation_05.bsp.
It emits an OBJ whose only purpose is measurement / silhouette comparison before
the scene is re-authored into the tiny semantic GG vocabulary.
"""
import argparse
import re
import struct
from pathlib import Path

DEFAULT_CROP=(-7350.0,-6300.0,-1700.0,-1000.0,-32.0,700.0)

def get_lump(data,index):
    off=8+index*16
    fileofs,filelen,version,fourcc=struct.unpack_from("<iii4s",data,off)
    return memoryview(data)[fileofs:fileofs+filelen]

def cstr(blob,off):
    raw=bytes(blob)
    end=raw.find(b"\0",off)
    if end<0:end=len(raw)
    return raw[off:end].decode("latin1","replace")

def clean_material(name):
    name=name.strip("/").replace("\\","/")
    return re.sub(r"[^A-Za-z0-9_.-]+","_",name) or "material"

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("bsp")
    ap.add_argument("obj")
    ap.add_argument("--scale",type=float,default=0.1)
    ap.add_argument("--origin",default="-7312,-1664,0")
    ap.add_argument("--crop",default=",".join(map(str,DEFAULT_CROP)))
    args=ap.parse_args()

    ox,oy,oz=map(float,args.origin.split(","))
    crop=tuple(map(float,args.crop.split(",")))
    if len(crop)!=6:
        raise SystemExit("--crop needs x0,x1,y0,y1,z0,z1")

    data=Path(args.bsp).read_bytes()
    if data[:4]!=b"VBSP":
        raise SystemExit("not a Source VBSP")
    version=struct.unpack_from("<i",data,4)[0]

    verts_l=get_lump(data,3)
    edges_l=get_lump(data,12)
    surf_l=get_lump(data,13)
    faces_l=get_lump(data,7)
    texinfo_l=get_lump(data,6)
    texdata_l=get_lump(data,2)
    strdata=get_lump(data,43)
    strtable=get_lump(data,44)

    verts=[struct.unpack_from("<fff",verts_l,i) for i in range(0,len(verts_l),12)]
    edges=[struct.unpack_from("<HH",edges_l,i) for i in range(0,len(edges_l),4)]
    surfedges=[struct.unpack_from("<i",surf_l,i)[0] for i in range(0,len(surf_l),4)]
    str_offsets=[struct.unpack_from("<i",strtable,i)[0] for i in range(0,len(strtable),4)]

    texdata=[]
    for i in range(0,len(texdata_l),32):
        sid=struct.unpack_from("<i",texdata_l,i+12)[0]
        name="UNKNOWN"
        if 0<=sid<len(str_offsets):
            name=cstr(strdata,str_offsets[sid]).upper()
        texdata.append(name)

    texinfo=[]
    for i in range(0,len(texinfo_l),72):
        texinfo.append(struct.unpack_from("<i",texinfo_l,i+68)[0])

    selected=[]
    for i in range(0,len(faces_l),56):
        firstedge=struct.unpack_from("<i",faces_l,i+4)[0]
        numedges=struct.unpack_from("<H",faces_l,i+8)[0]
        ti=struct.unpack_from("<h",faces_l,i+10)[0]
        if numedges<3 or firstedge<0 or firstedge+numedges>len(surfedges):
            continue
        poly=[]
        for j in range(numedges):
            se=surfedges[firstedge+j]
            ei=abs(se)
            if ei>=len(edges):
                poly=[];break
            edge=edges[ei]
            vi=edge[0] if se>=0 else edge[1]
            if vi>=len(verts):
                poly=[];break
            poly.append(verts[vi])
        if len(poly)<3:
            continue

        cx=sum(p[0] for p in poly)/len(poly)
        cy=sum(p[1] for p in poly)/len(poly)
        cz=sum(p[2] for p in poly)/len(poly)
        if not (crop[0]<=cx<=crop[1] and crop[2]<=cy<=crop[3] and crop[4]<=cz<=crop[5]):
            continue

        xs=[p[0] for p in poly];ys=[p[1] for p in poly];zs=[p[2] for p in poly]
        if max(xs)-min(xs)>1200 or max(ys)-min(ys)>1200 or max(zs)-min(zs)>900:
            continue

        material="UNKNOWN"
        if 0<=ti<len(texinfo):
            td=texinfo[ti]
            if 0<=td<len(texdata):
                material=texdata[td]
        if (material.startswith(("TOOLS/","SKYBOX/","NATURE/TOOL")) or
            "NODRAW" in material or "TRIGGER" in material or "CLIP" in material):
            continue
        selected.append((material,poly))

    out=Path(args.obj)
    lines=[
        f"# d1_trainstation_05 Kleiner lab reference crop; Source BSP v{version}",
        f"# crop={crop} scale={args.scale} origin={(ox,oy,oz)}",
    ]
    vertex_index=1
    triangles=0
    materials={}
    for material,poly in selected:
        for x,y,z in poly:
            lines.append(
                f"v {(x-ox)*args.scale:.6f} {(y-oy)*args.scale:.6f} "
                f"{(z-oz)*args.scale:.6f}")
        mat=clean_material(material)
        materials[mat]=materials.get(mat,0)+1
        lines.append(f"usemtl {mat}")
        for k in range(1,len(poly)-1):
            lines.append(f"f {vertex_index} {vertex_index+k} {vertex_index+k+1}")
            triangles+=1
        vertex_index+=len(poly)
    out.write_text("\n".join(lines)+"\n")

    manifest=out.with_suffix(".manifest.txt")
    manifest.write_text("\n".join([
        f"bsp_version={version}",
        f"faces={len(selected)}",
        f"triangles={triangles}",
        f"vertices_written={vertex_index-1}",
        f"materials={len(materials)}",
        *[
            f"material {name} faces={count}"
            for name,count in sorted(materials.items(),key=lambda kv:(-kv[1],kv[0]))
        ],
    ])+"\n")
    print(
        f"HL2_LAB_EXTRACT_PASS faces={len(selected)} triangles={triangles} "
        f"vertices={vertex_index-1} materials={len(materials)}")

if __name__=="__main__":
    main()
