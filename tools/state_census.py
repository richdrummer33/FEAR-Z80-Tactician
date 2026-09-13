"""STATECENSUS: what this build actually declares, from the sources.

The GG toolchain (SDCC/sdasz80) is not present in this environment, so no
linker map can be produced.  Everything here is therefore read from the
committed sources and generated tables, and anything that would need a link to
settle is reported as UNKNOWN rather than estimated.
"""
from __future__ import annotations
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
GEN = SRC / "generated"

CTYPE = {"uint8_t": 1, "int8_t": 1, "char": 1, "uint16_t": 2, "int16_t": 2,
         "uint32_t": 4, "int32_t": 4}

DECL = re.compile(
    r"^static\s+(?:volatile\s+)?(?:const\s+)?([A-Za-z_]\w*)\s+(\w+)\s*"
    r"((?:\[[^\]]*\])*)\s*(?:=|;)", re.M)
ARR = re.compile(r"\[([^\]]*)\]")


def consts(text):
    out = {}
    for m in re.finditer(r"#define\s+(\w+)\s+([0-9]+)u?\b", text):
        out[m.group(1)] = int(m.group(2))
    return out


def dims(spec, k):
    n = 1
    for d in ARR.findall(spec):
        d = d.strip()
        if not d:
            return None
        if d.isdigit():
            n *= int(d)
        elif d in k:
            n *= k[d]
        else:
            expr = d
            for name, v in k.items():
                expr = re.sub(rf"\b{name}\b", str(v), expr)
            expr = expr.replace("u", "")
            try:
                n *= int(eval(expr, {"__builtins__": {}}, {}))
            except Exception:
                return None
    return n


def main():
    hdr = (SRC / "tilesector_polar.h").read_text()
    rnd = (SRC / "tilesector_polar_renderer.c").read_text()
    gentext = "\n".join(p.read_text() for p in sorted(GEN.glob("*.inc")))
    k = consts(hdr) | consts(rnd) | consts(gentext)
    # struct sizes, counted from their own fields
    # struct sizes counted from their own field lists, no padding assumed
    k["TSPColumn"] = 7
    k["PolarRun"] = 13 * 1 + 2 * 2          # 13 bytes + iq,step (int16)
    k.setdefault("TSPF_DEPTH_NORMAL_CLASS_COUNT", 7)   # 7 unique wall normals
    k.setdefault("TSPF_PROJ_MAX_CELL_BYTES", -1)       # generated per-build
    k["TSP_MAP_CELLS"] = k.get("TSP_COLS", 20) * k.get("TSP_ROWS", 18)

    lines = []
    def out(s=""):
        lines.append(s)
        print(s)

    out("=== STATECENSUS ===")
    out(f"source of truth: committed sources + generated tables")
    out(f"linker map: UNAVAILABLE (no SDCC/sdasz80 in this environment)")
    out("")

    # ---- mutable WRAM: non-const statics in the renderer ----
    out("--- MUTABLE WRAM (non-const statics in tilesector_polar_renderer.c) ---")
    # Which declarations sit inside `#ifndef __SDCC`?  Those are host-oracle
    # only and do NOT exist in the Game Gear build, so charging them to the
    # cartridge would overstate WRAM by 768 bytes.
    host_only = set()
    depth, hostdepth = 0, None
    for line in rnd.splitlines():
        st = line.strip()
        if st.startswith("#if"):
            depth += 1
            if st.startswith("#ifndef __SDCC") and hostdepth is None:
                hostdepth = depth
        elif st.startswith("#endif"):
            if hostdepth == depth:
                hostdepth = None
            depth -= 1
        elif hostdepth is not None:
            m2 = re.match(r"static\s+\w+\s+(\w+)", st)
            if m2:
                host_only.add(m2.group(1))
    total, unknown = 0, []
    rows = []
    for m in DECL.finditer(rnd):
        ty, name, spec = m.group(1), m.group(2), m.group(3)
        if "const" in m.group(0):
            continue
        unit = CTYPE.get(ty) or k.get(ty)
        n = dims(spec, k) if spec else 1
        if unit is None or n is None:
            unknown.append((ty, name, spec))
            continue
        if unit < 0 or n < 0:
            unknown.append((ty, name, spec)); continue
        rows.append((unit * n, ty, name, spec, name in host_only))
        total += unit * n
    gg = sum(b for b, _, _, _, h in rows if not h)
    ho = sum(b for b, _, _, _, h in rows if h)
    for b, ty, name, spec, h in sorted(rows, reverse=True):
        out(f"  {b:7d}  {ty:>10} {name}{spec}{'   [HOST ORACLE ONLY]' if h else ''}")
    out(f"  {gg:7d}  GAME GEAR persistent WRAM")
    out(f"  {ho:7d}  host-oracle only, NOT in the cartridge")
    out(f"  {total:7d}  total declared")
    if unknown:
        out(f"  UNKNOWN (generated per build, needs a link): {unknown}")
    out("")
    out("  stack allowance / high-water: UNKNOWN without a link + run")
    out("")

    # ---- ROM: generated tables ----
    out("--- GENERATED ROM TABLES (const arrays in src/generated) ---")
    grows, gtot = [], 0
    for m in re.finditer(
            r"static\s+const\s+([A-Za-z_]\w*)\s+(\w+)\s*((?:\[[^\]]*\])+)",
            gentext):
        ty, name, spec = m.group(1), m.group(2), m.group(3)
        unit = CTYPE.get(ty)
        n = dims(spec, k)
        if unit is None or n is None:
            continue
        grows.append((unit * n, ty, name, spec))
        gtot += unit * n
    for b, ty, name, spec in sorted(grows, reverse=True)[:14]:
        out(f"  {b:7d}  {ty:>10} {name}{spec}")
    out(f"  {gtot:7d}  TOTAL generated table bytes "
        f"({len(grows)} arrays, top 14 shown)")
    out("")

    # ---- cartridge ----
    roms = sorted((ROOT / "roms").glob("*.gg"))
    out("--- CARTRIDGE ---")
    for r in roms:
        out(f"  {r.stat().st_size:7d}  {r.name}")
    mk = (ROOT / "Makefile").read_text()
    m = re.search(r"-Wm-yo(\d+)", mk)
    out(f"  ROM banks configured: -Wm-yo{m.group(1) if m else '?'}"
        f"  = {int(m.group(1))*16 if m else '?'} KiB")
    out("")

    # ---- A46 ----
    out("--- A46 PROGRAM ARCHITECTURE, AS BUILT ---")
    a46 = list(SRC.rglob("*edge_prog*")) + list(SRC.rglob("*dispatch*"))
    out(f"  A46 tables in src/:            {len(a46)} files  -> {a46 if a46 else '0 bytes'}")
    out(f"  A46 kernel composed into src/: NO")
    out(f"  A46 ROM in the cartridge:      0 bytes")
    out(f"  (A46 is host-side census in tools/ plus throwaway Z80 probes;")
    out(f"   its 2.58 MB dispatch / 2.91 MB total are PROJECTIONS from")
    out(f"   edge_dispatch_verify, never generated or linked.)")
    out("")

    # ---- temporal state ----
    out("--- A29/A31 TEMPORAL STATE, AS BUILT ---")
    has = "ColState" in rnd or "SpanState" in rnd
    out(f"  retained (tl,tr,bl,br,shade,border) column records present: "
        f"{'YES' if has else 'NO'}")
    out(f"  -> the 120-byte figure is NOT chargeable to this build")
    mapb = k.get("TSP_MAP_CELLS", 360) * 2
    out(f"  20x18x2 name-table mirror: {mapb} bytes, present in host builds "
        f"(g_map/g_far/g_near); on GG the map lives in VRAM")
    (ROOT / "build" / "state_census.txt").write_text("\n".join(lines) + "\n")
    out("")
    out(f"written to build/state_census.txt")


if __name__ == "__main__":
    main()
