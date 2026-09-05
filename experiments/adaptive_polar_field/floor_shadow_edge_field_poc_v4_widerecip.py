#!/usr/bin/env python3
"""Wide-reciprocal GG model for baked shadow edge playback.

This isolates the likely weakness of v3: the mature wall inverse-depth value is
8-bit and intentionally saturates inside ten world units.  Shadow boundaries
can pass much closer to the camera while their oblique forward depth remains
visible.  A generic PC-generated Q4 reciprocal table can retain that precision
without restoring division or polygon geometry at runtime.

The reciprocal itself is the familiar Polar scale:
    inv = round(40960 / abs(signed_distance_q4))
which equals 2560 / world_distance and fits in uint16 (maximum 40960).
A production Z80 path can multiply this uint16 by one signed Q7 normal factor
with a small 16x8 decomposition, then clamp only after forward depth is known.
"""
from __future__ import annotations

import math
import floor_shadow_edge_field_poc_v2 as base2
import floor_shadow_edge_field_poc as base
import floor_shadow_edge_field_poc_v3_ggdepth as v3


def edge_py_wide(a,b,floor_z,cx,cy,yaw,px):
    nx,ny=v3.q7_normal(a,b)
    cxq=int(round(cx*16.0));cyq=int(round(cy*16.0))
    dq4=base.shr0(nx*(a.x_q4-cxq)+ny*(a.y_q4-cyq),7)
    if dq4==0:return None
    sign=1 if dq4>0 else -1
    invd=min(40960,int(round(40960.0/abs(dq4))))
    ang=yaw*math.tau/256.0
    sn=int(round(math.sin(ang)*127.0));cs=int(round(math.cos(ang)*127.0))
    nf=base.shr0(nx*cs+ny*sn,7)
    ns=base.shr0(nx*(-sn)+ny*cs,7)
    if sign<0:nf=-nf;ns=-ns
    tanx=(px-80.0)/base.oracle.FOCAL
    invf=float(invd)*(float(nf)+float(ns)*tanx)/127.0
    if invf<=1e-9:return None
    scale=0.5 if floor_z==0.0 else 0.375
    return base.oracle.HORIZON+invf*scale


base.edge_py_at_px=edge_py_wide
base2.base.edge_py_at_px=edge_py_wide
base.projected_shadow_frame=base2.projected_shadow_frame_closed

if __name__=='__main__':
    raise SystemExit(base.main())
