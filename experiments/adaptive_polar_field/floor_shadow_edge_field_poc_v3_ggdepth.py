#!/usr/bin/env python3
"""GG-depth-model variant of the topology-closed baked shadow edge field.

v2 proves the representation with exact straight-line projection.  This variant
keeps the same baked endpoint bearings and topology closure, but replaces the
edge Y solve with the kind of cheap integer ingredients the Game Gear can use:

  * one signed Q7 normal per straight shadow edge;
  * signed perpendicular camera distance from two small products (cardinal
    lines later collapse to subtraction shortcuts);
  * the mature 8-bit inverse-distance LUT/interpolator;
  * Q7 camera-forward / camera-side normal components;
  * affine inverse depth across screen tan(X), exactly as Polar walls already do.

The point is to determine whether that cheap playback is already accurate
enough or whether near-edge regions deserve a wider/dedicated baked reciprocal
field before any Z80 implementation is committed.
"""
from __future__ import annotations

import math
import floor_shadow_edge_field_poc_v2 as base2
import floor_shadow_edge_field_poc as base
import endpoint_depth_field_poc as ep

EXTRA=ep.load_extra()


def q7_normal(a,b):
    ax=a.x_q4/16.0;ay=a.y_q4/16.0;bx=b.x_q4/16.0;by=b.y_q4/16.0
    nx=by-ay;ny=ax-bx
    m=math.hypot(nx,ny)
    if m<1e-12:return 0,0
    return int(round(nx*127.0/m)),int(round(ny*127.0/m))


def edge_py_gg(a,b,floor_z,cx,cy,yaw,px):
    nx,ny=q7_normal(a,b)
    cxq=int(round(cx*16.0));cyq=int(round(cy*16.0))
    dq4=base.shr0(nx*(a.x_q4-cxq)+ny*(a.y_q4-cyq),7)
    if dq4==0:return None
    sign=1 if dq4>0 else -1
    invd=ep.inv_for_dq4(EXTRA,abs(dq4))
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


base.edge_py_at_px=edge_py_gg
base2.base.edge_py_at_px=edge_py_gg
base.projected_shadow_frame=base2.projected_shadow_frame_closed

if __name__=='__main__':
    raise SystemExit(base.main())
