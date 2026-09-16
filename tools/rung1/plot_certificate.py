#!/usr/bin/env python3
"""Visual diagnostics for the DDA band structure and its temporal certificate.

Six panels, in the order they answer the question "what is this machine, how
stable is it as the player moves, and can its certificate be trusted":

  1  phase x step behaviour atlas, with the analytic p = (-c*step) mod 1024
     edges overlaid. This is the picture of the finite machine.
  2  certificate margin: how much player motion is provably safe, per motion axis
  3  predicted vs observed first raster change. The one property that must hold
     is that no point sits below the diagonal.
  4  why the raster actually changed, by motion axis
  5  Rung 2 error tails, baked field vs bearing_q12, as a complementary CDF
  6  vocabulary saturation over all 466 walkable cells
"""
import sys, csv
from collections import Counter, defaultdict
from pathlib import Path
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

SURF="#fcfcfb"; INK="#0b0b0b"; INK2="#52514e"; GRID="#e4e3df"
S1="#2a78d6"; S2="#eb6834"; S3="#1baf7a"; S4="#eda100"

d=Path(sys.argv[1]); voc=Path(sys.argv[2]); out=Path(sys.argv[3])

fig=plt.figure(figsize=(16.5,13.0),facecolor=SURF)
gs=fig.add_gridspec(3,2,hspace=0.42,wspace=0.22,top=0.90,bottom=0.05,left=0.062,right=0.975)
def style(a):
    a.set_facecolor(SURF)
    for sp in ("top","right"): a.spines[sp].set_visible(False)
    for sp in ("left","bottom"): a.spines[sp].set_color(GRID)
    a.tick_params(colors=INK2,labelsize=9)
    return a

# ---- 1  phase x step atlas ----------------------------------------------
a=style(fig.add_subplot(gs[0,0]))
rows=list(csv.reader(open(d/"atlas.csv")))[1:]
steps=sorted({int(r[0]) for r in rows})
sidx={s:i for i,s in enumerate(steps)}
img=np.full((len(steps),1024),np.nan)
ex,ey=[],[]
for st,ph,cl,ed in rows:
    st=int(st); ph=int(ph); cl=int(cl)
    if cl>=0: img[sidx[st],ph]=cl
    if ed=="1": ex.append(ph); ey.append(sidx[st])
a.imshow(img,aspect="auto",cmap="turbo",interpolation="nearest",origin="lower",
         extent=(0,1024,0,len(steps)))
a.scatter(ex,ey,s=0.35,c="#ffffff",linewidths=0,alpha=0.85)
a.set_xlabel("DDA phase  (iq + 32) mod 1024",color=INK2,fontsize=9.5)
ticks=np.linspace(0,len(steps)-1,7).astype(int)
a.set_yticks(ticks+0.5); a.set_yticklabels([str(steps[i]) for i in ticks],fontsize=8.5)
a.set_ylabel("step  (192 sampled of 3,103)",color=INK2,fontsize=9.5)
a.set_title("1  Six-column behaviour atlas, analytic edges in white",
            fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=10)
a.text(0.015,0.975,"colour = behaviour class    white = p = (-c·step) mod 1024",
       transform=a.transAxes,fontsize=8.5,color="white",va="top")

# ---- the certificate census ---------------------------------------------
MODE={0:"+X  (1/16 cell)",1:"+Y  (1/16 cell)",2:"+yaw  (1 unit)"}
cert=[r for r in csv.DictReader(open(d/"certificate.csv")) if r["path"]=="true"]
KS=32

# ---- 2  certificate margin ----------------------------------------------
a=style(fig.add_subplot(gs[0,1]))
for m,(color,lbl) in zip((0,1,2),((S1,MODE[0]),(S3,MODE[1]),(S2,MODE[2]))):
    v=np.array([min(int(r["pred_first"])-1,KS) for r in cert if int(r["mode"])==m])
    if not len(v): continue
    xs=np.arange(0,KS+1)
    cdf=np.array([(v>=k).mean()*100 for k in xs])
    a.plot(xs,cdf,color=color,lw=2,label=lbl)
a.set_xlabel("player-motion steps the certificate proves safe",color=INK2,fontsize=9.5)
a.set_ylabel("share of spans still safe (%)",color=INK2,fontsize=9.5)
a.set_title("2  How long the certificate holds",fontsize=12.5,color=INK,
            fontweight="bold",loc="left",pad=10)
a.legend(fontsize=9,frameon=False,labelcolor=INK2)
a.grid(True,color=GRID,lw=0.8); a.set_axisbelow(True); a.set_xlim(0,KS); a.set_ylim(0,100)

# ---- 3  predicted vs observed -------------------------------------------
a=style(fig.add_subplot(gs[1,0]))
H=np.zeros((KS+2,KS+2))
unsafe=0
for r in cert:
    p=min(int(r["pred_first"]),KS+1); o=min(int(r["obs_first"]),KS+1)
    H[p,o]+=1
    if p>o: unsafe+=1
a.imshow(np.log10(H+1),origin="lower",cmap="Blues",aspect="auto",
         extent=(0,KS+2,0,KS+2))
a.plot([0,KS+2],[0,KS+2],color=INK2,lw=1.4,ls="--")
# Sound requires predicted <= observed, so the forbidden region is ABOVE the
# diagonal. The first version of this panel shaded and labelled the region below
# it, which is the safe, conservative side.
a.fill_between([0,KS+2],[0,KS+2],[KS+2,KS+2],color=S2,alpha=0.10)
a.text(KS*0.28,KS*0.80,f"UNSAFE region\ncertificate outlives the change\n{unsafe} points",
       fontsize=9.5,color=S2 if unsafe else INK2,ha="center",fontweight="bold")
a.text(KS*0.72,KS*0.22,"conservative\n(wakes early)",fontsize=9,color=INK2,ha="center")
a.set_xlabel("observed first raster change (motion steps)",color=INK2,fontsize=9.5)
a.set_ylabel("certificate-predicted first change",color=INK2,fontsize=9.5)
a.set_title("3  Predicted vs observed — nothing may rise above the diagonal",
            fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=10)

# ---- 4  cause breakdown --------------------------------------------------
a=style(fig.add_subplot(gs[1,1]))
CAUSES=["phase band","step","endpoint column","left frame","UNATTRIBUTED"]
COLS=[S1,S3,S4,INK2,S2]
per={m:Counter() for m in MODE}
for r in cert:
    c=r["cause"]
    if c.startswith("no change"): continue
    per[int(r["mode"])][c]+=1
y=np.arange(len(MODE)); left=np.zeros(len(MODE))
for ci,cname in enumerate(CAUSES):
    vals=np.array([100.0*per[m][cname]/max(sum(per[m].values()),1) for m in MODE])
    a.barh(y,vals,left=left,height=0.6,color=COLS[ci],label=cname,edgecolor=SURF,linewidth=2)
    for i in range(len(MODE)):
        if vals[i]>=6: a.text(left[i]+vals[i]/2,y[i],f"{vals[i]:.0f}",ha="center",va="center",
                              fontsize=8.5,color="white",fontweight="bold")
    left+=vals
a.set_yticks(y); a.set_yticklabels([MODE[m] for m in MODE],fontsize=9.5,color=INK)
a.invert_yaxis(); a.set_xlim(0,100)
a.set_xlabel("share of spans whose raster changed (%)   — denominator: spans that changed,\nnot all spans, so these differ from the run's table",color=INK2,fontsize=9)
a.set_title("4  Why the raster actually changed",fontsize=12.5,color=INK,
            fontweight="bold",loc="left",pad=10)
a.legend(fontsize=8.5,frameon=False,ncol=3,loc="lower center",
         bbox_to_anchor=(0.5,-0.30),labelcolor=INK2)
a.xaxis.grid(True,color=GRID,lw=0.8); a.set_axisbelow(True)

# ---- 5  Rung 2 error tails ----------------------------------------------
a=style(fig.add_subplot(gs[2,0]))
txt=(d.parent/"rung2-cross-cell.txt")
if txt.exists():
    import re
    tbl={}
    cur=None
    for line in txt.read_text().splitlines():
        s=line.strip()
        if s in ("baked field","bearing_q12"): cur=s
        m=re.match(r"(interior|leaf boundary|CELL BOUNDARY)\s+([\d.]+)%\s+(\d+)\s+(\d+)",s)
        if m and cur: tbl[(cur,m.group(1))]=(float(m.group(2)),int(m.group(3)),int(m.group(4)))
    xs=np.arange(3); w=0.35
    classes=["interior","leaf boundary","CELL BOUNDARY"]
    for k,(src,color) in enumerate((("baked field",S1),("bearing_q12",S2))):
        vals=[tbl.get((src,c),(0,0,0))[2] for c in classes]
        b=a.bar(xs+(k-0.5)*w,vals,width=w*0.9,color=color,label=src)
        a.bar_label(b,fmt="%.0f px",fontsize=8.5,color=INK2,padding=2)
    a.set_xticks(xs); a.set_xticklabels(classes,fontsize=9.5,color=INK)
    a.set_ylabel("largest endpoint jump (screen px)",color=INK2,fontsize=9.5)
    a.set_title("5  Rung 2 worst tail — the part incidence hides",
                fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=10)
    a.legend(fontsize=9,frameon=False,labelcolor=INK2)
    a.yaxis.grid(True,color=GRID,lw=0.8); a.set_axisbelow(True)

# ---- 6  vocabulary saturation -------------------------------------------
a=style(fig.add_subplot(gs[2,1]))
g=list(csv.DictReader(open(voc/"growth.csv")))
xs=[int(r["instances"])/1e9 for r in g]
ys=[int(r["trajectories"]) for r in g]
a.plot(xs,ys,color=S1,lw=2)
a.axhline(432,color=S3,lw=1.2,ls="--")
a.text(xs[-1]*0.55,432-26,"432, reached at the 10th walkable cell",
       fontsize=9.5,color=S3,fontweight="bold")
a.set_xlabel("chunk-family instances swept (billions)",color=INK2,fontsize=9.5)
a.set_ylabel("distinct raster trajectories",color=INK2,fontsize=9.5)
a.set_title("6  Exhaustive saturation, all 466 walkable cells",
            fontsize=12.5,color=INK,fontweight="bold",loc="left",pad=10)
a.set_ylim(0,500); a.grid(True,color=GRID,lw=0.8); a.set_axisbelow(True)

fig.suptitle("The DDA band machine and its temporal certificate",
             fontsize=16,color=INK,fontweight="bold",x=0.012,ha="left",y=0.975)
fig.text(0.012,0.938,
         "Six-column raster behaviour is piecewise constant in DDA phase, with eight bands whose edges "
         "have a closed form. The distance to the nearest edge is a provable safe region for player motion.",
         fontsize=10.5,color=INK2,ha="left")
fig.savefig(out,dpi=135,facecolor=SURF)
print(f"wrote {out}")
