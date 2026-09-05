#!/usr/bin/env python3
"""A/B the screen-affine shadow polygons WITHOUT a receiver-room mask.

The 13 polygon-edge inequalities already encode the four accepted shadow
footprints. This wrapper asks whether those footprints alone are sufficient at
runtime: each polygon is evaluated on its authored floor plane, their results
are ORed, and only the final ordinary floor-eligibility mask is applied.

If this matches the masked variant, no separate Room-A/Room-B receiver classifier
is needed by the GG light pass. If it does not, the delta quantifies exactly how
much receiver ownership still matters.
"""
from __future__ import annotations
import numpy as np
import floor_shadow_screen_affine_poc as base


def frame_nomask(cx,cy,yaw,trig_bits):
    S=(1<<trig_bits)-1
    ang=yaw*base.math.tau/256.0
    sn=int(round(base.math.sin(ang)*S));cs=int(round(base.math.cos(ang)*S))
    cxq=int(round(cx*16.0));cyq=int(round(cy*16.0))
    yy,xx=np.mgrid[73:base.oracle.H,0:base.oracle.W]
    v2=(2*yy+1)-144
    dx2=(2*xx+1)-160

    wx4,wy4,d4=base.oracle.camera_floor_arrays(cx,cy,yaw,base.oracle.ROOM_B_FLOOR_Z)
    room4=base.oracle.points_in_poly(wx4,wy4,base.oracle.ROOM_B)&(d4>0)
    wx0,wy0,d0=base.oracle.camera_floor_arrays(cx,cy,yaw,0.0)
    e0=(base.oracle.points_in_poly(wx0,wy0,base.oracle.ROOM_A)|base.oracle.points_in_poly(wx0,wy0,base.oracle.CONNECTOR))&(d0>0)
    use4=room4&(~e0|(d4<d0));use0=e0&~use4
    eligible=use0|use4

    sh=np.zeros_like(eligible)
    for pi,lines in enumerate(base.POLY_LINES):
        h=16 if pi<2 else 12
        inside=np.ones_like(eligible)
        for ln in lines:
            Lc=ln.A*cxq+ln.B*cyq+ln.C
            nf=ln.A*cs+ln.B*sn
            ns=ln.A*(-sn)+ln.B*cs
            E2=(S*Lc)*v2 + (32*h*int(base.oracle.FOCAL))*nf + (16*h*ns)*dx2
            inside &= (ln.inside*E2)>=0
        sh |= inside
    full_e=np.zeros((base.oracle.H,base.oracle.W),bool)
    full_s=np.zeros_like(full_e)
    full_e[73:]=eligible
    full_s[73:]=sh&eligible
    return full_e,full_s

base.frame=frame_nomask

if __name__=='__main__':
    raise SystemExit(base.main())
