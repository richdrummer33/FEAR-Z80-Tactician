#!/usr/bin/env python3
"""Generate a 1-world-cell local bearing field for exact E1M1 vertices.

This is a performance ROM, not topology. The exact-Q4 visibility envelope still
chooses which boundary vertices matter. For each 1x1 world cell and each map
vertex, first bake the cheap affine form:

    bearing(local_x,local_y) ~= base + sx*local_x/16 + sy*local_y/16

where local_x/local_y are the player's Q4 fractional coordinates 0..15.
Every representable Q4 point is exhaustively checked against atan2. When the
original tangent fit misses --threshold, a second fit uses the four cell corners
and may add a tiny signed 4-bit bilinear term:

    cross = trunc(trunc(sxy*local_x/16)*local_y/16)

The signed sxy coefficient is packed into the otherwise-unused high nibble of
base_hi, so the bilinear rescue costs ZERO extra ROM bytes and ZERO extra cell
load bytes. sxy=0 is the normal affine fast path. Only rescued records with a
non-zero nibble execute the extra two 8x4 multiplies. Records that still exceed
the threshold retain their fallback bit and use the exact bearing LUT.

The record remains fixed-size: 4 fallback bytes + 30 * 4-byte descriptors =
124 bytes/cell. One world row (96 cells) is 11,904 bytes and therefore fits
comfortably in one 16 KiB ROM bank. Cell loads happen only when the integer
player cell changes; per-frame visible vertex evaluation then runs from WRAM.
"""
from __future__ import annotations
import argparse, math, pathlib, re

TAU=math.tau
QTURN=4096.0

def arr(text,name):
    m=re.search(r"static\s+const\s+[^;=]+?\b"+re.escape(name)+r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};",text,re.S)
    if not m: raise SystemExit("missing generated array "+name)
    return [int(x,0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+",m.group(1))]

def wrap(v):
    return ((v+2048.0)%4096.0)-2048.0

def bearing(vx,vy,xq,yq):
    dx=vx*16.0-xq; dy=vy*16.0-yq
    if dx*dx+dy*dy < 1e-12: return None
    return (math.atan2(dy,dx)*QTURN/TAU)%QTURN

def shr0(v,n=4):
    return v>>n if v>=0 else -((-v)>>n)

def sample_grid(vx,vy,wx,wy):
    x0=wx*16; y0=wy*16
    out=[]
    for ly in range(16):
      for lx in range(16):
        e=bearing(vx,vy,x0+lx,y0+ly)
        if e is None:
            return None
        out.append(e)
    return out

def affine_record(vx,vy,wx,wy,threshold,exact):
    x0=wx*16; y0=wy*16
    cx=x0+7.5; cy=y0+7.5
    X=vx*16.0-cx; Y=vy*16.0-cy
    r2=X*X+Y*Y
    if r2<0.25: return None,float("inf")
    center=bearing(vx,vy,cx,cy)
    scale=QTURN/TAU
    dx=(Y/r2)*scale
    dy=(-X/r2)*scale
    # Intercept of the center-tangent plane at local (0,0).
    base=(center-dx*7.5-dy*7.5)%QTURN
    sx=round(dx*16.0); sy=round(dy*16.0)
    if not(-127<=sx<=127 and -127<=sy<=127):
        return None,float("inf")
    b=int(round(base))&4095
    worst=0.0
    i=0
    for ly in range(16):
      for lx in range(16):
        p=b+shr0(sx*lx)+shr0(sy*ly)
        worst=max(worst,abs(wrap(exact[i]-p))); i+=1
    if worst>threshold:
        return None,worst
    return (b,sx,sy,0),worst

def nearest(v,ref):
    return ref+wrap(v-ref)

def bilinear_record(vx,vy,wx,wy,threshold,exact):
    """Second-chance fit matching the Z80 integer evaluator exactly."""
    e00=exact[0]; e10=nearest(exact[15],e00); e01=nearest(exact[240],e00)
    e11=nearest(exact[255],(e10+e01-e00))
    sx=int(round((e10-e00)*16.0/15.0))
    sy=int(round((e01-e00)*16.0/15.0))
    sxy=int(round((e11-e10-e01+e00)*256.0/225.0))
    if not(-127<=sx<=127 and -127<=sy<=127 and -8<=sxy<=7):
        return None,float("inf")
    b0=int(round(e00))
    # For fixed quantised slopes, changing base adds the same integer to every
    # prediction. Solve that one-dimensional Chebyshev adjustment directly
    # instead of brute-forcing seven complete 16x16 passes.
    residual=[]; i=0
    for ly in range(16):
      for lx in range(16):
        cross=shr0(shr0(sxy*lx)*ly)
        p=b0+shr0(sx*lx)+shr0(sy*ly)+cross
        residual.append(wrap(exact[i]-p)); i+=1
    mid=(min(residual)+max(residual))*0.5
    c0=math.floor(mid)
    candidates=(c0,c0+1)
    best=None; best_worst=float("inf")
    for db in candidates:
        worst=max(abs(wrap(r-db)) for r in residual)
        if worst<best_worst:
            best_worst=worst; best=((b0+db)&4095,sx,sy,sxy)
    if best_worst>threshold:
        return None,best_worst
    return best,best_worst

def record(vx,vy,wx,wy,threshold):
    exact=sample_grid(vx,vy,wx,wy)
    if exact is None:
        return None,float("inf"),"fallback"
    rec,err=affine_record(vx,vy,wx,wy,threshold,exact)
    if rec is not None:
        return rec,err,"affine"
    rec2,err2=bilinear_record(vx,vy,wx,wy,threshold,exact)
    if rec2 is not None:
        return rec2,err2,"linear-rescue" if rec2[3]==0 else "bilinear-rescue"
    return None,min(err,err2),"fallback"

def emit_u8(name,data,per=24):
    out=[f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0,len(data),per):
        out.append("    "+", ".join(str(v) for v in data[i:i+per])+",")
    out.append("};")
    return "\n".join(out)

def emit_u16(name,data,per=12):
    out=[f"static const uint16_t {name}[{len(data)}] = {{"]
    for i in range(0,len(data),per):
        out.append("    "+", ".join(str(v) for v in data[i:i+per])+",")
    out.append("};")
    return "\n".join(out)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--map-inc",required=True)
    ap.add_argument("--out-dir",required=True)
    ap.add_argument("--bank-base",type=int,default=144)
    ap.add_argument("--dispatch-bank",type=int,default=200)
    ap.add_argument("--threshold",type=float,default=2.0)
    ap.add_argument("--world-min-x",type=int,default=16)
    ap.add_argument("--world-min-y",type=int,default=24)
    ap.add_argument("--world-max-x",type=int,default=112)
    ap.add_argument("--world-max-y",type=int,default=80)
    a=ap.parse_args()

    text=pathlib.Path(a.map_inc).read_text()
    vx=arr(text,"k_tspf_vx"); vy=arr(text,"k_tspf_vy")
    if len(vx)!=len(vy) or len(vx)>32:
        raise SystemExit(f"unsupported vertex count {len(vx)}/{len(vy)}")
    nv=len(vx)
    width=a.world_max_x-a.world_min_x
    height=a.world_max_y-a.world_min_y
    if a.bank_base+height>a.dispatch_bank:
        raise SystemExit("row banks collide with dispatch bank")

    outdir=pathlib.Path(a.out_dir); outdir.mkdir(parents=True,exist_ok=True)
    cell_bytes=4+nv*4
    fallback=affine=linear_rescue=bilinear_rescue=0
    worst_accepted=0.0; worst_all=0.0

    for ry in range(height):
        wy=a.world_min_y+ry
        cells=bytearray()
        for rx in range(width):
            wx=a.world_min_x+rx
            mask=0
            desc=[]
            for vid,(x,y) in enumerate(zip(vx,vy)):
                rec,err,kind=record(x,y,wx,wy,a.threshold)
                if math.isfinite(err): worst_all=max(worst_all,err)
                if rec is None:
                    mask|=1<<vid; fallback+=1
                    desc.extend((0,0,0,0))
                else:
                    b,sx,sy,sxy=rec
                    if kind=="affine": affine+=1
                    elif kind=="linear-rescue": linear_rescue+=1
                    else: bilinear_rescue+=1
                    worst_accepted=max(worst_accepted,err)
                    # low nibble = base bits 8..11; high nibble = signed sxy.
                    # The existing evaluator already masks the low nibble for
                    # base, so old affine records remain representation-compatible.
                    desc.extend((b&255,((b>>8)&15)|((sxy&15)<<4),sx&255,sy&255))
            cells.extend((mask&255,(mask>>8)&255,(mask>>16)&255,(mask>>24)&255))
            cells.extend(desc)
        fn=f"e1env_bearing_row_{ry}"
        offsets=[i*cell_bytes for i in range(width)]
        src=f"""/* GENERATED by tools/e1env_local_bearing_field.py. */
#include <stdint.h>
#include <string.h>
#include <gbdk/platform.h>
#pragma bank {a.bank_base+ry}
BANKREF(e1env_bearing_row_{ry})
#define CELL_BYTES {cell_bytes}u
{emit_u16("k_off",offsets)}
{emit_u8("k_cells",cells)}
void {fn}(uint8_t x,uint8_t *dst) BANKED {{
    memcpy(dst,&k_cells[k_off[x]],CELL_BYTES);
}}
"""
        (outdir/f"{fn}.c").write_text(src)

    hdr=[
      "/* GENERATED exact-envelope local bearing field. */",
      "#ifndef E1ENV_LOCAL_BEARING_FIELD_H",
      "#define E1ENV_LOCAL_BEARING_FIELD_H",
      "#include <stdint.h>",
      "#include <gbdk/platform.h>",
      f"#define E1ENV_LOCAL_BEARING_FIELD 1u",
      f"#define E1ENV_LBF_BILINEAR_NIBBLE 1u",
      f"#define E1ENV_LBF_WORLD_MIN_X {a.world_min_x}",
      f"#define E1ENV_LBF_WORLD_MIN_Y {a.world_min_y}",
      f"#define E1ENV_LBF_WIDTH {width}u",
      f"#define E1ENV_LBF_HEIGHT {height}u",
      f"#define E1ENV_LBF_VERTEX_COUNT {nv}u",
      f"#define E1ENV_LBF_CELL_BYTES {cell_bytes}u",
      "void e1env_local_bearing_load(uint8_t wx,uint8_t wy,uint8_t *dst) BANKED;",
      "#endif",""
    ]
    (outdir/"e1env_local_bearing_field.h").write_text("\n".join(hdr))

    dispatch=[
      f"#pragma bank {a.dispatch_bank}",
      "#include <stdint.h>",
      "#include <gbdk/platform.h>",
    ]
    for ry in range(height):
        dispatch.append(f"void e1env_bearing_row_{ry}(uint8_t x,uint8_t *dst) BANKED;")
    dispatch += [
      "void e1env_local_bearing_load(uint8_t wx,uint8_t wy,uint8_t *dst) BANKED {",
      f"    if(wx>={width}u || wy>={height}u) return;",
      "    switch(wy) {",
    ]
    for ry in range(height):
        dispatch.append(f"    case {ry}u: e1env_bearing_row_{ry}(wx,dst); break;")
    dispatch += ["    default: break;","    }","}",""]
    (outdir/"e1env_local_bearing_dispatch.c").write_text("\n".join(dispatch))

    total=affine+linear_rescue+bilinear_rescue+fallback
    rescued=linear_rescue+bilinear_rescue
    print(f"E1ENV_LOCAL_BEARING_FIELD cells={width*height} vertices={nv} cell_bytes={cell_bytes} "
          f"row_banks={height} bank_range={a.bank_base}..{a.bank_base+height-1} dispatch_bank={a.dispatch_bank}")
    print(f"threshold_q12={a.threshold:g} affine={affine}/{total} linear_rescue={linear_rescue} "
          f"bilinear_rescue={bilinear_rescue} rescued={rescued} fallback={fallback} "
          f"fallback_pct={100.0*fallback/total:.2f} worst_accepted_q12={worst_accepted:.3f} "
          f"raw_data_bytes={width*height*cell_bytes}")

if __name__=="__main__":
    main()
