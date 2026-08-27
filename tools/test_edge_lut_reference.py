#!/usr/bin/env python3
"""Verify the hardware edge LUT is bit-for-bit equivalent to C edge_entry().

A phase error here can still emit an "edge tile" while moving the black line
outside the selected 8x8 row, visually collapsing projected walls to square
tile boundaries. Keep this test coupled to the C reference semantics.
"""
from pathlib import Path
import re

p=Path("src/tilesector_raster_gg.s")
s=p.read_text()
a=s.index("edge_lut$:")
b=s.index(".area _DATA",a)
vals=[int(x,16) for x in re.findall(r"0x([0-9A-Fa-f]{4})",s[a:b])]
assert len(vals)==930, f"expected 930 edge LUT words, got {len(vals)}"

BASE=39
FLIPX=0x0200
FLIPY=0x0400
PALETTE=0x0800

def clamp(v,lo,hi):
    return max(lo,min(hi,v))

def edge_entry(local,slope,bottom):
    attr=0
    if bottom:
        local=7-local
        slope=-slope
        attr=FLIPY|PALETTE
    if slope<0:
        mag=-slope
        local-=mag
        attr|=FLIPX
    else:
        mag=slope
    mag=min(mag,7)
    off=clamp(local,-7,8)
    return BASE + ((off+7)*8) + mag | attr

expected=[
    edge_entry(local,slope,bottom)
    for bottom in range(2)
    for slope in range(-7,8)
    for local in range(-15,16)
]

for i,(got,want) in enumerate(zip(vals,expected)):
    if got!=want:
        bottom=i//(15*31)
        rem=i%(15*31)
        slope=rem//31-7
        local=rem%31-15
        raise SystemExit(
            f"edge LUT mismatch bottom={bottom} slope={slope:+d} local={local:+d}: "
            f"got=0x{got:04X} want=0x{want:04X}"
        )

print("GG edge LUT matches C edge_entry reference: PASS (930 words)")
