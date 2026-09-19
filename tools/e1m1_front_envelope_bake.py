#!/usr/bin/env python3
"""Bake/census exact first-hit visibility envelopes for flattened E1M1.

This is deliberately ROM-heavy and runtime-light.  For each fine spatial cell
we solve the complete 360-degree first-visible wall topology offline.  A stable
program is a cyclic sequence of:

    boundary_vertex_id, owner_surface_id

meaning "from this boundary ray until the next boundary ray, this surface is
the first wall hit".  Runtime therefore does not need to discover candidate
visibility, depth-sort walls, or arbitrate ordinary FULL-wall ownership.

The bake samples several legal camera positions inside each cell.  If the exact
cyclic program differs, that cell is marked FALLBACK rather than lying to the
runtime.  Smaller cells can be requested with --cell 0.5.

No compression cleverness: 16-bit cell->program IDs and a byte stream.  The
point of this rung is to measure/topologically solve the problem, not save ROM.
"""
from __future__ import annotations
import argparse, math, statistics
from pathlib import Path

import e1m1_latest_full_bake as base

TAU = 2.0 * math.pi
NO_WALL = 0xFF
FALLBACK = 0xFFFF
RADIUS = 4.0 / 16.0  # matches E1X_PLAYER_RADIUS_Q4=4

def walkable(px, py, offs, runs):
    if base.floor_at(px, py, offs, runs) is None:
        return False
    for dx,dy in ((-RADIUS,0),(RADIUS,0),(0,-RADIUS),(0,RADIUS)):
        if base.floor_at(px+dx, py+dy, offs, runs) is None:
            return False
    return True

def ray_first(px, py, ang, verts, segs):
    hits = base.intersections(px, py, ang, verts, segs)
    return hits[0][1] if hits else NO_WALL

def event_groups(px, py, verts):
    """Return unique vertex-ray events sorted around the camera.

    Collinear vertices share one angular event.  The closest vertex is the
    useful boundary witness: it is the occluder endpoint that can actually
    change first-hit ownership.
    """
    groups = {}
    for vid,(x,y) in enumerate(verts):
        dx,dy=x-px,y-py
        if abs(dx)<1e-12 and abs(dy)<1e-12:
            continue
        a=math.atan2(dy,dx) % TAU
        q=int(round(a * (1<<24) / TAU)) & ((1<<24)-1)
        d2=dx*dx+dy*dy
        old=groups.get(q)
        if old is None or d2 < old[0]:
            groups[q]=(d2,vid,a)
    ev=sorted((a,vid) for _d,vid,a in groups.values())
    return ev

def canonical_cycle(entries):
    """Canonical rotation of a cyclic (boundary_vid, owner_sid) program."""
    if not entries:
        return ()
    t=tuple(entries)
    return min(t[i:]+t[:i] for i in range(len(t)))

def envelope_program(px, py, verts, segs):
    ev=event_groups(px,py,verts)
    if len(ev)<2:
        return ()
    owners=[]
    n=len(ev)
    for i,(a0,_v0) in enumerate(ev):
        a1=ev[(i+1)%n][0]
        if i==n-1:
            a1 += TAU
        mid=((a0+a1)*0.5) % TAU
        owners.append(ray_first(px,py,mid,verts,segs))

    # Interval i begins at event i.  Emit an entry only when first-hit owner
    # changes there.  This is the exact visibility-envelope topology.
    out=[]
    for i,owner in enumerate(owners):
        prev=owners[(i-1)%n]
        if owner != prev:
            out.append((ev[i][1], owner))
    if not out:
        # Closed convex case with one owner is pathological for a room, but
        # retain a deterministic boundary witness if it ever occurs.
        out=[(ev[0][1], owners[0])]
    return canonical_cycle(out)

def sample_points(x0,y0,cell,offs,runs):
    # Centre + four inset corners. Never sample exactly on a topology boundary.
    f=(0.18,0.82)
    cand=[(x0+cell*0.5,y0+cell*0.5)]
    cand += [(x0+cell*fx,y0+cell*fy) for fy in f for fx in f]
    return [(x,y) for x,y in cand if walkable(x,y,offs,runs)]

def emit_arr(ctype,name,vals,per=16):
    out=[f"static const {ctype} {name}[{len(vals)}] = {{"]
    for i in range(0,len(vals),per):
        out.append("    "+", ".join(str(v) for v in vals[i:i+per])+",")
    out.append("};")
    return "\n".join(out)

def bake(cell, verts, segs, offs, runs):
    cols=int(round((base.WORLD_MAX_X-base.WORLD_MIN_X)/cell))
    rows=int(round((base.WORLD_MAX_Y-base.WORLD_MIN_Y)/cell))
    programs=[]
    prog_id={}
    grid=[]
    stable_cells=unstable_cells=walk_cells=empty_cells=0
    spans_per=[]
    sample_counts=[]
    for gy in range(rows):
        y0=base.WORLD_MIN_Y+gy*cell
        for gx in range(cols):
            x0=base.WORLD_MIN_X+gx*cell
            samples=sample_points(x0,y0,cell,offs,runs)
            if not samples:
                grid.append(FALLBACK)
                empty_cells += 1
                continue
            walk_cells += 1
            ps=[envelope_program(px,py,verts,segs) for px,py in samples]
            sample_counts.append(len(samples))
            p0=ps[0]
            if not p0 or any(p!=p0 for p in ps[1:]):
                grid.append(FALLBACK)
                unstable_cells += 1
                continue
            stable_cells += 1
            pid=prog_id.get(p0)
            if pid is None:
                pid=len(programs)
                prog_id[p0]=pid
                programs.append(p0)
                spans_per.append(len(p0))
            grid.append(pid)
    return {
        "cell":cell,"cols":cols,"rows":rows,"grid":grid,"programs":programs,
        "stable":stable_cells,"unstable":unstable_cells,"walk":walk_cells,
        "empty":empty_cells,"spans_per":spans_per,"sample_counts":sample_counts,
    }

def write_include(path, result):
    programs=result["programs"]
    off=[]; stream=[]
    for p in programs:
        off.append(len(stream))
        stream.append(len(p))
        for v,sid in p:
            stream.extend((v,sid))
    grid=result["grid"]
    text=[
        "/* GENERATED by tools/e1m1_front_envelope_bake.py. */",
        f"#define E1ENV_CELL_Q4 {int(round(result['cell']*16))}u",
        f"#define E1ENV_COLS {result['cols']}u",
        f"#define E1ENV_ROWS {result['rows']}u",
        f"#define E1ENV_PROGRAM_COUNT {len(programs)}u",
        "#define E1ENV_FALLBACK 65535u",
        emit_arr("uint16_t","k_e1env_cell_program",grid,12),
        emit_arr("uint16_t","k_e1env_program_off",off,12),
        emit_arr("uint8_t","k_e1env_program_stream",stream,20),
    ]
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text("\n\n".join(text)+"\n")
    return len(grid)*2 + len(off)*2 + len(stream),len(stream)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--geometry",default="src/generated/e1m1_room1_exact_geometry.h")
    ap.add_argument("--floor",default="src/generated/e1m1_room1_exact_floor.h")
    ap.add_argument("--cell",type=float,default=1.0,choices=(1.0,0.5,0.25))
    ap.add_argument("--out")
    args=ap.parse_args()

    verts0,segs0=base.parse_geometry(Path(args.geometry))
    offs,runs=base.parse_floor(Path(args.floor))
    verts,segs,_source_vertices=base.flatten_compact(verts0,segs0,())
    if any(p != base.FULL for _sid,_src,_a,_b,_bias,p in segs):
        raise SystemExit("front-envelope rung expects FULL-only geometry")

    result=bake(args.cell,verts,segs,offs,runs)
    spans=result["spans_per"]
    walk=result["walk"]
    stable=result["stable"]
    unstable=result["unstable"]
    coverage=(100.0*stable/walk) if walk else 0.0
    rom_bytes=stream_bytes=0
    if args.out:
        rom_bytes,stream_bytes=write_include(Path(args.out),result)

    # Geometry-grid alignment census.  Integer vertices are naturally aligned
    # to both 1.0 and 0.5 grids; report it explicitly because future authored
    # maps can make this a compiler contract rather than an accident.
    q=int(round(1.0/args.cell))
    aligned=sum(1 for x,y in verts if abs(x*q-round(x*q))<1e-9 and abs(y*q-round(y*q))<1e-9)

    print(f"E1ENV_BAKE_PASS cell={args.cell:g} grid={result['cols']}x{result['rows']} "
          f"walkable_cells={walk} stable={stable} unstable={unstable} stable_pct={coverage:.2f}")
    print(f"programs_unique={len(result['programs'])} "
          f"spans_mean={(statistics.mean(spans) if spans else 0):.2f} "
          f"spans_p95={(sorted(spans)[int(.95*(len(spans)-1))] if spans else 0)} "
          f"spans_max={(max(spans) if spans else 0)}")
    print(f"grid_aligned_vertices={aligned}/{len(verts)} cell={args.cell:g}")
    if args.out:
        print(f"rom_uncompressed_bytes={rom_bytes} program_stream_bytes={stream_bytes} "
              f"index_bytes={len(result['grid'])*2} offsets_bytes={len(result['programs'])*2}")
    print(f"fallback_cells={unstable} empty_or_unwalkable={result['empty']}")
    print("runtime_contract=cell->program; program=(boundary_vertex,first_hit_surface)*; "
          "ordinary FULL ownership/sort are bake-time facts")

if __name__=="__main__":
    main()
