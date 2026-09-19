# E1M1 Room 1 — latest renderer, FULL-only

Purpose: isolate the cost of the E1M1 XY layout from vertical profile special cases.

This branch starts from `feature/renderer-optimized-map`, not the older September-3 E1M1 renderer lineage.

Geometry policy:
- exact Room-1 source geometry/floor oracle originally generated from `map1.js`;
- retain only source segments marked structural/occluding;
- flatten all retained spans to one centered FULL wall;
- no windows, LINTEL, RISER, RAISED, stairs, or floor/ceiling inset surfaces;
- preserve both square pillars.

Renderer policy:
- keep current retained exact-key gate and FULL boundary patcher;
- widen packed endpoint IDs from 4 to 5 bits only for this build;
- extend retained wall state to all 30 physical surfaces;
- use a conservative 8x8-cell, 16-yaw-bin offline PVS.
