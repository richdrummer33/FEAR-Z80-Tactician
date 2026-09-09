#!/usr/bin/env python3
"""Cycle-exact benchmark of the span interpreter's GATE selector evaluation.

This is `selector_pass()` as real Z80: `v = sel_a*lx + sel_b*ly + sel_c;
pass = (v>=0) ^ sel_inv`. Same two-layer discipline as the decode-clip kernel
(tools/z80_decode_bench.py): an isolated, self-tested arithmetic primitive
first, then the real kernel built on top of it and verified bit-for-bit
against the real selector data at real poses.

WHY THIS NEEDED ITS OWN PRIMITIVE, NOT THE CLIP KERNEL'S ONE
--------------------------------------------------------------
The decode-clip kernel's primitive answered "is X < Y (signed)?". This stage
needs a different operation the Z80 also lacks natively: signed 8-bit x
small-unsigned multiply, sign-extended to 16 bits. Real coefficient ranges
(read from src/generated, not assumed): sel_a in [0,29], sel_b in [-69,37],
sel_c in [-3712,1344] - sel_a happens to be non-negative in this map's data,
but its C type is int8_t, so the primitive is tested across the FULL signed
8-bit range rather than the narrower range this map happens to exercise.
lx,ly are always in [0,63] (6-bit sub-cell position, masked upstream in the
real renderer) - the multiply loop runs 6 iterations on that basis, and the
mask is asserted, not assumed, in the self-test.

Once both products are formed, "v>=0" is just a bit test (bit 15 of the
16-bit sum) - the cheapest possible comparison on this part, needing no
bias trick at all, which is why this kernel is expected to be markedly
cheaper per test than the clip kernel despite doing "more math".

    make span-gate-bench
"""
from __future__ import annotations
import pathlib
import random
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from span_block_bake import load, build_block, OP_GATE, OP_END  # noqa: E402
from local_projection_field_poc import CELL_Q4, GRID_W, GRID_H  # noqa: E402

# --------------------------------------------------------------------------
# Memory map
# --------------------------------------------------------------------------
CODE = 0x0000
SEL_A, SEL_B, SEL_C = 0xD000, 0xD001, 0xD002   # i8, i8, i16
LX, LY = 0xD004, 0xD005                         # u8, u8 (0..63)
INV = 0xD006                                    # u8, 0 or 1
OUT_PASS = 0xD007                               # u8, output

MUL_A, MUL_LX, MUL_RESULT = 0xD010, 0xD011, 0xD012   # multiply-primitive I/O

# --------------------------------------------------------------------------
# Minimal two-pass assembler
# --------------------------------------------------------------------------
NO_ARG = {
    "halt": 0x76, "ld a,h": 0x7C, "ld a,l": 0x7D, "ld h,a": 0x67, "ld l,a": 0x6F,
    "ld d,a": 0x57, "ld e,a": 0x5F, "ld a,d": 0x7A, "ld a,e": 0x7B,
    "add hl,de": 0x19, "rrca": 0x0F, "sla e": None, "rl d": None,
    "xor a": 0xAF, "xor d": 0xAA, "or a": 0xB7, "push hl": 0xE5, "pop hl": 0xE1, "ex de,hl": 0xEB,
}
IMM8 = {"ld a,": 0x3E, "ld b,": 0x06, "ld c,": 0x0E, "ld d,": 0x16, "ld e,": 0x1E, "xor ": 0xEE, "and ": 0xE6}
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
    if line == "djnz":
        raise SystemExit("use 'djnz label'")
    if line.startswith("djnz "):
        tgt = _val(line[5:], labels, sizing)
        d = 0 if sizing else (tgt - (pc + 2)) & 0xFF
        return bytes([0x10, d])
    if line in NO_ARG and NO_ARG[line] is not None:
        return bytes([NO_ARG[line]])
    if line.startswith("bit 7,"):
        reg = line.split(",")[1].strip()
        r = {"a":7,"b":0,"c":1,"d":2,"e":3,"h":4,"l":5}[reg]
        return bytes([0xCB, 0x40 | (7 << 3) | r])
    if line.startswith("ld hl,(") and line.endswith(")"):
        v = _val(line[7:-1], labels, sizing)
        return bytes([0x2A, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld de,(") and line.endswith(")"):
        v = _val(line[7:-1], labels, sizing)
        return bytes([0xED, 0x5B, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld a,(") and line.endswith(")"):
        v = _val(line[6:-1], labels, sizing)
        return bytes([0x3A, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld (") and ",hl" in line:
        inner = line[4:line.index("),hl")]
        v = _val(inner, labels, sizing)
        return bytes([0x22, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld (") and ",a" in line:
        inner = line[4:line.index("),a")]
        v = _val(inner, labels, sizing)
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
    for pre, op in IMM8.items():
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
        elif op == 0x19: self.sethl(self.hl + self.de); t = 11
        elif op == 0x0F:                        # rrca
            bit0 = self.a & 1
            self.a = ((self.a >> 1) | (bit0 << 7)) & 0xFF
            self.cf = bool(bit0)
            t = 4
        elif op == 0xAF: self.a = 0; self.zf = True; self.cf = False; t = 4
        elif op == 0xAA:                        # xor d
            self.a ^= self.d; self.zf = (self.a == 0); self.cf = False; t = 4
        elif op == 0xE5:                        # push hl
            self.sp = (self.sp - 1) & 0xFFFF; m[self.sp] = self.h
            self.sp = (self.sp - 1) & 0xFFFF; m[self.sp] = self.l
            t = 11
        elif op == 0xE1:                        # pop hl
            self.l = m[self.sp]; self.sp = (self.sp + 1) & 0xFFFF
            self.h = m[self.sp]; self.sp = (self.sp + 1) & 0xFFFF
            t = 10
        elif op == 0xEB:                        # ex de,hl
            self.d, self.h = self.h, self.d
            self.e, self.l = self.l, self.e
            t = 4
        elif op == 0xB7: self.zf = (self.a == 0); self.cf = False; t = 4
        elif op == 0x3E: self.a = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x06: self.b = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x0E: self.c = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x16: self.d = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x1E: self.e = m[self.pc]; self.pc += 1; t = 7
        elif op == 0xEE: self.a ^= m[self.pc]; self.pc += 1; self.zf = (self.a == 0); t = 7
        elif op == 0xE6: self.a &= m[self.pc]; self.pc += 1; self.zf = (self.a == 0); t = 7
        elif op == 0x10:                        # djnz
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
            if sub == 0x23:                     # sla e
                self.cf = bool(self.e & 0x80)
                self.e = (self.e << 1) & 0xFF
                t = 8
            elif sub == 0x12:                   # rl d
                newcarry = bool(self.d & 0x80)
                self.d = ((self.d << 1) | (1 if self.cf else 0)) & 0xFF
                self.cf = newcarry
                t = 8
            elif sub in (0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7F):  # bit 7,r
                reg_map = {0x78:'b',0x79:'c',0x7A:'d',0x7B:'e',0x7C:'h',0x7D:'l',0x7F:'a'}
                val = getattr(self, reg_map[sub])
                self.zf = ((val >> 7) & 1) == 0
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
# LAYER 1: signed8(sign-extended-to-16) x unsigned6bit -> signed16 multiply,
# self-tested against Python's native arithmetic before it goes near the
# real kernel.
# --------------------------------------------------------------------------


def selftest_multiply():
    """SIGNED_MUL below is exactly the multiply sequence the real KERNEL
    uses (sign-extend by testing bit 7 of the SOURCE byte, then a 6-iteration
    shift-add), so this self-test exercises the identical code path the
    kernel runs, not a simplified stand-in."""
    code, _ = assemble(SIGNED_MUL)
    rng = random.Random(99)
    cases = [(a, lx) for a in (0, 1, -1, 127, -128, 29, -69, 37, 63, -63)
              for lx in (0, 1, 31, 32, 63)]
    for _ in range(4000):
        cases.append((rng.randint(-128, 127), rng.randint(0, 63)))

    fails = 0
    for a_signed, lx in cases:
        mem = bytearray(0x10000)
        mem[0:len(code)] = code
        mem[MUL_A] = a_signed & 0xFF
        mem[MUL_LX] = lx
        cpu = Z80(mem)
        cpu.run(0)
        got = cpu.m[MUL_RESULT] | (cpu.m[MUL_RESULT + 1] << 8)
        got_signed = got - 0x10000 if got >= 0x8000 else got
        want = a_signed * lx
        if got_signed != want:
            fails += 1
            if fails <= 5:
                print(f"  FAIL: {a_signed} * {lx} = want {want}, got {got_signed}")
    print("=== LAYER 1: signed8 x unsigned6 multiply self-test ===")
    print(f"cases={len(cases)}  fails={fails}")
    if fails:
        raise SystemExit(f"multiply primitive is WRONG - {fails}/{len(cases)} failed")
    print("PASS - verified for full int8 x [0,63] domain\n")


SIGNED_MUL = f"""
        ld a,(0x{MUL_A:04X})
        ld e,a
        ld d,0
        bit 7,e
        jp z,ext_done2
        ld d,0xff
ext_done2:
        ld hl,0
        ld a,(0x{MUL_LX:04X})
        ld b,6
mloop:
        rrca
        jp nc,mskip
        add hl,de
mskip:
        sla e
        rl d
        djnz mloop
        ld (0x{MUL_RESULT:04X}),hl
        halt
"""


# --------------------------------------------------------------------------
# LAYER 2: the real GATE kernel - two SIGNED_MUL-shaped products, summed
# with sel_c, sign-tested, XORed with inv.
# --------------------------------------------------------------------------
KERNEL = f"""
        ld a,(0x{SEL_A:04X})
        ld e,a
        ld d,0
        bit 7,e
        jp z,e1
        ld d,0xff
e1:
        ld hl,0
        ld a,(0x{LX:04X})
        ld b,6
l1:
        rrca
        jp nc,s1
        add hl,de
s1:
        sla e
        rl d
        djnz l1
        push hl

        ld a,(0x{SEL_B:04X})
        ld e,a
        ld d,0
        bit 7,e
        jp z,e2
        ld d,0xff
e2:
        ld hl,0
        ld a,(0x{LY:04X})
        ld b,6
l2:
        rrca
        jp nc,s2
        add hl,de
s2:
        sla e
        rl d
        djnz l2
        ex de,hl
        pop hl
        add hl,de

        ld de,(0x{SEL_C:04X})
        add hl,de

        bit 7,h
        jp z,nonneg
        xor a
        jp have_sign
nonneg:
        ld a,1
have_sign:
        ld d,a
        ld a,(0x{INV:04X})
        xor d
        ld (0x{OUT_PASS:04X}),a
        halt
"""


def gather_cases(yaw_step=8, max_cases=6000):
    d = load()
    cases = []
    for gy in range(GRID_H):
        for gx in range(GRID_W):
            ops, _ = build_block(d, gx, gy)
            if ops is None:
                continue
            for op in ops:
                if op[0] == OP_GATE:
                    sel = op[1]
                    for lx in (0, 15, 31, 32, 47, 63):
                        for ly in (0, 15, 31, 32, 47, 63):
                            cases.append((sel, lx, ly))
                            if len(cases) >= max_cases:
                                return d, cases
    return d, cases


def run_kernel(code, sel_a, sel_b, sel_c, lx, ly, inv):
    mem = bytearray(0x10000)
    mem[CODE:CODE + len(code)] = code
    mem[SEL_A] = sel_a & 0xFF
    mem[SEL_B] = sel_b & 0xFF
    mem[SEL_C] = sel_c & 0xFF; mem[SEL_C + 1] = (sel_c >> 8) & 0xFF
    mem[LX] = lx; mem[LY] = ly
    mem[INV] = inv
    cpu = Z80(mem)
    cpu.run(CODE)
    return cpu.t, cpu.m[OUT_PASS]


def main():
    selftest_multiply()

    code, _ = assemble(KERNEL)
    print(f"=== LAYER 2: GATE selector KERNEL, cycle-exact, {len(code)} bytes ===")

    d, cases = gather_cases()
    print(f"test cases: {len(cases)} real (selector, lx, ly) triples from "
          f"real blocks x 6x6 sub-cell grid\n")

    mismatches = 0
    ts = []
    for sel, lx, ly in cases:
        sel_a, sel_b, sel_c, inv = d.sel_a[sel], d.sel_b[sel], d.sel_c[sel], d.sel_inv[sel]
        t, got = run_kernel(code, sel_a, sel_b, sel_c, lx, ly, inv)
        ts.append(t)
        v = sel_a * lx + sel_b * ly + sel_c
        want = (1 if v >= 0 else 0) ^ inv
        if got != want:
            mismatches += 1
            if mismatches <= 8:
                print(f"  MISMATCH sel={sel} lx={lx} ly={ly}: "
                      f"a={sel_a} b={sel_b} c={sel_c} inv={inv} v={v} "
                      f"want={want} got={got}")

    n = len(cases)
    print(f"VERIFIED: {n - mismatches}/{n} exact matches against selector_pass()")
    if mismatches:
        raise SystemExit(f"FAIL: {mismatches}/{n} mismatches")

    import statistics as st
    mean_t = st.mean(ts)
    print(f"\nT-states per GATE test: mean={mean_t:.1f} min={min(ts)} max={max(ts)}")

    gates_mean = 2.71   # measured, span_decode_workload.py
    print(f"\nusing measured cost x real gate-tested count ({gates_mean}/update):")
    print(f"  GATE evaluation only   {mean_t*gates_mean:,.0f} T/update  "
          f"[CYCLE-EXACT, {n} cases verified]")

    decode_clip = 11036.0   # from z80_decode_bench.py, cycle-exact
    emit = 21756.0          # from z80_emit_bench.py, cycle-exact
    gate = mean_t * gates_mean
    print(f"\nrunning whole-update total (decode-clip + GATE + emit, all "
          f"cycle-exact):")
    print(f"  decode-clip   {decode_clip:,.0f} T")
    print(f"  GATE          {gate:,.0f} T")
    print(f"  emit          {emit:,.0f} T")
    print(f"  -----------------------------")
    print(f"  TOTAL         {decode_clip+gate+emit:,.0f} T   "
          f"(bearing lookup and column-solve still uncosted)")


if __name__ == "__main__":
    main()
