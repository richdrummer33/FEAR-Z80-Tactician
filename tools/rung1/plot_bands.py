#!/usr/bin/env python3
"""Annotated phase-band strips: what the DDA machine means geometrically.

For a handful of representative steps, draw the phase line 0..1023 divided into
its behaviour bands, mark the eight analytic edges p = (-c*step) mod 1024, and
draw the actual six-column raster each band emits. One of these does more than a
page of statistics: it shows that the machine is a small number of contiguous
regions, and that the distance to the next edge is a literal distance along a
line the player's motion walks.
"""
import sys
from pathlib import Path
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

SURF="#fcfcfb"; INK="#0b0b0b"; INK2="#52514e"; GRID="#e4e3df"
BAND=["#2a78d6","#eb6834","#1baf7a","#eda100","#7b5cd6","#d63a6a","#2fa8b8","#8a8a86"]
BIAS=65536

def col_out(phase,step,c,fam=0):
    a0=BIAS+phase+c*step; a1=a0+step; a2=a1+step
    h0,h1,h2=a0>>7,a1>>7,a2>>7
    y0,y1,y2=(72+h0,72+h1,72+h2) if fam else (71-h0,71-h1,71-h2)
    lo0,hi0=min(y0,y1)>>3,max(y0,y1)>>3
    lo1=min(y1,y2)>>3
    nd,jump=hi0-lo0,lo1-hi0
    if nd<0 or nd>30 or jump>0 or jump<-4: return None
    return (nd,jump)

def sig(phase,step,n=6):
    o=[col_out(phase,step,c) for c in range(n)]
    return None if any(x is None for x in o) else tuple(o)

STEPS=[int(s) for s in (sys.argv[2].split(",") if len(sys.argv)>2 else
                        ["61","143","287","410","631","811"])]
out=Path(sys.argv[1])

fig,axes=plt.subplots(len(STEPS),1,figsize=(15.0,1.95*len(STEPS)),facecolor=SURF)
if len(STEPS)==1: axes=[axes]
for ax,step in zip(axes,STEPS):
    ax.set_facecolor(SURF)
    for sp in ax.spines.values(): sp.set_visible(False)
    ax.set_yticks([]); ax.set_xlim(0,1024); ax.set_ylim(0,1)
    ax.tick_params(colors=INK2,labelsize=8.5)
    sigs=[sig(p,step) for p in range(1024)]
    ids={}; seq=[]
    for s in sigs:
        if s is None: seq.append(-1); continue
        if s not in ids: ids[s]=len(ids)
        seq.append(ids[s])
    # contiguous bands
    start=0
    for p in range(1,1025):
        if p==1024 or seq[p]!=seq[start]:
            cid=seq[start]
            col = GRID if cid<0 else BAND[cid%len(BAND)]
            ax.axvspan(start,p,ymin=0.30,ymax=0.86,color=col,alpha=0.85,lw=0)
            if p-start>=52 and cid>=0:
                s=sigs[start]
                txt="".join(("v"*nd)+("→" if j==0 else "↗"*(-j)) for nd,j in s[:4])
                ax.text((start+p)/2,0.58,f"{cid}",ha="center",va="center",
                        fontsize=9.5,color="white",fontweight="bold")
                ax.text((start+p)/2,0.40,txt[:14],ha="center",va="center",
                        fontsize=7.5,color="white")
            start=p
    for c in range(8):
        e=(-c*step)%1024
        ax.plot([e,e],[0.30,0.93],color=INK,lw=1.1)
        ax.text(e,0.965,f"c{c}",ha="center",va="bottom",fontsize=7,color=INK2)
    # a worked example: a phase sitting in the widest band
    widths={}; start=0
    for p in range(1,1025):
        if p==1024 or seq[p]!=seq[start]:
            widths[(start,p)]=p-start; start=p
    (b0,b1),w=max(widths.items(),key=lambda kv:kv[1])
    cur=(b0+b1)//2
    ax.plot([cur],[0.19],marker="v",ms=9,color=INK)
    ax.annotate("",xy=(b0,0.14),xytext=(b1-1,0.14),
                arrowprops=dict(arrowstyle="<->",color=INK,lw=1.2))
    ax.text((b0+b1)/2,0.03,f"phase {cur}: safe for {min(cur-b0,b1-1-cur)} phase units "
                           f"either way (band width {w})",
            ha="center",fontsize=8.5,color=INK)
    nb=len({s for s in seq if s>=0})
    ax.set_title(f"step = {step}    {nb} behaviours in {len([1 for p in range(1,1024) if seq[p]!=seq[p-1]])+1} contiguous bands",
                 fontsize=10.5,color=INK,fontweight="bold",loc="left",pad=6)
axes[-1].set_xlabel("DDA phase  (iq + 32) mod 1024",color=INK2,fontsize=9.5)
fig.suptitle("Phase-band strips: the eight analytic edges and what each band emits",
             fontsize=14,color=INK,fontweight="bold",x=0.012,ha="left",y=0.995)
fig.tight_layout(rect=(0,0,1,0.965))
fig.savefig(out,dpi=140,facecolor=SURF)
print(f"wrote {out}")
