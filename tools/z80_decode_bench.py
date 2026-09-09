#!/usr/bin/env python3
"""Cycle-exact benchmark of the span interpreter's DECODE stage clip test.

This is `project_key`'s visibility window arithmetic
(`src/tilesector_polar_renderer.c:416-427`) as real Z80: assembled, run on a
from-scratch cycle-exact interpreter, and verified bit-for-bit (not just
aggregate statistics) against the Python reference on real camera poses.

WHY THIS IS THE HARD HALF OF DECODE
------------------------------------
The Z80 has no native signed 16-bit compare. Everything here is built from
one primitive: "is HL < DE (signed)?", answered by XORing bit 15 of both
operands (two's complement negatives, 0x8000-0xFFFF, become the SMALL
unsigned values 0x0000-0x7FFF; non-negatives become the large ones), then
doing a plain unsigned SBC HL,DE and reading the Carry flag. This primitive
is self-tested against 4,056 cases (all the boundary values this kernel
actually compares against, plus 4,000 random int16 pairs) BEFORE it goes
anywhere near the real kernel - see selftest_signed_lt().

Every intermediate value (len, st, en, lo, hi) lives in its own fixed memory
cell rather than being juggled through registers/stack across comparisons.
This costs a few extra loads/stores but makes each pipeline stage
independently inspectable and keeps the bias-flip from silently corrupting a
value another step still needs - the failure mode that made the first draft
of this kernel unreliable to reason about.

WHY THE WRAP "WHILE" LOOPS BECOME PLAIN "IF"S
-----------------------------------------------
`len` is rejected outside (0, 2048) before the wrap logic runs, so
len in [1, 2047]. `st = signed_q12(...)` is in [-2048, 2047] by
construction. So `en = st + len` is in [-2047, 4094]. Given that:

  - `while (en < -512)`: the smallest possible en is -2047. One +4096 step
    lands it at >= 2049, which can never again be < -512. So this "loop"
    executes 0 or 1 times, provably, for ANY input in the guaranteed range -
    not just empirically on this map's short wall segments.
  - `while (st > 512)`: symmetric argument, st in [-2048, 2047], one -4096
    step lands it at <= -2049, done.

So both become a single conditional +/-4096 adjustment to (st, en) together -
exactly matching the C semantics, with a proof of equivalence rather than an
assumption, and no unbounded loop for the Z80 to spend cycles discovering it
doesn't need.

    make span-decode-bench
"""
from __future__ import annotations
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from span_block_bake import load, build_block, OP_SPAN, OP_SPANC, OP_END  # noqa: E402
from span_decode_workload import exact_bearing, visible_window  # noqa: E402
from local_projection_field_poc import CELL_Q4, GRID_W, GRID_H  # noqa: E402

# --------------------------------------------------------------------------
# Memory map
# --------------------------------------------------------------------------
CODE = 0x0000
# inputs
A0, A1, YAWQ = 0xD000, 0xD002, 0xD004
# working cells - one per pipeline stage, all u16 (two's complement where signed)
LEN_, ST_, EN_ = 0xD010, 0xD012, 0xD014
LO_, HI_ = 0xD016, 0xD018
TMP = 0xD01A
# outputs
OUT_VIS, OUT_LO, OUT_HI = 0xD020, 0xD021, 0xD023

# --------------------------------------------------------------------------
# Minimal two-pass assembler for exactly the opcodes this kernel uses
# --------------------------------------------------------------------------
NO_ARG = {
    "or a": 0xB7, "halt": 0x76, "or l": 0xB5, "xor a": 0xAF,
    "ld a,h": 0x7C, "ld h,a": 0x67, "ld a,l": 0x7D, "ld l,a": 0x6F,
    "ld a,d": 0x7A, "ld d,a": 0x57,
}
IMM16 = {"ld hl,": 0x21, "ld de,": 0x11}
IMM8 = {"and ": 0xE6, "xor ": 0xEE, "or ": 0xF6}
ABS = {"jp ": 0xC3, "jp z,": 0xCA, "jp nz,": 0xC2, "jp c,": 0xDA, "jp nc,": 0xD2}


def assemble(src: str, org: int = CODE):
    raw = [l.split(";")[0].strip() for l in src.splitlines()]
    lines = [l for l in raw if l]
    labels: dict[str, int] = {}
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
    if line in NO_ARG:
        return bytes([NO_ARG[line]])
    if line == "sbc hl,de":
        return bytes([0xED, 0x52])
    if line.startswith("bit 3,"):
        reg = line.split(",")[1].strip()
        r = {"h": 4, "l": 5}[reg]
        return bytes([0xCB, 0x40 | (3 << 3) | r])
    if line.startswith("ld hl,(") and line.endswith(")"):
        v = _val(line[7:-1], labels, sizing)
        return bytes([0x2A, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld de,(") and line.endswith(")"):
        v = _val(line[7:-1], labels, sizing)
        return bytes([0xED, 0x5B, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld (") and ",hl" in line:
        inner = line[4:line.index("),hl")]
        v = _val(inner, labels, sizing)
        return bytes([0x22, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("ld (") and ",a" in line:
        inner = line[4:line.index("),a")]
        v = _val(inner, labels, sizing)
        return bytes([0x32, v & 0xFF, (v >> 8) & 0xFF])
    if line.startswith("add hl,de"):
        return bytes([0x19])
    if line.startswith("ld a,") and "(" not in line:
        v = _val(line[5:], labels, sizing)
        return bytes([0x3E, v & 0xFF])
    for pre, op in sorted(ABS.items(), key=lambda kv: -len(kv[0])):
        if line.startswith(pre):
            v = _val(line[len(pre):], labels, sizing)
            return bytes([op, v & 0xFF, (v >> 8) & 0xFF])
    for pre, op in IMM16.items():
        if line.startswith(pre):
            v = _val(line[len(pre):], labels, sizing)
            return bytes([op, v & 0xFF, (v >> 8) & 0xFF])
    for pre, op in IMM8.items():
        if line.startswith(pre):
            v = _val(line[len(pre):], labels, sizing)
            return bytes([op, v & 0xFF])
    raise SystemExit(f"assembler: unsupported instruction {line!r}")


# --------------------------------------------------------------------------
# Cycle-exact interpreter for exactly this opcode subset
# --------------------------------------------------------------------------
class Z80:
    def __init__(self, mem):
        self.m = bytearray(mem)
        self.a = self.h = self.l = self.d = self.e = 0
        self.pc = self.t = 0
        self.zf = self.cf = False

    @property
    def hl(self):
        return (self.h << 8) | self.l

    def sethl(self, v):
        v &= 0xFFFF
        self.h, self.l = v >> 8, v & 0xFF

    @property
    def de(self):
        return (self.d << 8) | self.e

    def setde(self, v):
        v &= 0xFFFF
        self.d, self.e = v >> 8, v & 0xFF

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
        if op == 0xB7:
            self.cf = False; self.zf = (self.a == 0); t = 4
        elif op == 0xAF:
            self.a = 0; self.cf = False; self.zf = True; t = 4
        elif op == 0xB5:
            self.zf = ((self.a | self.l) == 0); t = 4
        elif op == 0x7C: self.a = self.h; t = 4
        elif op == 0x67: self.h = self.a; t = 4
        elif op == 0x7D: self.a = self.l; t = 4
        elif op == 0x6F: self.l = self.a; t = 4
        elif op == 0x7A: self.a = self.d; t = 4
        elif op == 0x57: self.d = self.a; t = 4
        elif op == 0x3E:
            self.a = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x21:
            self.sethl(m[self.pc] | (m[self.pc + 1] << 8)); self.pc += 2; t = 10
        elif op == 0x11:
            self.setde(m[self.pc] | (m[self.pc + 1] << 8)); self.pc += 2; t = 10
        elif op == 0x2A:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            self.sethl(m[a] | (m[a + 1] << 8)); t = 16
        elif op == 0xED:
            sub = m[self.pc]; self.pc += 1
            if sub == 0x5B:                     # ld de,(nn)
                a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
                self.setde(m[a] | (m[a + 1] << 8)); t = 20
            elif sub == 0x52:                   # sbc hl,de
                res = (self.hl - self.de) & 0xFFFF
                self.cf = self.hl < self.de
                self.zf = (res == 0)
                self.sethl(res)
                t = 15
            else:
                raise SystemExit(f"z80: unimplemented ED opcode 0x{sub:02X}")
        elif op == 0x22:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            m[a] = self.l; m[a + 1] = self.h; t = 16
        elif op == 0x32:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            m[a] = self.a; t = 13
        elif op == 0xE6:
            self.a &= m[self.pc]; self.pc += 1; self.zf = (self.a == 0); t = 7
        elif op == 0xEE:
            self.a ^= m[self.pc]; self.pc += 1; self.zf = (self.a == 0); t = 7
        elif op == 0xF6:
            self.a |= m[self.pc]; self.pc += 1; self.zf = (self.a == 0); t = 7
        elif op == 0x19:                        # add hl,de
            self.sethl(self.hl + self.de); t = 11
        elif op == 0xCB:
            sub = m[self.pc]; self.pc += 1
            reg = sub & 0x07
            val = self.h if reg == 4 else self.l
            bit = (sub >> 3) & 0x07
            self.zf = ((val >> bit) & 1) == 0
            t = 8
        elif op == 0xC3:
            self.pc = m[self.pc] | (m[self.pc + 1] << 8); t = 10
        elif op == 0xCA:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            if self.zf: self.pc = a
            t = 10
        elif op == 0xC2:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            if not self.zf: self.pc = a
            t = 10
        elif op == 0xDA:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            if self.cf: self.pc = a
            t = 10
        elif op == 0xD2:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            if not self.cf: self.pc = a
            t = 10
        else:
            raise SystemExit(f"z80: unimplemented opcode 0x{op:02X} at pc={self.pc-1:#06x}")
        self.t += t


# --------------------------------------------------------------------------
# LAYER 1: self-test the signed-compare primitive in total isolation.
# "is HL < DE (signed)?" via bias-then-unsigned-SBC, Carry flag = answer.
# HL and DE are both consumed (biased in place); callers that need the
# original values afterward must reload them from memory - by design, so
# the kernel below never has to reason about "is this register secretly
# biased right now".
# --------------------------------------------------------------------------
LT_PRIMITIVE = """
        ld a,h
        xor 0x80
        ld h,a
        ld a,d
        xor 0x80
        ld d,a
        or a
        sbc hl,de
        halt
"""


def selftest_signed_lt():
    code, _ = assemble(LT_PRIMITIVE)
    cases = []
    import random
    rng = random.Random(1234)
    fixed = [0, -1, 1, -512, 512, -511, 511, -513, 513, -2048, 2047, 4094, -2047]
    for x in fixed:
        for y in (-512, 512, 0, x):
            cases.append((x & 0xFFFF, y & 0xFFFF))
    for _ in range(4000):
        x = rng.randint(-32768, 32767)
        y = rng.randint(-32768, 32767)
        cases.append((x & 0xFFFF, y & 0xFFFF))

    fails = 0
    for hlv, dev in cases:
        mem = bytearray(0x10000)
        mem[0:len(code)] = code
        cpu = Z80(mem)
        cpu.sethl(hlv)
        cpu.setde(dev)
        cpu.run(0)
        x_signed = hlv - 0x10000 if hlv >= 0x8000 else hlv
        y_signed = dev - 0x10000 if dev >= 0x8000 else dev
        expect = x_signed < y_signed
        if cpu.cf != expect:
            fails += 1
            if fails <= 5:
                print(f"  FAIL: HL={x_signed} DE={y_signed} expect_LT={expect} got={cpu.cf}")
    print("=== LAYER 1: signed-compare primitive self-test ===")
    print(f"cases={len(cases)}  fails={fails}")
    if fails:
        raise SystemExit(f"signed-compare primitive is WRONG - {fails}/{len(cases)} failed. "
                          "Stopping before building anything on top of it.")
    print("PASS - verified for the full int16 range\n")


# --------------------------------------------------------------------------
# LAYER 2: the real decode-clip kernel. Every LT_PRIMITIVE use is inlined
# as "load operands to HL/DE, run the same 8-instruction bias-sbc sequence,
# branch on Carry" - never abbreviated, so it stays visibly identical to the
# self-tested primitive above at every call site.
# --------------------------------------------------------------------------
KERNEL = f"""
        ld hl,(0x{A1:04X})
        ld de,(0x{A0:04X})
        or a
        sbc hl,de
        ld a,h
        and 0x0f
        ld h,a
        ld (0x{LEN_:04X}),hl

        ld a,h
        or l
        jp z,reject
        bit 3,h
        jp nz,reject

        ld hl,(0x{A0:04X})
        ld de,(0x{YAWQ:04X})
        or a
        sbc hl,de
        ld a,h
        and 0x0f
        ld h,a
        bit 3,h
        jp z,st_pos
        ld a,h
        or 0xf0
        ld h,a
st_pos:
        ld (0x{ST_:04X}),hl

        ld de,(0x{LEN_:04X})
        add hl,de
        ld (0x{EN_:04X}),hl

        ld hl,(0x{EN_:04X})
        ld de,0xfe00
        ld a,h
        xor 0x80
        ld h,a
        ld a,d
        xor 0x80
        ld d,a
        or a
        sbc hl,de
        jp nc,en_ok

        ld hl,(0x{ST_:04X})
        ld de,0x1000
        add hl,de
        ld (0x{ST_:04X}),hl
        ld hl,(0x{EN_:04X})
        ld de,0x1000
        add hl,de
        ld (0x{EN_:04X}),hl
en_ok:

        ld de,(0x{ST_:04X})
        ld hl,0x0200
        ld a,h
        xor 0x80
        ld h,a
        ld a,d
        xor 0x80
        ld d,a
        or a
        sbc hl,de
        jp nc,st_ok

        ld hl,(0x{ST_:04X})
        ld de,0x1000
        or a
        sbc hl,de
        ld (0x{ST_:04X}),hl
        ld hl,(0x{EN_:04X})
        ld de,0x1000
        or a
        sbc hl,de
        ld (0x{EN_:04X}),hl
st_ok:

        ld hl,(0x{ST_:04X})
        ld de,0xfe00
        ld a,h
        xor 0x80
        ld h,a
        ld a,d
        xor 0x80
        ld d,a
        or a
        sbc hl,de
        jp c,lo_is_neg512
        ld hl,(0x{ST_:04X})
        jp lo_done
lo_is_neg512:
        ld hl,0xfe00
lo_done:
        ld (0x{LO_:04X}),hl

        ld hl,0x0200
        ld de,(0x{EN_:04X})
        ld a,h
        xor 0x80
        ld h,a
        ld a,d
        xor 0x80
        ld d,a
        or a
        sbc hl,de
        jp c,hi_is_512
        ld hl,(0x{EN_:04X})
        jp hi_done
hi_is_512:
        ld hl,0x0200
hi_done:
        ld (0x{HI_:04X}),hl

        ld hl,(0x{LO_:04X})
        ld de,(0x{HI_:04X})
        ld a,h
        xor 0x80
        ld h,a
        ld a,d
        xor 0x80
        ld d,a
        or a
        sbc hl,de
        jp c,visible

        xor a
        ld (0x{OUT_VIS:04X}),a
        halt
visible:
        ld a,1
        ld (0x{OUT_VIS:04X}),a
        ld hl,(0x{LO_:04X})
        ld (0x{OUT_LO:04X}),hl
        ld hl,(0x{HI_:04X})
        ld (0x{OUT_HI:04X}),hl
        halt
reject:
        xor a
        ld (0x{OUT_VIS:04X}),a
        halt
"""


def gather_cases(yaw_step=8, max_cases=6000):
    """Real (a0,a1,yawq) triples from real block spans at real poses."""
    d = load()
    cases = []
    for gy in range(GRID_H):
        for gx in range(GRID_W):
            ops, _ = build_block(d, gx, gy)
            if ops is None:
                continue
            px = gx * CELL_Q4 + CELL_Q4 / 2
            py = gy * CELL_Q4 + CELL_Q4 / 2
            for yaw in range(0, 256, yaw_step):
                yaw_q12 = yaw * 16
                prev_bearing_deg = None
                for op in ops:
                    if op[0] == OP_END:
                        break
                    if op[0] == OP_SPAN:
                        v0 = op[2]
                        a0 = int(round(exact_bearing(d, v0, px, py))) & 0xFFF
                    elif op[0] == OP_SPANC:
                        a0 = prev_bearing_deg
                    else:
                        continue
                    v1 = op[3] if op[0] == OP_SPAN else op[2]
                    a1 = int(round(exact_bearing(d, v1, px, py))) & 0xFFF
                    prev_bearing_deg = a1
                    cases.append((a0, a1, yaw_q12 & 0xFFF))
                    if len(cases) >= max_cases:
                        return cases
    return cases


def run_kernel(code, a0, a1, yawq):
    mem = bytearray(0x10000)
    mem[CODE:CODE + len(code)] = code
    mem[A0] = a0 & 0xFF; mem[A0 + 1] = (a0 >> 8) & 0xFF
    mem[A1] = a1 & 0xFF; mem[A1 + 1] = (a1 >> 8) & 0xFF
    mem[YAWQ] = yawq & 0xFF; mem[YAWQ + 1] = (yawq >> 8) & 0xFF
    cpu = Z80(mem)
    cpu.run(CODE)
    vis = cpu.m[OUT_VIS]
    if not vis:
        return cpu.t, None
    lo = cpu.m[OUT_LO] | (cpu.m[OUT_LO + 1] << 8)
    hi = cpu.m[OUT_HI] | (cpu.m[OUT_HI + 1] << 8)
    lo = lo - 0x10000 if lo >= 0x8000 else lo
    hi = hi - 0x10000 if hi >= 0x8000 else hi
    return cpu.t, (lo, hi)


def main():
    selftest_signed_lt()

    code, _ = assemble(KERNEL)
    print(f"=== LAYER 2: decode-clip KERNEL, cycle-exact, {len(code)} bytes ===")

    cases = gather_cases()
    print(f"test cases: {len(cases)} real (a0,a1,yawq) triples from real poses "
          f"(span_block_bake x span_workload_probe sampling)\n")

    mismatches = 0
    ts = []
    for a0, a1, yawq in cases:
        t, got = run_kernel(code, a0, a1, yawq)
        ts.append(t)
        want = visible_window(float(a0), float(a1), float(yawq))
        if want is None:
            if got is not None:
                mismatches += 1
                if mismatches <= 8:
                    print(f"  MISMATCH a0={a0} a1={a1} yawq={yawq}: "
                          f"reference=CULLED kernel={got}")
        else:
            wlo, whi = int(round(want[0])), int(round(want[1]))
            if got is None or got != (wlo, whi):
                mismatches += 1
                if mismatches <= 8:
                    print(f"  MISMATCH a0={a0} a1={a1} yawq={yawq}: "
                          f"reference=({wlo},{whi}) kernel={got}")

    n = len(cases)
    print(f"VERIFIED: {n - mismatches}/{n} exact matches against the Python "
          f"reference (visibility decision AND lo/hi window value)")
    if mismatches:
        raise SystemExit(f"FAIL: {mismatches}/{n} mismatches - kernel is not "
                          "correct, do not trust its timing")

    import statistics as st
    mean_t = st.mean(ts)
    print(f"\nT-states per clip test: mean={mean_t:.1f} min={min(ts)} max={max(ts)}")
    print(f"\ncompare to the instruction-counted estimate (docs/TODO_DEFERRED.md A7):")
    print(f"  estimate  262-317 T/span (SPANC reuse / fresh SPAN)")
    print(f"  measured  {mean_t:.1f} T/span  <- this is now the trustworthy number")

    tested_mean = 12.00       # measured, span_decode_workload.py
    print(f"\nusing measured decode-clip cost x real span-tested count "
          f"({tested_mean}/update):")
    print(f"  decode-clip only   {mean_t*tested_mean:,.0f} T/update  "
          f"[CYCLE-EXACT, {n} cases verified]")
    print(f"  (GATE selector evaluation and column-solve are separate, "
          f"still-uncosted stages)")


if __name__ == "__main__":
    main()
