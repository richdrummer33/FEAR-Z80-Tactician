#!/usr/bin/env python3
"""Topology-closed exact-line shadow field with a truly uniform bearing threshold.

floor_shadow_edge_field_poc.py originally accepts --bearing-threshold for the
four new light vertices, while its helper for the nine existing Polar vertex
identities still reads the module's default BEARING_THRESHOLD_Q12/MIN_LEAF_Q4.
That was fine for the first Q12=4 experiment but makes a threshold sweep unfair.

This wrapper preserves the exact v2 topology/line model and mirrors the CLI
threshold into the old-vertex evaluator before base.main() runs. No geometry or
error metric changes are made.
"""
from __future__ import annotations

import sys
import floor_shadow_edge_field_poc as base
import floor_shadow_edge_field_poc_v2 as closed  # installs topology-closed model


def arg_value(flag,default,cast):
    try:
        i=sys.argv.index(flag)
        return cast(sys.argv[i+1])
    except (ValueError,IndexError):
        return default

base.BEARING_THRESHOLD_Q12=arg_value('--bearing-threshold',base.BEARING_THRESHOLD_Q12,float)
base.MIN_LEAF_Q4=arg_value('--min-q4',base.MIN_LEAF_Q4,int)
base.projected_shadow_frame=closed.projected_shadow_frame_closed
base.point_in_poly=closed.point_in_poly_inclusive

if __name__=='__main__':
    raise SystemExit(base.main())
