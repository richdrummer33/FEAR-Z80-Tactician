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

def surface_faces_camera(px,py,seg,verts):
    # Match project_key's directed-span acceptance exactly at topology level:
    # len=(bearing(v1)-bearing(v0)) mod turn must be strictly between 0 and pi.
    _sid,_src,a,b,_bias,_profile=seg
    ax,ay=verts[a]; bx,by=verts[b]
    a0=math.atan2(ay-py,ax-px) % TAU
    a1=math.atan2(by-py,bx-px) % TAU
    d=(a1-a0) % TAU
    return d > 1e-12 and d < math.pi

def ray_first(px, py, ang, verts, segs):
    hits = base.intersections(px, py, ang, verts, segs)
    for _t,sid in hits:
        if surface_faces_camera(px,py,segs[sid],verts):
            return sid
    return NO_WALL

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
    # At quarter-world cells the runtime position lattice is FINITE: Q4 means
    # exactly four representable x values by four y values inside a 0.25 cell.
    # Enumerate every one rather than sampling. A cell called stable is then
    # exact for every player x_q4/y_q4 state the ROM can actually occupy.
    if cell <= 0.2500001:
        n=max(1,int(round(cell*16.0)))
        cand=[(x0+ix/16.0,y0+iy/16.0) for iy in range(n) for ix in range(n)]
    else:
        # Coarser rungs are census-only: centre + four inset corners.
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

def write_banked_sources(outdir, result, bank_base=32, rows_per_bank=16, prog_payload=10500):
    """Emit deliberately simple banked ROM tables for the quarter-cell runtime.

    Cell indices are split by row bands so lookup needs one banked call/frame.
    Program records are greedily split into banks so load needs one more banked
    call/frame.  No dictionary/entropy compression yet.
    """
    outdir=Path(outdir)
    outdir.mkdir(parents=True,exist_ok=True)
    grid=result["grid"]; cols=result["cols"]; rows=result["rows"]; programs=result["programs"]

    idx_banks=[]
    bank=bank_base
    for row0 in range(0,rows,rows_per_bank):
        row1=min(rows,row0+rows_per_bank)
        vals=grid[row0*cols:row1*cols]
        fn=f"e1env_idx_{len(idx_banks):02d}"
        src=[
            f"#pragma bank {bank}",
            "#include <stdint.h>",
            "#include <gbdk/platform.h>",
            emit_arr("uint16_t","k_idx",vals,12),
            f"uint16_t {fn}(uint16_t i) BANKED {{ return k_idx[i]; }}",
            "",
        ]
        (outdir/f"e1env_idx_{len(idx_banks):02d}.c").write_text("\n\n".join(src))
        idx_banks.append((row0,row1,bank,fn,len(vals)))
        bank += 1

    # Encode each program as count,(boundary_vertex,surface)*.
    recs=[]
    for p in programs:
        b=[len(p)]
        for v,sid in p: b.extend((v,sid))
        recs.append(b)

    prog_banks=[]; cur=[]; cur_bytes=0; base_pid=0
    for pid,rec in enumerate(recs):
        need=len(rec)+2  # conservative offset-table cost
        if cur and cur_bytes+need>prog_payload:
            prog_banks.append((base_pid,cur))
            base_pid=pid; cur=[]; cur_bytes=0
        cur.append(rec); cur_bytes += need
    if cur: prog_banks.append((base_pid,cur))

    prog_meta=[]
    for bi,(base_pid,reclist) in enumerate(prog_banks):
        offsets=[]; stream=[]
        for rec in reclist:
            offsets.append(len(stream)); stream.extend(rec)
        fn=f"e1env_prog_{bi:02d}"
        src=[
            f"#pragma bank {bank}",
            "#include <stdint.h>",
            "#include <gbdk/platform.h>",
            emit_arr("uint16_t","k_off",offsets,12),
            emit_arr("uint8_t","k_stream",stream,20),
            f"""uint8_t {fn}(uint16_t local, uint8_t *dst) BANKED {{
    uint16_t off=k_off[local];
    uint8_t n=k_stream[off], bytes=(uint8_t)(1u+(uint8_t)(n<<1)), i;
    for(i=0u;i<bytes;++i) dst[i]=k_stream[off+i];
    return n;
}}""",
            "",
        ]
        (outdir/f"e1env_prog_{bi:02d}.c").write_text("\n\n".join(src))
        prog_meta.append((base_pid,len(reclist),bank,fn))
        bank += 1

    dispatch_bank=bank_base-1
    hdr=[
        "#ifndef E1ENV_GENERATED_H",
        "#define E1ENV_GENERATED_H",
        "#include <stdint.h>",
        "#include <gbdk/platform.h>",
        f"#define E1ENV_WORLD_MIN_X {base.WORLD_MIN_X}",
        f"#define E1ENV_WORLD_MIN_Y {base.WORLD_MIN_Y}",
        f"#define E1ENV_CELL_Q4 {int(round(result['cell']*16))}u",
        f"#define E1ENV_COLS {cols}u",
        f"#define E1ENV_ROWS {rows}u",
        f"#define E1ENV_ROWS_PER_INDEX_BANK {rows_per_bank}u",
        f"#define E1ENV_PROGRAM_COUNT {len(programs)}u",
        "#define E1ENV_FALLBACK 65535u",
        "#define E1ENV_MAX_PROGRAM_BYTES 64u",
    ]
    for _r0,_r1,_bank,fn,_n in idx_banks:
        hdr.append(f"uint16_t {fn}(uint16_t i) BANKED;")
    for _base,_count,_bank,fn in prog_meta:
        hdr.append(f"uint8_t {fn}(uint16_t local, uint8_t *dst) BANKED;")
    hdr += [
        "uint16_t e1env_lookup_program_q4(int16_t xq, int16_t yq) BANKED;",
        "uint8_t e1env_load_program(uint16_t pid, uint8_t *dst) BANKED;",
        "#endif",
        "",
    ]
    (outdir/"e1env_generated.h").write_text("\n".join(hdr))

    cell_q4=int(round(result['cell']*16))
    # Current runtime intentionally uses quarter-unit cells and 16-row bands;
    # emit shifts/masks instead of asking SDCC for integer division helpers.
    if cell_q4==4:
        gx_expr="((uint16_t)rx >> 2)"
        gy_expr="((uint16_t)ry >> 2)"
    else:
        gx_expr=f"((uint16_t)rx / {cell_q4}u)"
        gy_expr=f"((uint16_t)ry / {cell_q4}u)"
    if rows_per_bank==16:
        local_expr="(uint16_t)(((gy & 15u) * E1ENV_COLS) + gx)"
        band_expr="(uint8_t)(gy >> 4)"
    else:
        local_expr="(uint16_t)(((gy % E1ENV_ROWS_PER_INDEX_BANK) * E1ENV_COLS) + gx)"
        band_expr="(uint8_t)(gy / E1ENV_ROWS_PER_INDEX_BANK)"

    dispatch=[
        f"#pragma bank {dispatch_bank}",
        "#include <stdint.h>",
        "#include <gbdk/platform.h>",
        '#include "e1env_generated.h"',
        "",
        "uint16_t e1env_lookup_program_q4(int16_t xq, int16_t yq) BANKED {",
        "    int16_t rx=(int16_t)(xq-(E1ENV_WORLD_MIN_X<<4));",
        "    int16_t ry=(int16_t)(yq-(E1ENV_WORLD_MIN_Y<<4));",
        "    uint16_t gx,gy,local;",
        "    uint8_t band;",
        "    if(rx<0||ry<0) return E1ENV_FALLBACK;",
        f"    gx={gx_expr}; gy={gy_expr};",
        "    if(gx>=E1ENV_COLS||gy>=E1ENV_ROWS) return E1ENV_FALLBACK;",
        f"    local={local_expr};",
        f"    band={band_expr};",
        "    switch(band) {",
    ]
    for bi,(_r0,_r1,_bank,fn,_n) in enumerate(idx_banks):
        dispatch.append(f"    case {bi}u: return {fn}(local);")
    dispatch += [
        "    default: return E1ENV_FALLBACK;",
        "    }",
        "}",
        "",
        "uint8_t e1env_load_program(uint16_t pid, uint8_t *dst) BANKED {",
    ]
    for bi,(base_pid,count,_bank,fn) in enumerate(prog_meta):
        end_pid=base_pid+count
        prefix="if" if bi==0 else "else if"
        dispatch.append(f"    {prefix}(pid<{end_pid}u) return {fn}((uint16_t)(pid-{base_pid}u),dst);")
    dispatch += ["    return 0u;","}",""]
    (outdir/"e1env_dispatch.c").write_text("\n".join(dispatch))

    manifest=[
        f"index_banks={len(idx_banks)}",
        f"program_banks={len(prog_meta)}",
        f"bank_first={bank_base}",
        f"bank_last={bank-1}",
        f"dispatch_bank={dispatch_bank}",
        f"generated_c_files={1+len(idx_banks)+len(prog_meta)}",
    ]
    (outdir/"manifest.txt").write_text("\n".join(manifest)+"\n")
    return idx_banks,prog_meta

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--geometry",default="src/generated/e1m1_room1_exact_geometry.h")
    ap.add_argument("--floor",default="src/generated/e1m1_room1_exact_floor.h")
    ap.add_argument("--cell",type=float,default=1.0,choices=(1.0,0.5,0.25))
    ap.add_argument("--out")
    ap.add_argument("--emit-banked-dir")
    ap.add_argument("--bank-base",type=int,default=32)
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
    banked=None
    if args.emit_banked_dir:
        banked=write_banked_sources(args.emit_banked_dir,result,args.bank_base)

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
    if banked:
        print(f"banked_index_banks={len(banked[0])} banked_program_banks={len(banked[1])} "
              f"bank_range={args.bank_base}..{args.bank_base+len(banked[0])+len(banked[1])-1}")
    print(f"fallback_cells={unstable} empty_or_unwalkable={result['empty']}")
    if args.cell <= 0.2500001:
        print("stability_proof=exhaustive_Q4_positions_per_cell")
    print("runtime_contract=cell->program; program=(boundary_vertex,first_hit_surface)*; "
          "ordinary FULL ownership/sort are bake-time facts")

if __name__=="__main__":
    main()
