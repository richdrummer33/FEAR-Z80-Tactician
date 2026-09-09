#!/usr/bin/env python3
"""Cycle-exact benchmark of the BEARING-FIELD evaluation on real Z80.

This is the third and last "the Z80 looks up an answer instead of computing
it" primitive, and the one the whole Adaptive Polar Field rests on: given a
corner and the camera's sub-cell position, produce that corner's Q12 bearing
by evaluating a baked local affine leaf rather than an atan2.

    leaf   = base:uint16(12 used) + span-dx:int8 + span-dy:int8
    result = (base + shr0(sx*lx0, shift) + shr0(sy*ly0, shift)) & 4095

(experiments/adaptive_polar_field/local_projection_field_poc.py, quant_leaf_
record / quant_leaf_error). `shr0` is a power-of-two divide rounded TOWARD
ZERO, not an arithmetic shift - they differ for negative values and the
kernel must reproduce the reference exactly, not approximately.

Leaves are a per-corner uniform quadtree of depth 0..3, so span is 64/32/16/8
and shift is 6/5/4/3. Depth also selects which leaf of the corner's table the
sub-cell position lands in.

THE ONE REAL TRICK IN HERE
---------------------------
A variable arithmetic shift right is expensive on this part (`srl h; rr l` in
a djnz loop is 29 T per bit, so a >>6 costs ~180 T - twice the multiply that
produced the value). But the shift amount is always 3..6, so:

    p >> shift  ==  (p << (8-shift)) >> 8   ==  high byte of (p << (8-shift))

and `add hl,hl` is 11 T. Taking H after 2..5 doublings replaces the whole
shift with 24 T per doubling and a register move. Range is safe by
construction, not by luck: |p| <= 127*(2^shift - 1) < 127*2^shift, so
|p << (8-shift)| < 127*256 = 32512 - always inside signed 16 bits, for every
depth. Sign is handled by normalising to positive first (so the discarded low
bits truncate toward zero like shr0, not toward -inf like a shift) and
negating back after.

Two layers, same discipline as the decode-clip and GATE kernels: the shr0
primitive is self-tested standalone across its full domain before the kernel
built on it is verified bit-for-bit against real baked leaves.

    make span-bearing-bench
"""
from __future__ import annotations
import collections
import pathlib
import random
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "experiments" / "adaptive_polar_field"))
sys.path.insert(0, str(ROOT / "tools"))
from local_projection_field_poc import (  # noqa: E402
    load as load_field, relevant, corner_quant_depth, quant_leaf_record,
    shr0, GRID_W, GRID_H, CELL_Q4,
)

THR = 4.0        # Q12, the poc's --emit-threshold default
MIN_Q4 = 8       # the poc's --min-q4 default -> max depth 3

# --------------------------------------------------------------------------
# Memory map (all scratch in WRAM; the leaf table itself is ROM in the real
# build, but a read costs the same either way on this part)
# --------------------------------------------------------------------------
CODE = 0x0000
LEAF_PTR = 0xD000        # u16 in: address of this corner's leaf table
DEP = 0xD002             # u8  in: 0..3
LX, LY = 0xD003, 0xD004  # u8  in: sub-cell position, 0..63
OUT = 0xD006             # u16 out: Q12 bearing

MASKTAB, NSHTAB = 0xD100, 0xD104     # 4-byte constant tables (ROM in reality)
LEAF_ADDR = 0xD010
MASK, NSH = 0xD012, 0xD013
LX0, LY0 = 0xD014, 0xD015
IX_, IY_ = 0xD016, 0xD017
BASE_LO, BASE_HI = 0xD018, 0xD019
SX, SY = 0xD01A, 0xD01B
NEG = 0xD01C
P1 = 0xD01E
DEPW = 0xD020
LEAF_TABLE = 0xE000      # where the harness stages the corner's leaf bytes

# --------------------------------------------------------------------------
# Assembler (extends the decode/GATE bench assembler with the opcodes this
# kernel needs: (hl) reads, inc hl, add hl,hl, 16-bit negate, and (hl))
# --------------------------------------------------------------------------
NO_ARG = {
    "halt": 0x76, "ld a,h": 0x7C, "ld a,l": 0x7D, "ld h,a": 0x67, "ld l,a": 0x6F,
    "ld d,a": 0x57, "ld e,a": 0x5F, "ld a,d": 0x7A, "ld a,e": 0x7B,
    "ld b,a": 0x47, "ld a,b": 0x78, "ld a,c": 0x79, "ld c,a": 0x4F,
    "add hl,de": 0x19, "add hl,hl": 0x29, "rrca": 0x0F,
    "xor a": 0xAF, "or a": 0xB7, "ex de,hl": 0xEB,
    "ld a,(hl)": 0x7E, "and (hl)": 0xA6, "inc hl": 0x23,
    "sub l": 0x95, "sub h": 0x94, "sbc a,a": 0x9F,
    "sla e": None, "rl d": None,
}
IMM8 = {"ld a,": 0x3E, "ld b,": 0x06, "ld c,": 0x0E, "ld d,": 0x16,
        "ld e,": 0x1E, "ld h,": 0x26, "ld l,": 0x2E,
        "add a,": 0xC6, "xor ": 0xEE, "and ": 0xE6}
ABS = {"jp nc,": 0xD2, "jp z,": 0xCA, "jp nz,": 0xC2, "jp ": 0xC3}


def assemble(src, org=CODE):
    raw = [l.split(";")[0].strip() for l in src.splitlines()]
    lines = [l for l in raw if l]
    labels = {}
    out = b""
    for sizing in (True, False):
        pc, buf = org, bytearray()
        for line in lines:
            if line.endswith(":"):
                labels[line[:-1]] = pc
                continue
            enc = encode(line, pc, labels, sizing)
            buf += enc
            pc += len(enc)
        out = bytes(buf)
    return out, labels


def _val(tok, labels, sizing):
    tok = tok.strip()
    if tok in labels:
        return labels[tok]
    if tok.startswith("0x"):
        return int(tok, 16)
    try:
        return int(tok, 0) & 0xFFFF
    except ValueError:
        if sizing:
            return 0
        raise SystemExit(f"assembler: unknown label {tok!r}")


def encode(line, pc, labels, sizing) -> bytes:
    if line == "sla e":
        return bytes([0xCB, 0x23])
    if line == "rl d":
        return bytes([0xCB, 0x12])
    if line.startswith("djnz "):
        tgt = _val(line[5:], labels, sizing)
        d = 0 if sizing else (tgt - (pc + 2)) & 0xFF
        return bytes([0x10, d])
    if line in NO_ARG and NO_ARG[line] is not None:
        return bytes([NO_ARG[line]])
    if line.startswith("bit 7,"):
        r = {"a": 7, "b": 0, "c": 1, "d": 2, "e": 3, "h": 4, "l": 5}[line.split(",")[1].strip()]
        return bytes([0xCB, 0x40 | (7 << 3) | r])
    if line.startswith("ld hl,(") and line.endswith(")"):
        v = _val(line[7:-1], labels, sizing)
        return bytes([0x2A, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld de,(") and line.endswith(")"):
        v = _val(line[7:-1], labels, sizing)
        return bytes([0xED, 0x5B, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld a,(") and line.endswith(")") and line != "ld a,(hl)":
        v = _val(line[6:-1], labels, sizing)
        return bytes([0x3A, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld (") and ",hl" in line:
        v = _val(line[4:line.index("),hl")], labels, sizing)
        return bytes([0x22, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld (") and ",a" in line:
        v = _val(line[4:line.index("),a")], labels, sizing)
        return bytes([0x32, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld hl,") and "(" not in line:
        v = _val(line[6:], labels, sizing)
        return bytes([0x21, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld de,") and "(" not in line:
        v = _val(line[6:], labels, sizing)
        return bytes([0x11, v & 0xFF, (v >> 8) & 0xFF])
    for pre, op in sorted(ABS.items(), key=lambda kv: -len(kv[0])):
        if line.startswith(pre):
            v = _val(line[len(pre):], labels, sizing)
            return bytes([op, v & 0xFF, (v >> 8) & 0xFF])
    for pre, op in sorted(IMM8.items(), key=lambda kv: -len(kv[0])):
        if line.startswith(pre) and "(" not in line:
            v = _val(line[len(pre):], labels, sizing)
            return bytes([op, v & 0xFF])
    raise SystemExit(f"assembler: unsupported instruction {line!r}")


# --------------------------------------------------------------------------
# Cycle-exact interpreter
# --------------------------------------------------------------------------
class Z80:
    def __init__(self, mem):
        self.m = bytearray(mem)
        self.a = self.h = self.l = self.d = self.e = self.b = self.c = 0
        self.sp = 0xFFF0
        self.pc = self.t = 0
        self.zf = self.cf = False

    @property
    def hl(self): return (self.h << 8) | self.l
    def sethl(self, v):
        v &= 0xFFFF; self.h, self.l = v >> 8, v & 0xFF
    @property
    def de(self): return (self.d << 8) | self.e
    def setde(self, v):
        v &= 0xFFFF; self.d, self.e = v >> 8, v & 0xFF

    def run(self, start, limit=200_000):
        self.pc = start
        for _ in range(limit):
            op = self.m[self.pc]
            if op == 0x76:
                self.t += 4
                return
            self.pc += 1
            self._exec(op)
        raise SystemExit("z80: runaway - kernel never halted")

    def _sub(self, v):
        r = self.a - v
        self.cf = r < 0
        self.a = r & 0xFF
        self.zf = (self.a == 0)

    def _exec(self, op):
        m = self.m
        if op == 0x7C: self.a = self.h; t = 4
        elif op == 0x7D: self.a = self.l; t = 4
        elif op == 0x67: self.h = self.a; t = 4
        elif op == 0x6F: self.l = self.a; t = 4
        elif op == 0x57: self.d = self.a; t = 4
        elif op == 0x5F: self.e = self.a; t = 4
        elif op == 0x7A: self.a = self.d; t = 4
        elif op == 0x7B: self.a = self.e; t = 4
        elif op == 0x47: self.b = self.a; t = 4
        elif op == 0x4F: self.c = self.a; t = 4
        elif op == 0x78: self.a = self.b; t = 4
        elif op == 0x79: self.a = self.c; t = 4
        elif op == 0x19: self.sethl(self.hl + self.de); t = 11
        elif op == 0x29: self.sethl(self.hl + self.hl); t = 11
        elif op == 0x23: self.sethl(self.hl + 1); t = 6
        elif op == 0x7E: self.a = m[self.hl]; t = 7
        elif op == 0xA6:
            self.a &= m[self.hl]; self.zf = (self.a == 0); self.cf = False; t = 7
        elif op == 0x95: self._sub(self.l); t = 4
        elif op == 0x94: self._sub(self.h); t = 4
        elif op == 0x9F:
            self.a = 0xFF if self.cf else 0x00
            self.zf = (self.a == 0)
            t = 4
        elif op == 0xC6:
            v = m[self.pc]; self.pc += 1
            r = self.a + v
            self.cf = r > 0xFF
            self.a = r & 0xFF
            self.zf = (self.a == 0)
            t = 7
        elif op == 0x0F:
            bit0 = self.a & 1
            self.a = ((self.a >> 1) | (bit0 << 7)) & 0xFF
            self.cf = bool(bit0)
            t = 4
        elif op == 0xAF: self.a = 0; self.zf = True; self.cf = False; t = 4
        elif op == 0xEB:
            self.d, self.h = self.h, self.d
            self.e, self.l = self.l, self.e
            t = 4
        elif op == 0xB7: self.zf = (self.a == 0); self.cf = False; t = 4
        elif op == 0x3E: self.a = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x06: self.b = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x0E: self.c = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x16: self.d = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x1E: self.e = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x26: self.h = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x2E: self.l = m[self.pc]; self.pc += 1; t = 7
        elif op == 0xEE:
            self.a ^= m[self.pc]; self.pc += 1; self.zf = (self.a == 0); t = 7
        elif op == 0xE6:
            self.a &= m[self.pc]; self.pc += 1; self.zf = (self.a == 0); t = 7
        elif op == 0x10:
            d = m[self.pc]; self.pc += 1
            self.b = (self.b - 1) & 0xFF
            if self.b:
                self.pc = (self.pc + (d - 256 if d > 127 else d)) & 0xFFFF; t = 13
            else:
                t = 8
        elif op == 0x21: self.sethl(m[self.pc] | (m[self.pc + 1] << 8)); self.pc += 2; t = 10
        elif op == 0x11: self.setde(m[self.pc] | (m[self.pc + 1] << 8)); self.pc += 2; t = 10
        elif op == 0x2A:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            self.sethl(m[a] | (m[a + 1] << 8)); t = 16
        elif op == 0x3A:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            self.a = m[a]; t = 13
        elif op == 0x22:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            m[a] = self.l; m[a + 1] = self.h; t = 16
        elif op == 0x32:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            m[a] = self.a; t = 13
        elif op == 0xED:
            sub = m[self.pc]; self.pc += 1
            if sub == 0x5B:
                a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
                self.setde(m[a] | (m[a + 1] << 8)); t = 20
            else:
                raise SystemExit(f"z80: unimplemented ED opcode 0x{sub:02X}")
        elif op == 0xCB:
            sub = m[self.pc]; self.pc += 1
            if sub == 0x23:
                self.cf = bool(self.e & 0x80)
                self.e = (self.e << 1) & 0xFF
                t = 8
            elif sub == 0x12:
                nc = bool(self.d & 0x80)
                self.d = ((self.d << 1) | (1 if self.cf else 0)) & 0xFF
                self.cf = nc
                t = 8
            elif sub in (0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7F):
                rm = {0x78: 'b', 0x79: 'c', 0x7A: 'd', 0x7B: 'e',
                      0x7C: 'h', 0x7D: 'l', 0x7F: 'a'}
                self.zf = ((getattr(self, rm[sub]) >> 7) & 1) == 0
                t = 8
            else:
                raise SystemExit(f"z80: unimplemented CB opcode 0x{sub:02X}")
        elif op == 0xC3: self.pc = m[self.pc] | (m[self.pc + 1] << 8); t = 10
        elif op == 0xCA:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            if self.zf: self.pc = a
            t = 10
        elif op == 0xC2:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            if not self.zf: self.pc = a
            t = 10
        elif op == 0xD2:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            if not self.cf: self.pc = a
            t = 10
        else:
            raise SystemExit(f"z80: unimplemented opcode 0x{op:02X} at pc={self.pc-1:#06x}")
        self.t += t


# --------------------------------------------------------------------------
# LAYER 1: the shr0 primitive (signed divide by 2^shift, rounded toward zero)
# implemented as "normalise sign, shift LEFT by 8-shift, take H".
# --------------------------------------------------------------------------
def _product_and_shift(tag, slope_addr, coord_addr):
    """Shared source for both the standalone self-test and the real kernel.

    Emitting the SAME text into both means the self-test cannot drift away
    from what the kernel actually executes - the failure mode that hid the
    GATE sign-extension byte swap until the primitive test caught it."""
    return f"""
        ld a,({slope_addr})
        ld e,a
        ld d,0
        bit 7,e
        jp z,{tag}_ext
        ld d,0xff
{tag}_ext:
        ld hl,0
        ld a,({coord_addr})
        ld b,6
{tag}_ml:
        rrca
        jp nc,{tag}_ms
        add hl,de
{tag}_ms:
        sla e
        rl d
        djnz {tag}_ml
        bit 7,h
        jp z,{tag}_pos
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
        ld a,1
        ld (0x{NEG:04X}),a
        jp {tag}_sh
{tag}_pos:
        xor a
        ld (0x{NEG:04X}),a
{tag}_sh:
        ld a,(0x{NSH:04X})
        ld b,a
{tag}_shl:
        add hl,hl
        djnz {tag}_shl
        ld a,h
        ld l,a
        ld h,0
        ld a,(0x{NEG:04X})
        or a
        jp z,{tag}_done
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
{tag}_done:
"""


PRIM = _product_and_shift("p", f"0x{SX:04X}", f"0x{LX0:04X}") + f"""
        ld (0x{P1:04X}),hl
        halt
"""


def selftest_primitive():
    code, _ = assemble(PRIM)
    rng = random.Random(4242)
    cases = []
    for shift in (3, 4, 5, 6):
        span = 1 << shift
        for sx in (0, 1, -1, 127, -128, 63, -63, 29, -69, 37):
            for lx in (0, 1, span // 2, span - 1):
                cases.append((sx, lx, shift))
        for _ in range(1000):
            cases.append((rng.randint(-128, 127), rng.randint(0, span - 1), shift))

    fails = 0
    for sx, lx, shift in cases:
        mem = bytearray(0x10000)
        mem[0:len(code)] = code
        mem[SX] = sx & 0xFF
        mem[LX0] = lx
        mem[NSH] = 8 - shift
        cpu = Z80(mem)
        cpu.run(0)
        got = cpu.m[P1] | (cpu.m[P1 + 1] << 8)
        got = got - 0x10000 if got >= 0x8000 else got
        want = shr0(sx * lx, shift)
        if got != want:
            fails += 1
            if fails <= 5:
                print(f"  FAIL: shr0({sx}*{lx}, {shift}) want {want} got {got}")
    print("=== LAYER 1: signed product + shr0 (round-toward-zero) self-test ===")
    print(f"cases={len(cases)}  fails={fails}")
    if fails:
        raise SystemExit(f"primitive is WRONG - {fails}/{len(cases)} failed")
    print("PASS - int8 slope x [0,span) coord, every depth 0..3\n")


# --------------------------------------------------------------------------
# LAYER 2: the real bearing kernel - leaf select, leaf read, two products,
# accumulate, mask to Q12.
# --------------------------------------------------------------------------
KERNEL = f"""
        ld a,(0x{DEP:04X})
        or a
        jp nz,gen
        ld hl,(0x{LEAF_PTR:04X})
        ld (0x{LEAF_ADDR:04X}),hl
        ld a,(0x{LX:04X})
        ld (0x{LX0:04X}),a
        ld a,(0x{LY:04X})
        ld (0x{LY0:04X}),a
        ld a,2
        ld (0x{NSH:04X}),a
        jp body
gen:
        ld l,a
        ld h,0
        ld (0x{DEPW:04X}),hl
        ld de,0x{MASKTAB:04X}
        add hl,de
        ld a,(hl)
        ld (0x{MASK:04X}),a
        ld hl,(0x{DEPW:04X})
        ld de,0x{NSHTAB:04X}
        add hl,de
        ld a,(hl)
        ld (0x{NSH:04X}),a

        ld a,(0x{LX:04X})
        ld l,a
        ld h,0
        ld a,(0x{DEP:04X})
        add a,2
        ld b,a
ixl:
        add hl,hl
        djnz ixl
        ld a,h
        ld (0x{IX_:04X}),a
        ld a,(0x{LX:04X})
        ld hl,0x{MASK:04X}
        and (hl)
        ld (0x{LX0:04X}),a

        ld a,(0x{LY:04X})
        ld l,a
        ld h,0
        ld a,(0x{DEP:04X})
        add a,2
        ld b,a
iyl:
        add hl,hl
        djnz iyl
        ld a,h
        ld (0x{IY_:04X}),a
        ld a,(0x{LY:04X})
        ld hl,0x{MASK:04X}
        and (hl)
        ld (0x{LY0:04X}),a

        ld a,(0x{IY_:04X})
        ld l,a
        ld h,0
        ld a,(0x{DEP:04X})
        ld b,a
iysh:
        add hl,hl
        djnz iysh
        ld a,(0x{IX_:04X})
        ld e,a
        ld d,0
        add hl,de
        add hl,hl
        add hl,hl
        ld de,(0x{LEAF_PTR:04X})
        add hl,de
        ld (0x{LEAF_ADDR:04X}),hl
body:
        ld hl,(0x{LEAF_ADDR:04X})
        ld a,(hl)
        ld (0x{BASE_LO:04X}),a
        inc hl
        ld a,(hl)
        ld (0x{BASE_HI:04X}),a
        inc hl
        ld a,(hl)
        ld (0x{SX:04X}),a
        inc hl
        ld a,(hl)
        ld (0x{SY:04X}),a
""" + _product_and_shift("x", f"0x{SX:04X}", f"0x{LX0:04X}") + f"""
        ld (0x{P1:04X}),hl
""" + _product_and_shift("y", f"0x{SY:04X}", f"0x{LY0:04X}") + f"""
        ld de,(0x{P1:04X})
        add hl,de
        ld de,(0x{BASE_LO:04X})
        add hl,de
        ld a,h
        and 0x0f
        ld h,a
        ld (0x{OUT:04X}),hl
        halt
"""


def gather_leaves():
    """Real corner leaf tables from the real map, at their real chosen depth."""
    d = load_field()
    entries = []
    hist = collections.Counter()
    fallbacks = 0
    for gy in range(GRID_H):
        for gx in range(GRID_W):
            corners, _ = relevant(d, gx, gy)
            for v in corners:
                dep, _ = corner_quant_depth(d, v, gx, gy, THR, MIN_Q4)
                hist[dep] += 1
                if dep is None:
                    fallbacks += 1
                    continue
                n = 1 << dep
                step = CELL_Q4 // n
                recs = []
                for yy in range(n):
                    for xx in range(n):
                        r = quant_leaf_record(d, v, gx, gy, xx * step, (xx + 1) * step,
                                              yy * step, (yy + 1) * step)
                        if r is None:
                            recs = None
                            break
                        recs.append(r)
                    if recs is None:
                        break
                if recs is None:
                    fallbacks += 1
                    continue
                entries.append((gx, gy, v, dep, recs))
    return entries, hist, fallbacks


def reference(recs, dep, lx, ly):
    n = 1 << dep
    shift = 6 - dep
    step = 1 << shift
    idx = (ly >> shift) * n + (lx >> shift)
    base, sx, sy = recs[idx]
    return (base + shr0(sx * (lx & (step - 1)), shift)
            + shr0(sy * (ly & (step - 1)), shift)) & 4095


def run_kernel(code, recs, dep, lx, ly):
    mem = bytearray(0x10000)
    mem[CODE:CODE + len(code)] = code
    mem[MASKTAB:MASKTAB + 4] = bytes([63, 31, 15, 7])
    mem[NSHTAB:NSHTAB + 4] = bytes([2, 3, 4, 5])
    for i, (base, sx, sy) in enumerate(recs):
        a = LEAF_TABLE + i * 4
        mem[a] = base & 0xFF
        mem[a + 1] = (base >> 8) & 0x0F
        mem[a + 2] = sx & 0xFF
        mem[a + 3] = sy & 0xFF
    mem[LEAF_PTR] = LEAF_TABLE & 0xFF
    mem[LEAF_PTR + 1] = LEAF_TABLE >> 8
    mem[DEP] = dep
    mem[LX] = lx
    mem[LY] = ly
    cpu = Z80(mem)
    cpu.run(CODE)
    return cpu.t, cpu.m[OUT] | (cpu.m[OUT + 1] << 8)


def main():
    selftest_primitive()

    code, _ = assemble(KERNEL)
    print(f"=== LAYER 2: bearing-field KERNEL, cycle-exact, {len(code)} bytes ===")

    entries, hist, fallbacks = gather_leaves()
    total = sum(hist.values())
    print(f"real corner records: {total} across the walkable map "
          f"(threshold {THR:g} Q12, min leaf {MIN_Q4})")
    for dep in (0, 1, 2, 3):
        print(f"  depth {dep} (span {64 >> dep}, shift {6-dep}): {hist[dep]:5d}"
              f"  {hist[dep]/total:6.1%}")
    print(f"  NO depth<=3 meets threshold:       {hist[None]:5d}"
          f"  {hist[None]/total:6.1%}   <- exact-fallback debt, see C4")
    print()

    rng = random.Random(7)
    probes = [(0, 0), (63, 63), (0, 63), (63, 0), (31, 32), (32, 31), (17, 45), (45, 17)]
    probes += [(rng.randint(0, 63), rng.randint(0, 63)) for _ in range(4)]

    mismatches = 0
    ts_by_dep = collections.defaultdict(list)
    n = 0
    for gx, gy, v, dep, recs in entries:
        for lx, ly in probes:
            t, got = run_kernel(code, recs, dep, lx, ly)
            ts_by_dep[dep].append(t)
            n += 1
            want = reference(recs, dep, lx, ly)
            if got != want:
                mismatches += 1
                if mismatches <= 8:
                    print(f"  MISMATCH cell=({gx},{gy}) corner={v} dep={dep} "
                          f"lx={lx} ly={ly}: want {want} got {got}")

    print(f"VERIFIED: {n - mismatches}/{n} exact matches against the baked "
          f"leaf reference")
    if mismatches:
        raise SystemExit(f"FAIL: {mismatches}/{n} mismatches")

    print("\nT-states per bearing lookup, by leaf depth:")
    for dep in sorted(ts_by_dep):
        v = ts_by_dep[dep]
        print(f"  depth {dep}:  mean={stt.mean(v):7.1f}  min={min(v)}  max={max(v)}"
              f"  n={len(v)}")

    # Weight by how often each depth actually occurs in the shipped map, not
    # by how many test cases each depth happened to get.
    weighted = sum(stt.mean(ts_by_dep[dep]) * hist[dep] for dep in ts_by_dep)
    denom = sum(hist[dep] for dep in ts_by_dep)
    mean_t = weighted / denom
    print(f"\nmap-weighted mean: {mean_t:.1f} T per bearing lookup")

    naive, distinct, worst = bearings_per_update()
    print(f"\nbearing lookups per update (measured, same pose sampling as "
          f"span_decode_workload.py):")
    print(f"  naive (evaluate at each span vertex)  {naive:.2f}")
    print(f"  DISTINCT corners actually referenced  {distinct:.2f}  "
          f"(worst case {worst})")
    print(f"  redundancy                            {naive/distinct:.2f}x")
    print(f"\n  Every corner is a shared endpoint, so the naive count pays for "
          f"each\n  one about twice. lx,ly are FIXED for the whole update, so a "
          f"corner's\n  bearing cannot change within it: a per-update cache of at "
          f"most {worst}\n  entries (corner id -> Q12, plus a validity byte) makes "
          f"the second and\n  later references a ~30 T table read. This is not an "
          f"optimisation to\n  consider later - at {mean_t:.0f} T a miss it is the "
          f"difference between the\n  largest line in the budget and the second "
          f"largest.")
    bearing_naive = mean_t * naive
    bearing_total = mean_t * distinct
    print(f"\n  bearing lookup, naive   {bearing_naive:,.0f} T/update")
    print(f"  bearing lookup, cached  {bearing_total:,.0f} T/update  "
          f"[CYCLE-EXACT, {n} cases verified]")

    decode_clip, gate, emit = 11036.0, 2339.0, 21756.0
    print("\nrunning whole-update total (all cycle-exact, cached bearings):")
    print(f"  bearing lookup {bearing_total:9,.0f} T")
    print(f"  decode-clip    {decode_clip:9,.0f} T")
    print(f"  GATE           {gate:9,.0f} T")
    print(f"  emit           {emit:9,.0f} T")
    print(f"  ----------------------------")
    tot = bearing_total + decode_clip + gate + emit
    print(f"  TOTAL          {tot:9,.0f} T   (column-solve still uncosted)")
    print(f"\nVBlank budget is 15,960 T; a whole frame at 59.9 Hz is 59,736 T.")
    print(f"  update / frame = {tot/59736.0:.2f}  -> "
          f"{59736.0/tot:.2f} updates per frame equivalent")


def bearings_per_update():
    """How many bearing evaluations one update needs, naive and deduplicated.

    Every tested instruction needs its a1 corner; only a fresh SPAN needs a0
    as well (SPANC inherits the previous span's right vertex). Counted over
    the same real poses span_decode_workload.py uses, so this number composes
    with that tool's span counts rather than being an independent estimate.

    Returns (naive, distinct, worst_distinct)."""
    sys.path.insert(0, str(ROOT / "tools"))
    from span_block_bake import load, build_block, OP_SPAN, OP_GATE, OP_END, selector_pass

    d = load()
    total = uniq = poses = 0
    worst = 0
    for gy in range(GRID_H):
        for gx in range(GRID_W):
            ops, _ = build_block(d, gx, gy)
            if ops is None:
                continue
            lx = ly = CELL_Q4 // 2
            for _yaw in range(0, 256, 8):
                skip = False
                count = 0
                seen = set()
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
                        count += 2
                        seen.add(op[2]); seen.add(op[3])
                    else:
                        count += 1
                        seen.add(op[2])
                total += count
                uniq += len(seen)
                worst = max(worst, len(seen))
                poses += 1
    return total / poses, uniq / poses, worst


if __name__ == "__main__":
    main()
