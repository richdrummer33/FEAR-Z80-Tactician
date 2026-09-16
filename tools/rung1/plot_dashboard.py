#!/usr/bin/env python3
"""Architecture evidence dashboard for the finite-raster vocabulary result."""
import sys, csv
from pathlib import Path
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

SURF="#fcfcfb"; INK="#0b0b0b"; INK2="#52514e"; GRID="#e4e3df"
S1="#2a78d6"; S2="#eb6834"; S3="#1baf7a"; S4="#eda100"

d=Path(sys.argv[1]); out=Path(sys.argv[2])
growth=list(csv.DictReader(open(d/"growth.csv")))
traj=list(csv.DictReader(open(d/"trajectories.csv")))
moves=list(csv.DictReader(open(d/"moves.csv")))

gi=[int(r["instances"])/1e6 for r in growth]
gt=[int(r["trajectories"]) for r in growth]
counts=sorted((int(r["count"]) for r in traj), reverse=True)
tot=sum(counts)
cum=[]; s=0
for c in counts: s+=c; cum.append(100.0*s/tot)

mv=[r for r in moves if int(r["occurrences"])>0]
LABEL={"39":"down 1 row","1":"next col","-39":"next col, up 1","-79":"next col, up 2",
       "-119":"next col, up 3","-159":"next col, up 4","0":"run terminator"}
mlab=[LABEL.get(r["move"],r["move"]) for r in mv]
mocc=[int(r["occurrences"])/1e6 for r in mv]
mtr =[int(r["trajectories_containing"]) for r in mv]

fig=plt.figure(figsize=(13,14),facecolor=SURF)
gs=fig.add_gridspec(4,2,height_ratios=[1.5,.85,1.1,1.1],hspace=.52,wspace=.26)

def style(ax,title,xl,yl):
    ax.set_facecolor(SURF)
    ax.set_title(title,color=INK,fontsize=12,fontweight="bold",loc="left",pad=10)
    ax.set_xlabel(xl,color=INK2,fontsize=9); ax.set_ylabel(yl,color=INK2,fontsize=9)
    ax.tick_params(colors=INK2,labelsize=8)
    for sp in ("top","right"): ax.spines[sp].set_visible(False)
    for sp in ("left","bottom"): ax.spines[sp].set_color(GRID)
    ax.grid(True,color=GRID,lw=.8,alpha=.9); ax.set_axisbelow(True)

ax=fig.add_subplot(gs[0,:]); style(ax,"1. Vocabulary saturation — does the trajectory set terminate?",
    "cumulative chunk instances generated (millions)","distinct raster trajectories")
ax.plot(gi,gt,color=S1,lw=2,marker="o",ms=7,zorder=3)
ax.annotate(f"{gt[-1]:,} trajectories\nfrom {gi[-1]:.1f}M instances",
    xy=(gi[-1],gt[-1]),xytext=(-14,-46),textcoords="offset points",
    color=INK,fontsize=10,fontweight="bold",ha="right")
ax.set_ylim(0,max(gt)*1.25)

ax=fig.add_subplot(gs[1,:]); style(ax,"   new trajectories discovered per checkpoint — flat means finite",
    "cumulative chunk instances generated (millions)","newly discovered")
new=[gt[0]]+[gt[i]-gt[i-1] for i in range(1,len(gt))]
ax.bar(gi,new,width=max(gi)/max(len(gi)*2.2,8),color=S3,zorder=3)
ax.set_ylim(0,max(max(new)*1.3,1))

ax=fig.add_subplot(gs[2,0]); style(ax,"2. Trajectory frequency by rank","rank (most → least common)","occurrences (log)")
ax.plot(range(1,len(counts)+1),counts,color=S1,lw=2); ax.set_yscale("log")

ax=fig.add_subplot(gs[2,1]); style(ax,"   cumulative share of all instances","trajectories included (rank order)","% of instances")
ax.plot(range(1,len(cum)+1),cum,color=S2,lw=2)
for n in (10,50):
    if n<=len(cum):
        ax.plot([n],[cum[n-1]],marker="o",ms=7,color=S2,zorder=4)
        ax.annotate(f"top {n} → {cum[n-1]:.1f}%",xy=(n,cum[n-1]),xytext=(12,-6),
                    textcoords="offset points",color=INK,fontsize=9)
ax.set_ylim(0,104)

ax=fig.add_subplot(gs[3,0]); style(ax,"3. Move alphabet — occurrences","","millions of occurrences")
ax.barh(range(len(mlab)),mocc,color=S1,height=.62,zorder=3)
ax.set_yticks(range(len(mlab))); ax.set_yticklabels(mlab,fontsize=9); ax.invert_yaxis()

ax=fig.add_subplot(gs[3,1]); style(ax,"   move alphabet — trajectories containing it","","distinct trajectories")
ax.barh(range(len(mlab)),mtr,color=S4,height=.62,zorder=3)
ax.set_yticks(range(len(mlab))); ax.set_yticklabels(mlab,fontsize=9); ax.invert_yaxis()

fig.suptitle("Finite-raster vocabulary: arbitrary pose → ROM projection → canonical trajectory",
             color=INK,fontsize=15,fontweight="bold",x=.045,ha="left",y=.975)
fig.text(.045,.952,"No sampled dictionary. Exhaustive 64×64 local translations × all 256 headings.",
         color=INK2,fontsize=10,ha="left")
fig.savefig(out,dpi=140,facecolor=SURF,bbox_inches="tight")
print(f"wrote {out}")
print(f"  trajectories={gt[-1]}  instances={gi[-1]:.1f}M  top10={cum[9]:.1f}%  top50={cum[49]:.1f}%")
