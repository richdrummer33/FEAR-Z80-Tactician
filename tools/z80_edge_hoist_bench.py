"""HOIST_A: test r1 >= r0 BEFORE computing the slope.

The conventional competitor to the persistent-walker architecture.  A39's
census found that 20.1% of draw_edge calls emit ZERO rows, and the shipped
order pays the full signed slope clamp - two biased 16-bit compares - before
discovering that.  The early-out needs only the row pair, so the two blocks
swap.  Exact by construction: neither block reads the other's output.

A/B against EDGELUT3 over the pose oracle, one change, nothing else touched.
"""
import sys, statistics as stt, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from z80core import assemble
import z80_materialize_masked_bench as mb, z80_materialize_dda_bench as dda
import z80_edge_lut_bench as el

def build_hoist(src):
    """HOIST_A: test r1 >= r0 BEFORE computing the slope.

    A39's census: 20.1% of draw_edge calls emit ZERO rows, and the current
    order pays the full signed slope clamp (two 16-bit biased compares) before
    discovering that. The early-out needs only the row pair, so the two blocks
    swap. Exact by construction - neither block reads the other's output."""
    i = src.index("draw_edge:\n")
    j = src.index("de_rows2:\n", i)
    k = src.index("de_r1ok:\n", j)
    e = src.index("        ret c                        ; r1 < r0 -> nothing\n", k)
    e += len("        ret c                        ; r1 < r0 -> nothing\n")
    slope_blk = src[i + len("draw_edge:\n"):j]      # the slope clamp
    rows_blk  = src[j:e]                            # row select + early out
    return src[:i] + "draw_edge:\n" + rows_blk + slope_blk + src[e:]

def main():
    limit = 300
    lines=[l.split() for l in open(ROOT / 'build' / 'coverage_pose_oracle.txt') if l.strip()]
    step=max(1,len(lines)//limit)
    cases=[]
    for f in lines[::step]:
        n=int(f[0]); runs=[tuple(int(v) for v in f[1+8*k:9+8*k]) for k in range(n)]
        cases.append((runs,[int(v) for v in f[1+8*n:]]))
    low,high=mb.tables(); bgm=mb.background_map(); rows=el.load_table()
    base = el.build_lut_variant(dda.SRC_DDA,3)
    res={}
    for name,src in (("EDGELUT3",base),("HOIST_A",build_hoist(base))):
        code,_=assemble(src,0)
        img=bytearray(0x10000); img[0:len(code)]=code
        img[mb.LOWTAB:mb.LOWTAB+len(low)]=low; img[mb.HIGHTAB:mb.HIGHTAB+len(high)]=high
        for r,vals in rows.items():
            for i,v in enumerate(vals):
                img[el.LUT+64*r+2*i]=v&0xFF; img[el.LUT+64*r+2*i+1]=(v>>8)&0xFF
        for r in range(30):
            a=el.LUT+64*r; img[0xB800+2*r]=a&0xFF; img[0xB800+2*r+1]=a>>8
        fails=0; ts=[]
        for runs,want in cases:
            t,got=mb.run_pose(img,runs,False,bgm); ts.append(t)
            if got!=want: fails+=1
        res[name]=(stt.mean(ts),len(code),fails)
        print(f"{name:10} {stt.mean(ts):10,.1f} T/pose  {len(code):5d} bytes  "
              f"{'EXACT' if not fails else str(fails)+'/'+str(len(cases))+' WRONG'}")
    a=res['EDGELUT3'][0]; b=res['HOIST_A'][0]
    print(f"\n  HOIST_A vs EDGELUT3: {100*b/a-100:+.1f}%   "
          f"saves {a-b:,.0f} T/pose")
    print(f"  whole update 223,266 -> {223266-(a-b):,.0f} T")
    print(f"  sequence baseline 138,609 -> {138609*b/a:,.0f} T")


if __name__ == "__main__":
    main()
