#!/usr/bin/env python3
"""Sanity-check split generated Polar C arrays.

C silently zero-fills underspecified arrays, which is dangerous for LUTs split
across multiple generated .inc parts. Fail if any explicit array dimension does
not match the number of emitted initializers.
"""
from __future__ import annotations
import pathlib,re,sys

ROOT=pathlib.Path(__file__).resolve().parents[1]
GEN=ROOT/"src"/"generated"
text="\n".join(p.read_text() for p in sorted(GEN.glob("tilesector_polar_data_part*.inc")))
pat=re.compile(r"static\s+const\s+[^;=]+?\b(k_tspf_[A-Za-z0-9_]+)\s*\[(\d+)\]\s*=\s*\{(.*?)\};",re.S)
seen=0
for m in pat.finditer(text):
    seen+=1
    name=m.group(1); declared=int(m.group(2))
    actual=len(re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+",m.group(3)))
    if actual!=declared:
        raise SystemExit(f"{name}: declared {declared}, emitted {actual} initializers")
if not seen:
    raise SystemExit("no generated Polar arrays found")
print(f"OK: {seen} generated Polar arrays have complete initializer counts")
