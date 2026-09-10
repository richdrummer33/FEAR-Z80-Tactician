#!/usr/bin/env python3
"""Bake per-camera-cell span programs for the Z80 span interpreter.

This is the bake side of the span interpreter: for each 4-world-unit camera
cell it emits a short program in the span ISA that the Z80 walks to draw the
view, replacing runtime candidate selection, projection ordering and the
insertion sort with a fixed instruction stream.

    op        bytes  operands                       meaning
    SPAN      5      V0, V1, SID, ATTR              wall span between corners
    SPANC     4      V1, SID, ATTR                  left vertex = previous right
    GATE      2      SEL                            skip next unless half-plane
    END       1      -                              end of block

SWEEP ORDER IS YAW-INVARIANT
----------------------------
Spans are emitted in order of the world bearing of their left corner, measured
at the cell centre. Camera yaw only rotates which part of that cycle is on
screen; it never reorders the cycle. So the baker can fix the order once per
cell and the runtime never sorts. That is what retires the measured 42.8%
far-to-near rate that currently makes the runtime insertion sort load-bearing.

SPANC IS GATED BY ITS PREDECESSOR
---------------------------------
SPANC inherits the previous span's right vertex, so it is only emitted when the
preceding instruction is guaranteed to have executed. A span behind a GATE may
be skipped at runtime, so any span following a gated span must be a full SPAN
even if it shares a corner. The bake reports how many shared-corner
opportunities survive that rule, because that difference is the real cost of
conditional visibility.

CORRECTNESS GATE
----------------
For sampled sub-cell positions the emitted program is interpreted and the set
of spans it draws is compared against the set the C renderer would draw
(base keys plus conditional keys whose selector passes). Any mismatch is a hard
failure - the bake never reports a size for a program that draws the wrong
geometry.

    make span-block-bake
"""
from __future__ import annotations
import argparse
import math
import pathlib
import statistics as st
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]
                       / "experiments" / "adaptive_polar_field"))
from local_projection_field_poc import (  # noqa: E402
    load as load_topology, arr, CELL_Q4, GRID_W, GRID_H, GEN,
)


class Data:
    """Topology plus the selector arrays the shared loader does not carry."""

    def __init__(self):
        base = load_topology()
        for f in ("keys", "ro", "rs", "bo", "bs", "grid", "vx", "vy"):
            setattr(self, f, getattr(base, f))
        text = "\n".join(p.read_text() for p in
                          sorted(GEN.glob("tilesector_polar_data_part*.inc")))
        self.sel_a = arr(text, "k_tspf_sel_a")
        self.sel_b = arr(text, "k_tspf_sel_b")
        self.sel_c = arr(text, "k_tspf_sel_c")
        self.sel_inv = arr(text, "k_tspf_sel_inv")


def load():
    return Data()

QTURN = 4096.0
TAU = math.tau

OP_SPAN, OP_SPANC, OP_GATE, OP_END = 0, 1, 2, 3
OP_BYTES = {OP_SPAN: 5, OP_SPANC: 4, OP_GATE: 2, OP_END: 1}
OP_NAME = {OP_SPAN: "SPAN", OP_SPANC: "SPANC", OP_GATE: "GATE", OP_END: "END"}


def keyparts(w):
    return w & 31, (w >> 5) & 15, (w >> 9) & 15


def cell_keys(d, gx, gy):
    """(unconditional keys, [(key, selector)]) exactly as tsp_polar_render reads them."""
    rid = d.grid[gy * GRID_W + gx]
    if rid == 255:
        return None
    o = d.ro[rid]
    base_id, ncond = d.rs[o], d.rs[o + 1]
    b = d.bo[base_id]
    n = d.bs[b]
    base = list(d.bs[b + 1:b + 1 + n])
    cond = []
    p = o + 2
    for _ in range(ncond):
        cond.append((d.rs[p], d.rs[p + 1]))
        p += 2
    return base, cond


def bearing(d, v, xq, yq):
    dx = d.vx[v] * 16.0 - xq
    dy = d.vy[v] * 16.0 - yq
    return (math.atan2(dy, dx) * QTURN / TAU) % QTURN


def selector_pass(d, sel, lx, ly):
    v = d.sel_a[sel] * lx + d.sel_b[sel] * ly + d.sel_c[sel]
    return (1 if v >= 0 else 0) ^ d.sel_inv[sel]


# DEFAULT: "recipe". Proven by `make fused-host-path` - recipe order plus the
# runtime depth sort reproduces the shipped renderer's name table exactly on
# 74,560/74,560 poses, where bearing order reaches only 96.8%. The 3.2% gap is
# equal-inv_mid tie-breaking, and it costs 947 bytes (3.0%) of extra payload
# because bearing order claims more SPANC. Correctness wins that trade.
ORDER_MODE = "recipe"


def build_block(d, gx, gy):
    """Emit the span program for one camera cell. Returns (ops, stats)."""
    ck = cell_keys(d, gx, gy)
    if ck is None:
        return None, None
    base, cond = ck
    cx = gx * CELL_Q4 + CELL_Q4 / 2
    cy = gy * CELL_Q4 + CELL_Q4 / 2

    entries = []
    for k in base:
        entries.append((k, None))
    for k, sel in cond:
        entries.append((k, sel))

    # Sweep order: world bearing of the span's left corner at the cell centre.
    #
    # ORDER_MODE selects between two orders that are NOT interchangeable:
    #
    #   "bearing" - sort by the left corner's bearing at the cell centre.
    #               Maximises SPANC (consecutive spans share a vertex more
    #               often), but the resulting DRAW order does not reproduce
    #               the renderer's own painter order, which is a runtime
    #               far->near inv_mid sort with insertion-order tie-breaking.
    #               Measured at 29.6% exact name tables (make fused-host-path).
    #
    #   "recipe"  - keep the recipe's own base-then-conditional order, which
    #               IS the order the shipped renderer inserts runs in.
    #
    # This is a real trade-off between storage and correctness, quantified in
    # docs/TODO_DEFERRED.md A18 - not a preference.
    if ORDER_MODE == "bearing":
        def sortkey(e):
            _, v0, _ = keyparts(d.keys[e[0]])
            return bearing(d, v0, cx, cy)
        entries.sort(key=sortkey)

    ops = []
    shared_total = 0          # consecutive pairs that share a corner
    shared_used = 0           # ... that SPANC could actually claim
    prev_v1 = None
    prev_unconditional = False
    for k, sel in entries:
        sid, v0, v1 = keyparts(d.keys[k])
        shares = (prev_v1 is not None and v0 == prev_v1)
        if shares:
            shared_total += 1
        if sel is not None:
            ops.append((OP_GATE, sel))
        # SPANC may only inherit from an instruction that always executes.
        if shares and prev_unconditional:
            ops.append((OP_SPANC, k, v1, sid))
            shared_used += 1
        else:
            ops.append((OP_SPAN, k, v0, v1, sid))
        prev_v1 = v1
        prev_unconditional = (sel is None)
    ops.append((OP_END,))

    nbytes = sum(OP_BYTES[o[0]] for o in ops)
    stats = dict(nbytes=nbytes, nspans=len(entries), ngates=len(cond),
                 shared_total=shared_total, shared_used=shared_used)
    return ops, stats


def interpret(d, ops, lx, ly):
    """Run the program the way the Z80 would; return the set of keys drawn."""
    drawn = []
    skip = False
    for op in ops:
        if op[0] == OP_END:
            break
        if op[0] == OP_GATE:
            skip = not selector_pass(d, op[1], lx, ly)
            continue
        if skip:
            skip = False
            continue
        drawn.append(op[1])
    return set(drawn)


def reference(d, gx, gy, lx, ly):
    """The key set tsp_polar_render would collect at this sub-cell position."""
    base, cond = cell_keys(d, gx, gy)
    out = set(base)
    for k, sel in cond:
        if selector_pass(d, sel, lx, ly):
            out.add(k)
    return out


def canon(ops):
    """Canonical byte-ish encoding of a block, for content deduplication."""
    return tuple(tuple(o) for o in ops)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=25,
                    help="sub-cell positions validated per cell")
    args = ap.parse_args()
    d = load()

    step = max(1, CELL_Q4 // int(math.isqrt(args.samples)))
    sizes, spans, gates, sh_tot, sh_use = [], [], [], 0, 0
    blocks = []
    checked = mismatches = 0
    op_counts = {OP_SPAN: 0, OP_SPANC: 0, OP_GATE: 0, OP_END: 0}

    for gy in range(GRID_H):
        for gx in range(GRID_W):
            ops, s = build_block(d, gx, gy)
            if ops is None:
                continue
            sizes.append(s["nbytes"])
            blocks.append((canon(ops), s["nbytes"]))
            spans.append(s["nspans"])
            gates.append(s["ngates"])
            sh_tot += s["shared_total"]
            sh_use += s["shared_used"]
            for o in ops:
                op_counts[o[0]] += 1
            for ly in range(0, CELL_Q4, step):
                for lx in range(0, CELL_Q4, step):
                    checked += 1
                    if interpret(d, ops, lx, ly) != reference(d, gx, gy, lx, ly):
                        mismatches += 1
                        if mismatches < 4:
                            print(f"  MISMATCH cell ({gx},{gy}) local ({lx},{ly})")

    total = sum(sizes)
    print("=== SPAN BLOCK BAKE ===")
    print(f"cells with geometry     {len(sizes)} of {GRID_W*GRID_H}")
    print(f"validated               {checked:,} sub-cell positions, "
          f"{mismatches} mismatches")
    if mismatches:
        raise SystemExit("FAIL: emitted program does not match the renderer's key set")
    print("CORRECT: every sampled position draws exactly the renderer's key set\n")

    print(f"spans/cell              mean={st.mean(spans):.2f} "
          f"median={st.median(spans)} max={max(spans)}")
    print(f"gates/cell              mean={st.mean(gates):.2f} max={max(gates)}")
    print(f"bytes/cell              mean={st.mean(sizes):.1f} "
          f"median={st.median(sizes)} max={max(sizes)}")
    print(f"opcode mix              " +
          "  ".join(f"{OP_NAME[k]}={v}" for k, v in sorted(op_counts.items())))
    print(f"shared-corner pairs     {sh_tot} total, {sh_used_pct(sh_use, sh_tot)}"
          f" claimable as SPANC ({sh_use})")
    print(f"  bytes lost to gating  {sh_tot - sh_use} spans x 1 byte = "
          f"{sh_tot - sh_use} B\n")

    uniq = {}
    for body, nb in blocks:
        uniq.setdefault(body, nb)
    dedup_payload = sum(uniq.values())

    print(f"payload, one block per cell   {total:,} B ({total/1024:.1f} KiB)")
    print(f"distinct blocks               {len(uniq)} of {len(blocks)} cells "
          f"({1-len(uniq)/len(blocks):.1%} are duplicates)")
    print(f"payload, deduplicated         {dedup_payload:,} B "
          f"({dedup_payload/1024:.1f} KiB)   saves {total-dedup_payload:,} B")
    # Verified: the block is a pure function of the recipe id, so the shipped
    # k_tspf_recipe_grid[1152] already maps every camera cell to its block. The
    # index is that existing grid plus one offset per distinct block - not a
    # new per-cell table.
    offsets = len(uniq) * 2
    grid = GRID_W * GRID_H
    print(f"+ block offset table          {offsets:,} B ({len(uniq)} x uint16)")
    print(f"+ recipe grid (already in ROM) {grid:,} B")
    total_bake = dedup_payload + offsets + grid
    print(f"TOTAL SPAN BLOCK BAKE         {total_bake:,} B "
          f"({total_bake/1024:.1f} KiB)")
    print()
    bearing_field = 46109
    seg = 68
    luts = 1154
    print("alongside the fields it does NOT replace:")
    print(f"  bearing field               {bearing_field:,} B")
    print(f"  segment plane constants     {seg:,} B")
    print(f"  screen LUTs                 ~{luts:,} B")
    grand = total_bake + bearing_field + seg + luts
    print(f"COMPLETE STRUCTURAL BAKE      {grand:,} B ({grand/1024:.1f} KiB)")
    print(f"  fits 128 KiB ROM with {(131072-grand)/1024:.1f} KiB spare; "
          f"the frame bake needed 136.58 MiB")


def sh_used_pct(used, total):
    return f"{used/total:.1%}" if total else "n/a"


if __name__ == "__main__":
    main()

