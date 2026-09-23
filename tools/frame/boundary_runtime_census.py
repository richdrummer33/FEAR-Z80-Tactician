#!/usr/bin/env python3
"""Validate runtime exact-X boundary events against the continuous wall oracle.

The dynamic compositor reuses the old seam scratch vectors, but their meaning is
now (x, left packed owner, right packed owner) plus a profile-only vertex ID.
This checker compares those events with the independent continuous visibility
oracle used by exact_projection_census.py.  It deliberately validates the
handoff geometry before tile synthesis: wrong/missing X events are the direct
cause of the visible 8-pixel accordion/caterpillar failure this branch targets.
"""
from __future__ import annotations

import argparse
import csv
import pathlib
import statistics
from collections import defaultdict

from exact_projection_census import arr, derive_segments, exact_chain, pct


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--map-inc",required=True)
    ap.add_argument("--frames",required=True)
    ap.add_argument("--events",required=True)
    ap.add_argument("--label",default="trace")
    a=ap.parse_args()

    text=pathlib.Path(a.map_inc).read_text()
    vx=arr(text,"k_tspf_vx")
    vy=arr(text,"k_tspf_vy")
    keys=arr(text,"k_tspf_keys")
    segs=derive_segments(keys,vx,vy)
    frames=list(csv.DictReader(open(a.frames,newline="")))

    runtime=defaultdict(list)
    with open(a.events,newline="") as f:
        for r in csv.DictReader(f):
            runtime[int(r["frame"])].append({
                "x":int(r["x"]),
                "vid":int(r["vid"]),
                "left":int(r["left"]) & 31,
                "right":int(r["right"]) & 31,
            })

    exact_total=matched=missing=extra=owner_mismatch=same_owner=0
    xerr=[]
    phase_err=[]
    crowded_exact=crowded_runtime=0
    per_frame_recall=[]
    worst=[]

    for fi,fr in enumerate(frames):
        ev=[
            e for e in exact_chain(fr,vx,vy,segs)
            if e["vid"] is not None and e["left"] is not None and e["right"] is not None
            and 0.0 <= e["x"] < 160.0 and e["left"] != e["right"]
            # The runtime intentionally emits no dynamic event when the
            # physical handoff quantizes onto an ordinary 8px hardware edge.
            # Treat a continuous corner within half a pixel of that edge as
            # already represented by the coarse handoff.
            and abs(e["x"]-round(e["x"]/8.0)*8.0) > 0.5
        ]
        rt=runtime.get(fi,[])
        exact_total += len(ev)
        same_owner += sum(1 for r in rt if r["left"]==r["right"])

        exact_groups=defaultdict(int)
        for e in ev:
            exact_groups[int(e["x"])>>3]+=1
        runtime_groups=defaultdict(int)
        for r in rt:
            runtime_groups[r["x"]>>3]+=1
        crowded_exact += sum(1 for n in exact_groups.values() if n>=3)
        crowded_runtime += sum(1 for n in runtime_groups.values() if n>=3)

        used=set()
        fm=0
        for e in ev:
            candidates=[]
            for j,r in enumerate(rt):
                if j in used or r["vid"]!=e["vid"]:
                    continue
                owner_ok=(r["left"]==e["left"] and r["right"]==e["right"])
                candidates.append((0 if owner_ok else 1,abs(r["x"]-e["x"]),j,r))
            if not candidates:
                missing+=1
                continue
            bad,dx,j,r=min(candidates)
            used.add(j)
            matched+=1
            fm+=1
            owner_mismatch+=bad
            xerr.append(dx)
            # Distance to the exact event's sub-tile phase is the quantity that
            # would turn a smooth 1px handoff back into an 8px ownership snap.
            pd=abs((r["x"]&7)-(e["x"]%8.0))
            phase_err.append(min(pd,8.0-pd))
            worst.append((dx,fi,e["vid"],e["x"],r["x"],e["left"],e["right"],r["left"],r["right"]))
        extra += len(rt)-len(used)
        per_frame_recall.append(100.0*fm/len(ev) if ev else 100.0)

    print(f"BOUNDARY_RUNTIME_CENSUS label={a.label} frames={len(frames)}")
    print(f"events exact={exact_total} matched={matched} missing={missing} extra={extra} "
          f"recall={(100.0*matched/exact_total if exact_total else 100.0):.2f}% "
          f"owner_mismatch={owner_mismatch} same_owner_runtime={same_owner}")
    if xerr:
        print(f"x_abs_error n={len(xerr)} mean={statistics.fmean(xerr):.3f}px "
              f"p50={pct(xerr,.50):.3f}px p95={pct(xerr,.95):.3f}px max={max(xerr):.3f}px")
        print(f"subtile_phase_abs_error mean={statistics.fmean(phase_err):.3f}px "
              f"p95={pct(phase_err,.95):.3f}px max={max(phase_err):.3f}px")
    print(f"frame_recall mean={statistics.fmean(per_frame_recall):.2f}% "
          f"p05={pct(per_frame_recall,.05):.2f}%")
    print(f"crowded_tiles_3plus exact={crowded_exact} runtime={crowded_runtime}")
    for rec in sorted(worst,reverse=True)[:12]:
        dx,fi,vid,ex,rx,el,er,rl,rr=rec
        print(f"XOFFENDER frame={fi} vid={vid} err={dx:.3f}px exact_x={ex:.3f} runtime_x={rx} "
              f"exact={el}->{er} runtime={rl}->{rr}")


if __name__=="__main__":
    main()
