#!/usr/bin/env python3
"""Exact A/B for the two repeated-add loops still present in DPSOLVE.

Baseline DPSOLVE computes:
  1) iq += (c0-10) * step
  2) endq = iq + n * step
with runtime DJNZ loops over 16-bit adds/subtracts.

This bench tests finite-domain jump-table kernels for (2) alone and for both
(1)+(2).  The arithmetic is unchanged: each case is an unrolled sequence of
the same ADD HL,DE or OR A / SBC HL,DE operations.  Every variant is checked
against the existing target_solve_oracle corpus.
"""
from __future__ import annotations

import pathlib
import statistics as stt
import sys

import z80_column_solve_bench as cs
import z80_target_solve_bench as ds
from z80core import assemble, Z80

ROOT = pathlib.Path(__file__).resolve().parents[1]
WALKTAB = 0xD100              # 20 x u16 code addresses, indexed by c0 0..19
ENDTAB = 0xD140               # 21 x u16; entry 0 unused, n is 1..20


def unrolled_add(n: int) -> str:
    return "".join("        add hl,de\n" for _ in range(n))


def unrolled_sub(n: int) -> str:
    # SBC consumes carry, so clear it before every subtraction exactly as the
    # baseline loop's OR A does on every iteration.
    return "".join("        or a\n        sbc hl,de\n" for _ in range(n))


def end_jump_block() -> str:
    cases = []
    for n in range(1, 21):
        cases.append(f"dp_end_{n}:\n{unrolled_add(n)}        jp dp_end_done\n")
    return f"""; ---- endq = iq + n*step, finite n=1..20 ----
        ld a,({cs.NCOL:#06x})
        add a,a
        ld l,a
        ld h,0
        ld de,{ENDTAB:#06x}
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ex de,hl
        jp (hl)
{''.join(cases)}dp_end_done:
        ld ({ds.ENDQ:#06x}),hl
"""


def walk_jump_block() -> str:
    cases = []
    for c0 in range(20):
        d = c0 - 10
        op = unrolled_add(d) if d > 0 else unrolled_sub(-d)
        cases.append(
            f"dp_walk_{c0}:\n"
            f"        ld hl,({cs.IQ:#06x})\n"
            f"        ld de,({cs.STEP:#06x})\n"
            f"{op}"
            f"        jp dp_walked\n")
    return f"""; ---- walk iq from reference column 10 to c0, finite c0=0..19 ----
        ld a,({cs.C0:#06x})
        add a,a
        ld l,a
        ld h,0
        ld de,{WALKTAB:#06x}
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ex de,hl
        jp (hl)
{''.join(cases)}dp_walked:
        ld ({cs.IQ:#06x}),hl
"""


def replace_end(src: str) -> str:
    old = f"""; ---- endq = iq + n*step ----
        ld a,({cs.NCOL:#06x})
        ld b,a
dp_end:
        add hl,de
        djnz dp_end
        ld ({ds.ENDQ:#06x}),hl
"""
    if old not in src:
        raise RuntimeError("DPSOLVE endq loop changed; refusing dishonest A/B")
    return src.replace(old, end_jump_block(), 1)


def replace_walk(src: str) -> str:
    old = f"""; ---- walk iq from column 10 to c0 ----
        ld hl,({cs.IQ:#06x})
        ld de,({cs.STEP:#06x})
        ld a,({cs.C0:#06x})
        cp 10
        jp nc,dp_fwd
        ld b,a
        ld a,10
        sub b
        ld b,a                       ; 10 - c0, always >= 1 here
dp_back:
        or a
        sbc hl,de
        djnz dp_back
        jp dp_walked
dp_fwd:
        sub 10
        jp z,dp_walked               ; c0 == 10: djnz with B=0 would loop 256x
        ld b,a
dp_f:
        add hl,de
        djnz dp_f
dp_walked:
        ld ({cs.IQ:#06x}),hl
"""
    if old not in src:
        raise RuntimeError("DPSOLVE c0 walk changed; refusing dishonest A/B")
    return src.replace(old, walk_jump_block(), 1)


def wr16(mem, a, v):
    mem[a] = v & 0xFF
    mem[a + 1] = (v >> 8) & 0xFF


def populate_jump_tables(mem, labels, walk: bool, end: bool):
    if walk:
        for c0 in range(20):
            wr16(mem, WALKTAB + 2*c0, labels[f"dp_walk_{c0}"])
    if end:
        for n in range(1, 21):
            wr16(mem, ENDTAB + 2*n, labels[f"dp_end_{n}"])


def compile_variant(src, walk=False, end=False):
    code, labels = assemble(src, cs.CODE)
    return code, labels, walk, end


def rd16(mem, a):
    v = mem[a] | (mem[a + 1] << 8)
    return v - 0x10000 if v >= 0x8000 else v


def main():
    T = ds.load_tables()
    base_src = ds.build_target(cs.SRC)
    variants = {
        "baseline": compile_variant(base_src),
        "end finite": compile_variant(replace_end(base_src), end=True),
        "walk+end finite": compile_variant(replace_walk(replace_end(base_src)), walk=True, end=True),
    }

    dump = ROOT / "build" / "target_solve_oracle.txt"
    rows = [l.split() for l in dump.read_text().splitlines() if l.strip()]
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    if limit and len(rows) > limit:
        step = max(1, len(rows) // limit)
        rows = rows[::step]

    base_mem = cs.build_mem(T)
    for i, v in enumerate(T["cls"]):
        base_mem[ds.CLSTAB + i] = v & 0xFF

    stats = {k: [] for k in variants}
    fails = {k: 0 for k in variants}
    c0hist = [0] * 20
    nhist = [0] * 21

    for r in rows:
        (px, py, yaw, sid, invd, c0, c1, n, mid, iq, stp, lo, hi) = (int(v) for v in r)
        if 0 <= c0 < 20:
            c0hist[c0] += 1
        if 0 <= n <= 20:
            nhist[n] += 1
        want = (invd, c0, c1, n, mid, iq, stp)

        for name, (code, labels, use_walk, use_end) in variants.items():
            mem = bytearray(base_mem)
            mem[cs.CODE:cs.CODE + len(code)] = code
            populate_jump_tables(mem, labels, use_walk, use_end)
            for i in range(7):
                mem[ds.NFTAB + i] = T["nf"][i][yaw] & 0xFF
                mem[ds.SFTAB + i] = T["sf"][i][yaw] & 0xFF
            mem[cs.SID] = sid
            cs.w16(mem, cs.XQ4, px); cs.w16(mem, cs.YQ4, py)
            cs.w16(mem, cs.YAWQ, (yaw << 4) & 0xFFFF)
            cs.w16(mem, cs.LO, lo & 0xFFFF); cs.w16(mem, cs.HI, hi & 0xFFFF)
            cpu = Z80(mem); cpu.run(cs.CODE)
            stats[name].append(cpu.t)
            got = (cpu.m[cs.INVD], cpu.m[cs.C0], cpu.m[cs.C1], cpu.m[cs.NCOL],
                   cpu.m[ds.INVMID], rd16(cpu.m, cs.IQ), rd16(cpu.m, cs.STEP))
            if got != want:
                fails[name] += 1
                if fails[name] <= 2:
                    print(f"MISMATCH {name} ({px},{py},yaw={yaw}) want={want} got={got}")

    print("=== DPSOLVE FINITE SMALL-MULTIPLIER A/B ===")
    print(f"oracle rows                 {len(rows):,}")
    print("\nC0 DISTRIBUTION (top 8)")
    for c0, n in sorted(enumerate(c0hist), key=lambda x: x[1], reverse=True)[:8]:
        print(f"  c0={c0:2d}  {n:6,}  {100.0*n/max(len(rows),1):5.1f}%")
    print("\nRUN-LENGTH DISTRIBUTION (top 8)")
    for n, k in sorted(enumerate(nhist), key=lambda x: x[1], reverse=True)[:8]:
        if not k: continue
        print(f"  n={n:2d}   {k:6,}  {100.0*k/max(len(rows),1):5.1f}%")

    print("\nRESULT")
    bmean = stt.mean(stats["baseline"])
    bcode = len(variants["baseline"][0])
    for name in variants:
        mean = stt.mean(stats[name])
        code = len(variants[name][0])
        print(f"  {name:16} {mean:8.1f} T/span  code {code:4d} B  "
              f"delta {mean-bmean:+7.1f} T  fails {fails[name]}")
    print("\nAT 4.30 VISIBLE SPANS / UPDATE")
    for name in variants:
        mean = stt.mean(stats[name])
        print(f"  {name:16} {mean*4.30:9.0f} T/update")

    if any(fails.values()):
        return 1
    best = min(stats, key=lambda k: stt.mean(stats[k]))
    print(f"\nVERDICT: {best} is fastest; all variants oracle-EXACT.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
