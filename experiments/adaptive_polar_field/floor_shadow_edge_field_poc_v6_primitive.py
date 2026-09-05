#!/usr/bin/env python3
"""Primitive-line playback for the topology-closed baked shadow field.

The GG8 experiment quantized TWO things at once: it first normalized each world
line into a Q7 unit normal, then used Q7 camera trig and the wall reciprocal.
That produced large error. This POC isolates a cleaner representation.

Every static shadow edge already has exact Q4 endpoints. Reduce its line normal
by gcd to a primitive integer pair (A,B), normally small signed bytes. Runtime
signed line numerator is then exactly affine in camera position:

    Dq4 = A*(ax_q4-cam_x_q4) + B*(ay_q4-cam_y_q4)

No normalized-world-normal error exists. For this experiment division by Dq4
remains exact; only yaw coefficients are quantized. The camera-forward and side
coefficients are derived from QN sin/cos but remain signed int16 at Q7/Q8 for
this scene. This tells us whether a cheap primitive-line + separately solved
reciprocal can match the exact host model before designing that reciprocal.
"""
from __future__ import annotations

import argparse
import math
import sys
from functools import lru_cache

import floor_shadow_edge_field_poc as base
import floor_shadow_edge_field_poc_v2 as closed


def extra_arg(flag,default,cast):
    try:
        i=sys.argv.index(flag);return cast(sys.argv[i+1])
    except (ValueError,IndexError):return default

TRIG_BITS=extra_arg('--trig-bits',8,int)
if '--trig-bits' in sys.argv:
    i=sys.argv.index('--trig-bits');del sys.argv[i:i+2]
S=(1<<TRIG_BITS)-1

@lru_cache(maxsize=None)
def primitive(ax,ay,bx,by):
    dx=bx-ax;dy=by-ay
    g=math.gcd(abs(dx),abs(dy)) or 1
    return dy//g,-dx//g

@lru_cache(maxsize=None)
def yaw_coeff(A,B,yaw):
    ang=yaw*math.tau/256.0
    sn=int(round(math.sin(ang)*S));cs=int(round(math.cos(ang)*S))
    nf=A*cs+B*sn
    ns=A*(-sn)+B*cs
    return nf,ns


def edge_py_primitive(a,b,floor_z,cx,cy,yaw,px):
    A,B=primitive(a.x_q4,a.y_q4,b.x_q4,b.y_q4)
    cxq=int(round(cx*16.0));cyq=int(round(cy*16.0))
    # Same sign convention as the exact host: n dot (anchor-camera).
    D=A*(a.x_q4-cxq)+B*(a.y_q4-cyq)
    if D==0:return None
    nf,ns=yaw_coeff(A,B,yaw)
    t=(px-80.0)/base.oracle.FOCAL
    nd=float(nf)+float(ns)*t
    depth=float(D)*float(S)/(16.0*nd) if abs(nd)>1e-12 else -1.0
    if depth<=1e-8:return None
    return base.oracle.HORIZON+(base.oracle.CAMERA_Z-floor_z)*base.oracle.FOCAL/depth

# Make threshold sweep fair for old and new bearings too.
base.BEARING_THRESHOLD_Q12=extra_arg('--bearing-threshold',base.BEARING_THRESHOLD_Q12,float)
base.MIN_LEAF_Q4=extra_arg('--min-q4',base.MIN_LEAF_Q4,int)
base.edge_py_at_px=edge_py_primitive
closed.base.edge_py_at_px=edge_py_primitive
base.projected_shadow_frame=closed.projected_shadow_frame_closed
base.point_in_poly=closed.point_in_poly_inclusive

if __name__=='__main__':
    raise SystemExit(base.main())
