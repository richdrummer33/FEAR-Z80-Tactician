#!/usr/bin/env python3
"""Adjudication dashboard: how each (iq,step) derivation stands against
high-precision perspective geometry.

Four panels, in the order they answer the architectural question:
  5  agreement classes per path, as a 100% stacked bar (exact / 1px / 2px / worse)
  6  distance of every disputed tile boundary from the true edge, as a CDF --
     this is what separates "coin flip" from "wrong"
  7  temporal behaviour: reversal rate under smooth motion, per motion type,
     with the continuous reference's own rate as the floor
  8  the move state-transition matrix of the canonical trajectory vocabulary
"""
import sys, csv
from collections import Counter
from pathlib import Path
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

SURF="#fcfcfb"; INK="#0b0b0b"; INK2="#52514e"; GRID="#e4e3df"
S1="#2a78d6"; S2="#eb6834"; S3="#1baf7a"; S4="#eda100"

adj=Path(sys.argv[1]); voc=Path(sys.argv[2]); out=Path(sys.argv[3])
report=Path(sys.argv[4]) if len(sys.argv)>4 else None

agree=list(csv.DictReader(open(adj/"agree_classes.csv")))
own=list(csv.DictReader(open(adj/"ownership.csv")))
bd=list(csv.DictReader(open(adj/"boundary_dist.csv")))

fig,ax=plt.subplots(2,2,figsize=(15.5,10.5),facecolor=SURF)
for a in ax.ravel():
    a.set_facecolor(SURF)
    for sp in ("top","right"): a.spines[sp].set_visible(False)
    for sp in ("left","bottom"): a.spines[sp].set_color(GRID)
    a.tick_params(colors=INK2,labelsize=9)

# ---- 5: agreement classes ------------------------------------------------
a=ax[0,0]
rows=[(f"{r['path'].upper()} {r['edge']}",
       [int(r["exact"]),int(r["px1"]),int(r["px2"]),int(r["worse"])]) for r in agree]
labels=[r[0] for r in rows]
mat=np.array([r[1] for r in rows],dtype=float)
pct=100.0*mat/mat.sum(axis=1,keepdims=True)
cols=[S3,S1,S4,S2]
names=["exact (same pixel row)","within 1 px","within 2 px","worse than 2 px"]
left=np.zeros(len(rows))
y=np.arange(len(rows))
for k in range(4):
    a.barh(y,pct[:,k],left=left,height=0.6,color=cols[k],
           label=names[k],edgecolor=SURF,linewidth=2)
    for i in range(len(rows)):
        if pct[i,k]>=4.0:
            a.text(left[i]+pct[i,k]/2,y[i],f"{pct[i,k]:.1f}",ha="center",va="center",
                   fontsize=8.5,color="white",fontweight="bold")
    left+=pct[:,k]
a.set_yticks(y); a.set_yticklabels(labels,fontsize=9.5,color=INK)
a.invert_yaxis(); a.set_xlim(0,100); a.set_xlabel("share of scored columns (%)",color=INK2,fontsize=9.5)
a.set_title("5  Screen-Y agreement with high-precision geometry",
            fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=12)
a.legend(fontsize=8.5,frameon=False,ncol=2,loc="lower center",bbox_to_anchor=(0.5,-0.26),
         labelcolor=INK2)
a.xaxis.grid(True,color=GRID,lw=0.8); a.set_axisbelow(True)

# ---- 6: boundary-distance CDF -------------------------------------------
a=ax[0,1]
for path,color,lbl in (("rom",S1,"ROM screen_depth_plane"),
                       ("host",S2,"host inv0/inv1 interpolation")):
    pts=sorted((float(r["dist_px"]),int(r["count"])) for r in bd if r["path"]==path)
    if not pts: continue
    xs=np.array([p[0] for p in pts]); ws=np.array([p[1] for p in pts],dtype=float)
    a.plot(xs,100.0*np.cumsum(ws)/ws.sum(),color=color,lw=2,label=lbl)
a.axvline(1.0,color=INK2,lw=1.2,ls="--")
a.text(1.12,4,"1 px: below this the row\nassignment is a coin flip",
       fontsize=8.5,color=INK2,va="bottom")
a.set_xlabel("distance of the true edge from the disputed tile line (px)",color=INK2,fontsize=9.5)
a.set_ylabel("cumulative share of disputed cells (%)",color=INK2,fontsize=9.5)
a.set_xlim(0,4.2); a.set_ylim(0,100)
a.set_title("6  How close the truth sat to the line it lost",
            fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=12)
a.legend(fontsize=9,frameon=False,loc="lower right",labelcolor=INK2)
a.grid(True,color=GRID,lw=0.8); a.set_axisbelow(True)
o={r["path"]:r for r in own}
txt=[]
for p,lbl in (("rom","ROM"),("host","host")):
    r=o[p]; d=int(r["boundary_adjacent"])+int(r["one_cell_away"])+int(r["interior"])
    txt.append(f"{lbl}: {100*int(r['interior'])/d:.2f}% of disputes are 2+ rows off")
a.text(0.30,0.36,"\n".join(txt),transform=a.transAxes,fontsize=9.5,color=INK,va="top")

# ---- 7: temporal jitter --------------------------------------------------
a=ax[1,0]
jit=list(csv.DictReader(open(adj/"jitter.csv")))
MODES={0:"strafe",1:"walk forward",2:"turn"}
seqs={}
for r in jit:
    seqs.setdefault((int(r["seq"]),int(r["mode"])),[]).append(r)
def revs(v):
    d0=0; n=0
    for i in range(1,len(v)):
        d=v[i]-v[i-1]
        if d==0: continue
        if d0 and (d>0)!=(d0>0): n+=1
        d0=d
    return n
stats={m:{"ref":0,"rom":0,"host":0,"n":0} for m in MODES}
for (sq,m),rs in seqs.items():
    rs.sort(key=lambda r:int(r["step"]))
    stats[m]["ref"]+=revs([int(r["y_true_int"]) for r in rs])
    stats[m]["rom"]+=revs([int(r["y_rom"]) for r in rs])
    stats[m]["host"]+=revs([int(r["y_host"]) for r in rs])
    stats[m]["n"]+=len(rs)
x=np.arange(len(MODES)); w=0.26
for k,(key,color,lbl) in enumerate((("ref",S3,"continuous reference, rounded"),
                                    ("rom",S1,"ROM screen_depth_plane"),
                                    ("host",S2,"host inv0/inv1 interpolation"))):
    v=[100.0*stats[m][key]/max(stats[m]["n"],1) for m in MODES]
    b=a.bar(x+(k-1)*w,v,width=w*0.9,color=color,label=lbl)
    a.bar_label(b,fmt="%.2f",fontsize=8,color=INK2,padding=2)
a.set_xticks(x); a.set_xticklabels([MODES[m] for m in MODES],fontsize=10,color=INK)
a.set_ylabel("direction reversals per 100 samples",color=INK2,fontsize=9.5)
a.set_title("7  Edge jitter under smooth motion (one input unit per step)",
            fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=12)
a.legend(fontsize=8.5,frameon=False,loc="upper left",labelcolor=INK2)
a.yaxis.grid(True,color=GRID,lw=0.8); a.set_axisbelow(True)

# ---- 8: move state-transition matrix ------------------------------------
a=ax[1,1]
LABEL=["down 1","next col","up 1","up 2","up 3","up 4","END","wrap"]
tm=voc/"traj_moves.csv"
if tm.exists():
    N=len(LABEL)
    M=np.zeros((N,N))
    start=np.zeros(N)
    for r in csv.DictReader(open(tm)):
        sl=[int(v) for v in r["slots"].split() if v!="-1"]
        c=int(r["count"])
        if not sl: continue
        start[sl[0]]+=c
        for i in range(len(sl)-1): M[sl[i],sl[i+1]]+=c
    keep=[i for i in range(N) if M[i].sum()+M[:,i].sum()+start[i]>0]
    Mk=M[np.ix_(keep,keep)]
    rs=Mk.sum(axis=1,keepdims=True); rs[rs==0]=1
    P=100.0*Mk/rs
    im=a.imshow(P,cmap="Blues",vmin=0,vmax=100)
    a.set_xticks(range(len(keep))); a.set_yticks(range(len(keep)))
    a.set_xticklabels([LABEL[i] for i in keep],fontsize=9,color=INK,rotation=30,ha="right")
    a.set_yticklabels([LABEL[i] for i in keep],fontsize=9,color=INK)
    for i in range(len(keep)):
        for j in range(len(keep)):
            if P[i,j]>=0.05:
                a.text(j,i,f"{P[i,j]:.1f}",ha="center",va="center",fontsize=8.5,
                       color="white" if P[i,j]>55 else INK)
    outdeg=[int((Mk[i]>0).sum()) for i in range(len(keep))]
    a.set_xlabel("next move    (out-degree: "+", ".join(
        f"{LABEL[keep[i]]}={outdeg[i]}" for i in range(len(keep)))+")",
        color=INK2,fontsize=8.5)
    a.set_ylabel("current move",color=INK2,fontsize=9.5)
    a.set_title("8  Move transitions, weighted by chunk instances (row %)",
                fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=12)
    for sp in a.spines.values(): sp.set_visible(False)
    a.tick_params(length=0)
else:
    a.text(0.5,0.5,"traj_moves.csv not present",ha="center",va="center",
           color=INK2,transform=a.transAxes); a.axis("off")

fig.suptitle("Depth-plane adjudication against high-precision perspective geometry",
             fontsize=15,color=INK,fontweight="bold",x=0.012,ha="left",y=0.985)
fig.text(0.012,0.955,
         "Both derivations scored on identical columns, under the renderer's own 10/127-cell depth clip. "
         "The reference is closed-form geometry, not a second renderer.",
         fontsize=10,color=INK2,ha="left")
fig.tight_layout(rect=(0,0,1,0.94))
fig.savefig(out,dpi=140,facecolor=SURF)
print(f"wrote {out}")
