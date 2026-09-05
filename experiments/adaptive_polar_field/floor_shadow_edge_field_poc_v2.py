#!/usr/bin/env python3
"""Topology-closed variant of floor_shadow_edge_field_poc.

The first probe conservatively labelled camera-on-polygon-boundary poses as a
fallback.  That is unnecessary for a convex shadow polygon.  The position-only
inside selector is made boundary-inclusive; forward projected edge crossings
then close the shape without Sutherland-Hodgman or a general geometry fallback.

Everything else (bearing fields, arbitrary yaw, edge vocabulary, penumbra and
ROM accounting) comes from the v1 probe so this is a controlled A/B.
"""
from __future__ import annotations

import floor_shadow_edge_field_poc as base


def point_in_poly_inclusive(cx:float,cy:float,p):
    for i in range(len(p)):
        if base.point_on_segment(cx,cy,p[i],p[(i+1)%len(p)]):
            return True
    pts=[(v.x_q4/16.0,v.y_q4/16.0) for v in p]
    inside=False;j=len(pts)-1
    for i,(xi,yi) in enumerate(pts):
        xj,yj=pts[j]
        if ((yi>cy)!=(yj>cy)) and cx < (xj-xi)*(cy-yi)/(yj-yi+1e-300)+xi:
            inside=not inside
        j=i
    return inside


def projected_shadow_frame_closed(cx,cy,yaw,bearing):
    shadow=base.np.zeros((base.oracle.H,base.oracle.W),dtype=bool)
    bf=0;yawq=yaw<<4
    for p,floor_z in base.POLYS:
        inside=point_in_poly_inclusive(cx,cy,p)
        intervals=[]
        for i in range(len(p)):
            a=p[i];b=p[(i+1)%len(p)]
            ba,fa=bearing(a);bb,fb=bearing(b);bf+=int(fa)+int(fb)
            iv=base.angular_interval(ba,bb,yawq)
            if iv is not None:intervals.append((a,b,iv))
        if not intervals:continue
        for sx in range(base.oracle.W):
            px=sx+0.5;rq=base.rel_q12_for_px(px);ys=[]
            for a,b,(lo,hi) in intervals:
                if rq<lo-1e-9 or rq>hi+1e-9:continue
                py=base.edge_py_at_px(a,b,floor_z,cx,cy,yaw,px)
                if py is not None:ys.append(py)
            if ys:
                ys.sort();uniq=[]
                for py in ys:
                    if not uniq or abs(py-uniq[-1])>1e-6:uniq.append(py)
                ys=uniq
            if inside:
                # Camera lies in/on the convex polygon: the positive ray has an
                # exit crossing and the near side runs off the bottom of screen.
                if not ys:continue
                y0=min(ys)
                for sy in range(73,base.oracle.H):
                    if sy+0.5>=y0:shadow[sy,sx]=True
            elif len(ys)>=2:
                y0=ys[0];y1=ys[-1]
                for sy in range(73,base.oracle.H):
                    py=sy+0.5
                    if py>=y0 and py<=y1:shadow[sy,sx]=True
    # No general topology fallback: boundary membership is an explicit
    # position selector and the edge-run parity closes the projected shape.
    return shadow,False,bf


base.point_in_poly=point_in_poly_inclusive
base.projected_shadow_frame=projected_shadow_frame_closed

if __name__=='__main__':
    raise SystemExit(base.main())
