#!/usr/bin/env python3
"""Safe-region slices: is the certificate region a box, or something slanted?

Per-axis margins describe an axis-aligned rectangle. The composability
measurements said that rectangle is wrong -- moves inside both one-dimensional
margins break the columns 5.35% (X,Y) and 11.06% (X,yaw) of the time. These
slices show why: the real region is bounded by lines that cut across the box,
which is what an affine bearing field inside a leaf would produce.
"""
import sys, csv
from collections import defaultdict
from pathlib import Path
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import ListedColormap
from matplotlib.patches import Rectangle

SURF="#fcfcfb"; INK="#0b0b0b"; INK2="#52514e"; GRID="#e4e3df"
S1="#2a78d6"; S2="#eb6834"; S3="#1baf7a"

src=Path(sys.argv[1])/"safe_slices.csv"; out=Path(sys.argv[2])
data=defaultdict(dict)
for r in csv.DictReader(open(src)):
    data[(r["slice"],int(r["run"]))][(int(r["a"]),int(r["b"]))]=int(r["same"])

runs=sorted({k[1] for k in data})[:3]
fig,axes=plt.subplots(2,len(runs),figsize=(4.6*len(runs),9.2),facecolor=SURF)
cmap=ListedColormap(["#f3dcd2", S3])
for col,run in enumerate(runs):
    for row,(sl,xl,yl) in enumerate((("xy","dx  (1/16 cell)","dy  (1/16 cell)"),
                                     ("xyaw","dx  (1/16 cell)","dyaw  (units)"))):
        a=axes[row][col] if len(runs)>1 else axes[row]
        a.set_facecolor(SURF)
        g=data[(sl,run)]
        if not g: a.axis("off"); continue
        A=sorted({k[0] for k in g}); B=sorted({k[1] for k in g})
        M=np.zeros((len(B),len(A)))
        for i,bb in enumerate(B):
            for j,aa in enumerate(A): M[i,j]=g.get((aa,bb),0)
        a.imshow(M,origin="lower",cmap=cmap,aspect="auto",vmin=0,vmax=1,
                 extent=(A[0]-0.5,A[-1]+0.5,B[0]-0.5,B[-1]+0.5))
        # the axis-aligned box the independent margins would predict
        mx=0
        while (mx+1,0) in g and g[(mx+1,0)]: mx+=1
        mxn=0
        while (-(mxn+1),0) in g and g[(-(mxn+1),0)]: mxn+=1
        my=0
        while (0,my+1) in g and g[(0,my+1)]: my+=1
        myn=0
        while (0,-(myn+1)) in g and g[(0,-(myn+1))]: myn+=1
        a.add_patch(Rectangle((-mxn-0.5,-myn-0.5),mx+mxn+1,my+myn+1,
                              fill=False,edgecolor=S2,lw=2,ls="--"))
        a.plot([0],[0],marker="o",ms=6,color=INK)
        for sp in ("top","right"): a.spines[sp].set_visible(False)
        for sp in ("left","bottom"): a.spines[sp].set_color(GRID)
        a.tick_params(colors=INK2,labelsize=8.5)
        a.set_xlabel(xl,color=INK2,fontsize=9)
        if col==0: a.set_ylabel(yl,color=INK2,fontsize=9)
        a.set_title(f"span {run}   {'dx vs dy' if sl=='xy' else 'dx vs dyaw'}",
                    fontsize=10.5,color=INK,fontweight="bold",loc="left",pad=6)
fig.suptitle("Safe regions are not the box the per-axis margins predict",
             fontsize=15,color=INK,fontweight="bold",x=0.012,ha="left",y=0.985)
fig.text(0.012,0.952,
         "Green = the projected columns are unchanged at that destination pose. "
         "The orange dashed box is what conjoining the independent one-dimensional margins would claim. "
         "Where green does not fill it, that claim is unsound.",
         fontsize=9.5,color=INK2,ha="left")
fig.tight_layout(rect=(0,0,1,0.935))
fig.savefig(out,dpi=140,facecolor=SURF)
print(f"wrote {out}")
