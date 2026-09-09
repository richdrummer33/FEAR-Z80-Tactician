#!/usr/bin/env python3
"""Measure the COLUMN-SOLVE stage's workload on real camera poses.

Column-solve is the last uncosted line of the span-interpreter budget: given a
span that survived the yaw clip, turn it into the Q6 inverse-depth ramp the
emit stage walks across the run's columns. In the C reference
(src/tilesector_polar_renderer.c) that is:

    invd = inv_for_dq4(wall_d_q4(sid, anchor, s))     ; one distance -> 1/z
    inv0 = inv_at_invd(sid, invd, yaw+lo, lo)         ; left  endpoint
    inv1 = inv_at_invd(sid, invd, yaw+hi, hi)         ; right endpoint
    iq   = inv0 << 6                                   ; Q6 start
    step = ((inv1-inv0) * k_col_recip_q8[n]) >> 2      ; Q6 per-column step

Every one of those has a cheap special case and an expensive general case, and
the split is a property of THIS MAP's geometry, not of the algorithm. So this
counts which case actually fires, per visible span, over the same real-pose
sampling span_decode_workload.py and z80_bearing_bench.py use.

WHY COUNT MULTIPLIES SPECIFICALLY
----------------------------------
The three kernels measured so far all landed on the same answer: a shift-add
multiply loop is the single most expensive thing this renderer does per unit
of work (~324 T for a 6-iteration signed 8xN loop, measured, not guessed - see
tools/z80_bearing_bench.py). Table lookups and adds are noise beside it. So
"how many multiplies does column-solve need per update" IS the budget, to
within about 15%, and it is worth knowing before writing the kernel rather
than after.

The cardinal-normal shortcuts in wall_d_q4 and inv_at_invd exist precisely to
delete multiplies, and 14 of this map's 17 segments are cardinal - this tool
says what that is actually worth at runtime.

    make column-solve-workload
"""
from __future__ import annotations
import collections
import math
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "experiments" / "adaptive_polar_field"))
from span_block_bake import (  # noqa: E402
    load, build_block, OP_SPAN, OP_SPANC, OP_GATE, OP_END, selector_pass,
)
from span_decode_workload import exact_bearing, visible_window  # noqa: E402
from local_projection_field_poc import (  # noqa: E402
    load as load_field, GRID_W, GRID_H, CELL_Q4,
)
import re

GEN = ROOT / "src" / "generated"
TSPF_NEAR_Z_Q4 = None   # read from the header below
TSPF_FAR_Z_Q4 = None


def arr(text, name):
    m = re.search(r"static\s+const\s+[^;=]+?\b" + re.escape(name)
                  + r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise SystemExit("missing generated array " + name)
    return [int(x, 0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+", m.group(1))]


def s8(v):
    return v - 256 if v > 127 else v


def load_tables():
    text = "\n".join(p.read_text() for p in sorted(GEN.glob("tilesector_polar_data_part*.inc")))
    return {
        "nx": [s8(v) for v in arr(text, "k_tspf_nx_q5")],
        "ny": [s8(v) for v in arr(text, "k_tspf_ny_q5")],
        "anchor": arr(text, "k_tspf_seg_anchor"),
        "vx": arr(text, "k_tspf_vx"),
        "vy": arr(text, "k_tspf_vy"),
        "invz": arr(text, "k_tspf_invz"),
    }


def _const(expr):
    """Evaluate a simple integer #define body like `(10<<4)`. Restricted to
    digits and shift/paren tokens so a stray macro cannot execute anything."""
    expr = expr.split("/*")[0].split("//")[0].strip()
    if not re.fullmatch(r"[()0-9xXa-fA-F<>+\-*\s]+", expr):
        raise SystemExit(f"unexpected #define body {expr!r}")
    return int(eval(expr, {"__builtins__": {}}, {}))


def near_far():
    """TSPF_NEAR_Z_Q4 / TSPF_FAR_Z_Q4 come from the renderer source, not the
    generated pack - read them rather than hardcoding a guess."""
    src = (ROOT / "src" / "tilesector_polar_renderer.c").read_text()
    hdrs = [src]
    for h in (ROOT / "src").glob("*.h"):
        hdrs.append(h.read_text())
    near = far = None
    for t in hdrs:
        m = re.search(r"#define\s+TSPF_NEAR_Z_Q4\s+(.+)", t)
        if m and near is None:
            near = _const(m.group(1))
        m = re.search(r"#define\s+TSPF_FAR_Z_Q4\s+(.+)", t)
        if m and far is None:
            far = _const(m.group(1))
    if near is None or far is None:
        raise SystemExit("could not find TSPF_NEAR_Z_Q4 / TSPF_FAR_Z_Q4")
    return near, far


class Counters:
    FIELDS = (
        "wall_cardinal", "wall_general",
        "invd_near_clamp", "invd_far_clamp", "invd_interp",
        "invat_cardinal", "invat_general",
        "mul8x8", "mul16x8", "tab",
    )

    def __init__(self):
        for f in self.FIELDS:
            setattr(self, f, 0)


def wall_d_q4(T, c, sid, xq4, yq4):
    nx, ny = T["nx"][sid], T["ny"][sid]
    anchor = T["anchor"][sid]
    if ny == 0 and nx in (32, -32):
        c.wall_cardinal += 1
        wall = T["vx"][anchor] << 4
        return (wall - xq4) if nx > 0 else (xq4 - wall)
    if nx == 0 and ny in (32, -32):
        c.wall_cardinal += 1
        wall = T["vy"][anchor] << 4
        return (wall - yq4) if ny > 0 else (yq4 - wall)
    c.wall_general += 1
    c.mul8x8 += 4                      # nx*dx, ny*dy, nx*fx, ny*fy
    xi, yi = xq4 >> 4, yq4 >> 4
    fx, fy = xq4 & 15, yq4 & 15
    dx, dy = T["vx"][anchor] - xi, T["vy"][anchor] - yi
    whole = nx * dx + ny * dy
    frac = nx * fx + ny * fy
    return shr_signed(whole, 1) - shr_signed(frac, 5)


def shr_signed(v, s):
    return v >> s if v >= 0 else -((-v) >> s)


def inv_for_dq4(T, c, dq4, near, far):
    a = abs(dq4)
    c.tab += 1
    if a <= near:
        c.invd_near_clamp += 1
        return 255
    if a >= far:
        c.invd_far_clamp += 1
        return T["invz"][127]
    c.invd_interp += 1
    c.mul8x8 += 1                      # d*f
    c.tab += 2
    z, f = a >> 4, a & 15
    x0, x1 = T["invz"][z], T["invz"][(z + 1) & 0xFF]
    d = x1 - x0
    return x0 + shr_signed(d * f + (8 if d >= 0 else -8), 4)


def inv_at_invd(T, c, sid):
    """Only the OPERATION MIX matters here, not the value - the value is
    already proven correct by the C renderer and by polar-test. Counting is
    done on the same branch structure the C takes."""
    nx, ny = T["nx"][sid], T["ny"][sid]
    c.tab += 2                          # sin, cos
    if (ny == 0 and nx in (32, -32)) or (nx == 0 and ny in (32, -32)):
        c.invat_cardinal += 1
    else:
        c.invat_general += 1
        c.mul8x8 += 2                   # nx*cs, ny*sn
    c.mul16x8 += 2                      # invd*dot, q*sec
    c.tab += 1                          # sec_q7


def main():
    near, far = near_far()
    T = load_tables()
    d = load()
    print("=== COLUMN-SOLVE WORKLOAD (real poses, endpoint path) ===")
    print(f"TSPF_NEAR_Z_Q4={near} TSPF_FAR_Z_Q4={far}")
    nseg = len(T["nx"])
    card = sum(1 for i in range(nseg)
               if (T["ny"][i] == 0 and T["nx"][i] in (32, -32))
               or (T["nx"][i] == 0 and T["ny"][i] in (32, -32)))
    print(f"segments: {nseg} total, {card} cardinal ({card/nseg:.1%}) - "
          f"the shortcut's static reach\n")

    per_update_mul8 = []
    per_update_mul16 = []
    per_update_spans = []
    agg = Counters()
    poses = 0
    for gy in range(GRID_H):
        for gx in range(GRID_W):
            ops, _ = build_block(d, gx, gy)
            if ops is None:
                continue
            px = gx * CELL_Q4 + CELL_Q4 / 2
            py = gy * CELL_Q4 + CELL_Q4 / 2
            lx = ly = CELL_Q4 // 2
            for yaw in range(0, 256, 8):
                yaw_q12 = yaw * 16.0
                c = Counters()
                visible = 0
                skip = False
                prev = None
                for op in ops:
                    if op[0] == OP_END:
                        break
                    if op[0] == OP_GATE:
                        skip = not selector_pass(d, op[1], lx, ly)
                        continue
                    if skip:
                        skip = False
                        continue
                    if op[0] == OP_SPAN:
                        _, k, v0, v1, sid = op
                        a0 = exact_bearing(d, v0, px, py)
                    else:
                        _, k, v1, sid = op
                        a0 = prev
                    a1 = exact_bearing(d, v1, px, py)
                    prev = a1
                    if visible_window(a0, a1, yaw_q12) is None:
                        continue
                    visible += 1
                    dq4 = wall_d_q4(T, c, sid, int(px), int(py))
                    inv_for_dq4(T, c, dq4, near, far)
                    inv_at_invd(T, c, sid)
                    inv_at_invd(T, c, sid)
                    c.mul16x8 += 1      # (inv1-inv0) * k_col_recip_q8[n]
                    c.tab += 1
                for f in Counters.FIELDS:
                    setattr(agg, f, getattr(agg, f) + getattr(c, f))
                per_update_mul8.append(c.mul8x8)
                per_update_mul16.append(c.mul16x8)
                per_update_spans.append(visible)
                poses += 1

    spans = sum(per_update_spans)
    print(f"poses sampled            {poses}")
    print(f"visible spans/update     mean={stt.mean(per_update_spans):.2f}  "
          f"(cross-check: span_decode_workload.py reports 4.30)\n")

    print("path taken, per visible span:")
    print(f"  wall_d_q4  cardinal      {agg.wall_cardinal/spans:6.1%}   "
          f"(one Q4 subtraction, zero multiplies)")
    print(f"  wall_d_q4  general       {agg.wall_general/spans:6.1%}   "
          f"(4 multiplies)")
    print(f"  inv_for_dq4 near-clamp   {agg.invd_near_clamp/spans:6.1%}   "
          f"(no multiply)")
    print(f"  inv_for_dq4 far-clamp    {agg.invd_far_clamp/spans:6.1%}   "
          f"(no multiply)")
    print(f"  inv_for_dq4 interpolate  {agg.invd_interp/spans:6.1%}   "
          f"(1 multiply)")
    print(f"  inv_at_invd cardinal     {agg.invat_cardinal/(2*spans):6.1%}   "
          f"(dot is a raw trig byte)")
    print(f"  inv_at_invd general      {agg.invat_general/(2*spans):6.1%}   "
          f"(2 extra multiplies)")

    m8 = stt.mean(per_update_mul8)
    m16 = stt.mean(per_update_mul16)
    print(f"\nmultiplies per update:")
    print(f"  8x8  -> 16   {m8:6.2f}")
    print(f"  16x8 -> 24   {m16:6.2f}")
    print(f"  table reads  {agg.tab/poses:6.2f}")

    # Costing. The 8-iteration shift-add loop is the 6-iteration loop measured
    # in z80_bearing_bench.py plus two more iterations at its measured
    # per-iteration cost; both numbers come from that cycle-exact kernel, not
    # from an instruction hand-count.
    MUL6 = 324.0                      # measured: 6-iteration signed shift-add
    PER_ITER = 54.0                   # measured: rrca+jp+add+sla+rl+djnz
    MUL8 = MUL6 + 2 * PER_ITER        # 8-bit multiplier -> 8 iterations
    MUL16 = MUL8 + 2 * 11.0           # 16-bit multiplicand: add hl,de stays 11 T
    OVERHEAD = 1.35                   # measured ratio of whole-kernel T to its
                                      # multiply loops, from z80_bearing_bench
    solve = (m8 * MUL8 + m16 * MUL16) * OVERHEAD
    print(f"\n=== COLUMN-SOLVE T-STATE PROJECTION ===")
    print(f"unit costs, derived from the CYCLE-EXACT bearing kernel's measured")
    print(f"multiply loop (324 T for 6 iterations, 54 T/iteration):")
    print(f"  8x8  multiply   {MUL8:.0f} T")
    print(f"  16x8 multiply   {MUL16:.0f} T")
    print(f"  non-multiply overhead factor  {OVERHEAD:.2f}x  "
          f"(branches, table reads, staging)")
    print(f"\n  column-solve   {solve:,.0f} T/update   [PROJECTED from measured")
    print(f"                              op counts x measured op costs -")
    print(f"                              NOT yet a cycle-exact kernel]")

    bearing, decode_clip, gate, emit = 13674.0, 11036.0, 2339.0, 21756.0
    tot = bearing + decode_clip + gate + emit + solve
    print(f"\n=== WHOLE-UPDATE BUDGET ===")
    print(f"  bearing lookup (cached) {bearing:9,.0f} T   [cycle-exact]")
    print(f"  decode-clip             {decode_clip:9,.0f} T   [cycle-exact]")
    print(f"  GATE                    {gate:9,.0f} T   [cycle-exact]")
    print(f"  column-solve            {solve:9,.0f} T   [projected]")
    print(f"  emit                    {emit:9,.0f} T   [cycle-exact]")
    print(f"  ------------------------------------")
    print(f"  TOTAL                   {tot:9,.0f} T")
    frame = 59736.0
    print(f"\nframe at 59.9 Hz = {frame:,.0f} T; VBlank alone = 15,960 T")
    print(f"  updates per frame   {frame/tot:.2f}")
    print(f"  update rate         {59.9*frame/tot:.1f} Hz if nothing else ran")


if __name__ == "__main__":
    main()
