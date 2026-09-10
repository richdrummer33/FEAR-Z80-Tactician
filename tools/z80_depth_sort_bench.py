#!/usr/bin/env python3
"""Cycle-exact Z80 kernel for the MANDATORY runtime depth sort.

`make fused-host-path` established that a runtime depth sort is not optional.
Two different static baked orders - sweep-bearing and recipe order - both
produce only ~30% exact name tables, while the same key set fed through the
renderer's own far->near `inv_mid` insertion sort is 100% exact over 74,560
poses. No static per-cell order can track `inv_mid`, because `inv_mid`
depends on the camera's sub-cell position and yaw.

So the sort is a real budget line and gets measured like every other one.

WHAT IT REPRODUCES
------------------
`insert_run()` (src/tilesector_polar_renderer.c:520). Runs arrive one at a
time and each is insertion-sorted into `g_run_order` by `inv_mid`:

    i = count
    while (i > 0 && inv_mid[order[i-1]] > inv_mid[idx]) { order[i] = order[i-1]; --i; }
    order[i] = idx

The `>` is strict, so equal `inv_mid` values keep insertion order. That
tie-breaking is not incidental - getting it wrong is exactly what cost the
bearing-ordered bake 3.2% of its poses, so the kernel must reproduce it
rather than merely "sort by depth".

ORACLE
------
`tools/fused_block_render.c` dumps, per pose, the `inv_mid` values in
insertion order and the final `g_run_order` the SHIPPED code produced. The
kernel is verified against that array element-by-element, so it is checked
against the real sort's output including its tie behaviour, not against a
re-statement of the rule.

    make depth-sort-bench
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80  # noqa: E402

CODE = 0x0000
IMID = 0xC000      # inv_mid[idx], indexed by run index (insertion order)
ORDER = 0xC100     # the order array being built
CNT = 0xC200       # number of runs to insert

# ---------------------------------------------------------------------------
# The kernel. One pass over the runs, each insertion-sorted into ORDER.
#
# Register plan: C = idx (run being inserted), B = i (insert position),
# E = inv_mid[idx] held across the whole inner loop so the hot compare is a
# register-to-register `cp`, HL = &ORDER[i]. The array is bytes, so shifting
# down is `ld a,(hl) / inc hl / ld (hl),a` walking upward - no 16-bit work in
# the inner loop at all.
# ---------------------------------------------------------------------------
SRC = f"""
        ld a,({CNT:#06x})
        or a
        jp z,done
        ld c,0                       ; C = idx
outer:
        ld a,c
        ld l,a
        ld h,{IMID >> 8:#04x}
        ld e,(hl)                    ; E = inv_mid[idx], held for the scan
        ld b,c                       ; B = i = count (== idx here)
inner:
        ld a,b
        or a
        jp z,place                   ; i == 0 -> insert at front
        dec a
        ld l,a
        ld h,{ORDER >> 8:#04x}
        ld a,(hl)                    ; A = order[i-1]
        ld l,a
        ld h,{IMID >> 8:#04x}
        ld a,(hl)                    ; A = inv_mid[order[i-1]]
        cp e
        jp z,place                   ; EQUAL -> stop: ties keep insertion order
        jp c,place                   ; less   -> stop
        ; strictly greater: shift order[i-1] up into order[i]
        ld a,b
        dec a
        ld l,a
        ld h,{ORDER >> 8:#04x}
        ld a,(hl)
        inc l
        ld (hl),a
        dec b
        jp inner
place:
        ld a,b
        ld l,a
        ld h,{ORDER >> 8:#04x}
        ld (hl),c
        inc c
        ld a,({CNT:#06x})
        cp c
        jp nz,outer
done:
        halt
"""


def load_cases(path, limit):
    """Stride the whole file rather than taking its head.

    The oracle is written in map-scan order, so the first N lines are one
    corner of the map. Taking the head would sample a spatial cluster - the
    same class of coverage hole recorded in docs/TODO_DEFERRED.md A16, where a
    convenient-looking sample silently excluded an entire code path."""
    lines = [l for l in open(path) if l.strip()]
    step = max(1, len(lines) // limit)
    cases = []
    for line in lines[::step]:
        v = line.split()
        n = int(v[0])
        cases.append((n, [int(x) for x in v[1:1 + n]],
                      [int(x) for x in v[1 + n:1 + 2 * n]]))
    return cases


def main():
    dump = ROOT / "build" / "sort_oracle.txt"
    if not dump.exists():
        raise SystemExit(f"missing {dump} - run `make fused-host-path` first")
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
    cases = load_cases(dump, limit)

    code, _ = assemble(SRC, CODE)
    print(f"=== Z80 DEPTH-SORT KERNEL (insert_run), {len(code)} bytes ===")
    print(f"oracle cases: {len(cases)} poses strided across all "
          f"{sum(1 for _ in open(dump))}, from the shipped insert_run's own output\n")

    base = bytearray(0x10000)
    base[CODE:CODE + len(code)] = code

    fails = 0
    ts = []
    by_n = {}
    for n, imid, want in cases:
        mem = bytearray(base)
        mem[IMID:IMID + n] = bytes(imid)
        mem[CNT] = n
        cpu = Z80(mem)
        cpu.run(CODE)
        got = list(cpu.m[ORDER:ORDER + n])
        ts.append(cpu.t)
        by_n.setdefault(n, []).append(cpu.t)
        if got != want:
            fails += 1
            if fails <= 5:
                print(f"  MISMATCH n={n} inv_mid={imid}")
                print(f"    want {want}")
                print(f"    got  {got}")
    print(f"VERIFIED: {len(cases) - fails}/{len(cases)} exact - the final "
          f"run order matches the shipped insert_run,")
    print(f"          including its equal-inv_mid tie-breaking")
    if fails:
        raise SystemExit(f"FAIL: {fails} mismatches")

    mean_t = stt.mean(ts)
    print(f"\nT-states per update: mean={mean_t:.1f}  min={min(ts)}  max={max(ts)}")
    print("\nby run count:")
    for n in sorted(by_n):
        v = by_n[n]
        print(f"  n={n:2d}  mean={stt.mean(v):7.1f}  poses={len(v):6d}")

    bearing, decode_clip, gate, colsolve, emit = 5833.0, 11036.0, 2339.0, 26820.0, 21756.0
    tot = bearing + decode_clip + gate + colsolve + emit + mean_t
    frame = 59736.0
    print(f"\n=== WHOLE-UPDATE BUDGET (every line measured) ===")
    print(f"  bearing lookup (A12)  {bearing:9,.0f} T")
    print(f"  decode-clip           {decode_clip:9,.0f} T")
    print(f"  GATE                  {gate:9,.0f} T")
    print(f"  column-solve          {colsolve:9,.0f} T")
    print(f"  DEPTH SORT (new)      {mean_t:9,.0f} T   <- mandatory, and cheap")
    print(f"  emit                  {emit:9,.0f} T")
    print(f"  ------------------------------------")
    print(f"  TOTAL                 {tot:9,.0f} T   {frame/tot:.2f} updates/frame")
    print(f"\n  The sort costs {mean_t/tot:.2%} of the update. It was the last")
    print(f"  correctness unknown in the pipeline and it is budget noise.")


if __name__ == "__main__":
    main()
