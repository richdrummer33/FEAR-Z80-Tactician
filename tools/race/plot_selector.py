#!/usr/bin/env python3
"""Structural plots for the selector census.

Panel 1 is the one the interval-selector hypothesis lives or dies on: whether
"about eight bands per step" describes the population or only its average.
Panel 3 is the structural map -- the exact body identity over every step and
phase -- which is where cross-step regularity would show up if it exists.
"""
import sys, csv
from pathlib import Path
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

SURF="#fcfcfb"; INK="#0b0b0b"; INK2="#52514e"; GRID="#e4e3df"
S1="#2a78d6"; S2="#eb6834"; S3="#1baf7a"; S4="#eda100"

d=Path(sys.argv[1]); out=Path(sys.argv[2])
bands=list(csv.DictReader(open(d/"bands_per_step.csv")))
occ=list(csv.DictReader(open(d/"body_occupancy.csv")))
nb=np.array([int(r["bands"]) for r in bands])
sv=np.array([int(r["step"]) for r in bands])
nstep=len(nb)
bm=np.fromfile(d/"body_map.u8",dtype=np.uint8).reshape(nstep,1024)

fig=plt.figure(figsize=(16.5,12.5),facecolor=SURF)
gs=fig.add_gridspec(3,2,hspace=0.40,wspace=0.20,top=0.90,bottom=0.055,left=0.065,right=0.975)
def style(a):
    a.set_facecolor(SURF)
    for s in ("top","right"): a.spines[s].set_visible(False)
    for s in ("left","bottom"): a.spines[s].set_color(GRID)
    a.tick_params(colors=INK2,labelsize=9); return a

# 1 band-count distribution
a=style(fig.add_subplot(gs[0,0]))
vals,counts=np.unique(nb,return_counts=True)
b=a.bar(vals,counts,color=S1,width=0.6)
a.bar_label(b,fmt="%d",fontsize=9,color=INK2,padding=2)
a.set_yscale("log")
a.set_xlabel("merged contiguous phase bands for one exact step",color=INK2,fontsize=9.5)
a.set_ylabel("number of exact steps (log)",color=INK2,fontsize=9.5)
a.set_title("1  Band count is a spike at eight, not an average",
            fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=10)
a.text(0.03,0.92,f"mean {nb.mean():.2f}   median {np.median(nb):.0f}\n"
                 f"p95 {np.percentile(nb,95):.0f}   p99 {np.percentile(nb,99):.0f}   max {nb.max()}",
       transform=a.transAxes,fontsize=10,color=INK,va="top",fontweight="bold")
a.set_xticks(vals)

# 2 bands vs step value
a=style(fig.add_subplot(gs[0,1]))
o=np.argsort(sv)
a.plot(sv[o],nb[o],color=S1,lw=0.7)
a.scatter(sv[nb<8],nb[nb<8],s=26,color=S2,zorder=3,label="fewer than eight bands")
a.set_xlabel("exact step value (signed)",color=INK2,fontsize=9.5)
a.set_ylabel("merged bands",color=INK2,fontsize=9.5)
a.set_title("2  Where the degenerate steps are",
            fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=10)
a.legend(fontsize=9,frameon=False,labelcolor=INK2)
a.grid(True,color=GRID,lw=0.8); a.set_axisbelow(True)

# 3 the structural map
a=style(fig.add_subplot(gs[1,:]))
rng=np.random.default_rng(7)
perm=rng.permutation(256)          # decorrelate ID magnitude from colour
img=perm[bm]
a.imshow(img,aspect="auto",cmap="nipy_spectral",interpolation="nearest",origin="lower",
         extent=(0,1024,0,nstep))
a.set_xlabel("DDA phase",color=INK2,fontsize=9.5)
a.set_ylabel("step ordinal",color=INK2,fontsize=9.5)
a.set_title("3  Six-column body identity over exact step and phase  "
            "(colour is a shuffled category, not a magnitude)",
            fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=10)

# 4 crop
a=style(fig.add_subplot(gs[2,0]))
lo=nstep//2
a.imshow(perm[bm[lo:lo+120,:]],aspect="auto",cmap="nipy_spectral",interpolation="nearest",
         origin="lower",extent=(0,1024,lo,lo+120))
a.set_xlabel("DDA phase",color=INK2,fontsize=9.5)
a.set_ylabel("step ordinal",color=INK2,fontsize=9.5)
a.set_title("4  Crop: 120 consecutive steps",fontsize=12.5,color=INK,
            fontweight="bold",loc="left",pad=10)

# 5 body occupancy
a=style(fig.add_subplot(gs[2,1]))
st=np.array(sorted((int(r["states"]) for r in occ),reverse=True),dtype=float)
a.plot(np.arange(1,len(st)+1),100*np.cumsum(st)/st.sum(),color=S1,lw=2)
for k,c in ((10,S3),(50,S4),(100,S2)):
    if k<=len(st):
        a.axvline(k,color=c,lw=1,ls="--")
        a.text(k,100*np.cumsum(st)[k-1]/st.sum()-6,f"top {k}: {100*np.cumsum(st)[k-1]/st.sum():.0f}%",
               fontsize=8.5,color=c,ha="center")
a.set_xlabel("bodies, ranked by how many states map to them",color=INK2,fontsize=9.5)
a.set_ylabel("cumulative share of states (%)",color=INK2,fontsize=9.5)
a.set_title("5  Occupancy is flat: no small hot subset",fontsize=12.5,color=INK,
            fontweight="bold",loc="left",pad=10)
a.grid(True,color=GRID,lw=0.8); a.set_axisbelow(True); a.set_ylim(0,100)

fig.suptitle("Selector census: the exact-step interval hypothesis",
             fontsize=15,color=INK,fontweight="bold",x=0.012,ha="left",y=0.975)
fig.text(0.012,0.938,
         "3,103 reachable steps x 1,024 phases = 3,177,472 states onto 175 six-column bodies. "
         "These are the six-column body vocabulary only, not the whole rasteriser.",
         fontsize=10,color=INK2,ha="left")
fig.savefig(out,dpi=130,facecolor=SURF)
print(f"wrote {out}")
