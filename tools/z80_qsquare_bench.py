#!/usr/bin/env python3
"""Cycle-exact benchmark of a QUARTER-SQUARE multiply table replacing the
shift-add primitive every kernel so far has used.

`z80_bearing_bench.py` and `z80_gate_bench.py` both measured the same
signed-8bit x [0,63]-unsigned multiply at ~324-432 T via a 6-iteration
double-and-add loop. `docs/TODO_DEFERRED.md` A11 projected a quarter-square
table could cut that to ~60-80 T from the shape of the trick, without
building it. This builds it, self-tests it, and substitutes it into the
bearing kernel's own 53,112-case oracle so the saving is measured, not
guessed - the discipline every other number in this file has already been
held to.

THE TRICK
---------
For non-negative integers p, q:  p*q = S(p+q) - S(|p-q|),  S(n) = floor(n^2/4)

This is exact (not approximate) because p+q and p-q always share parity, so
the two floor-divisions round the same way and the remainder cancels. A
256-entry table of S(n) for n=0..255 (16-bit entries, 512 bytes - the exact
size A11 guessed) covers every sum this kernel produces: the multiplies
being replaced are signed-8bit (magnitude 0..128) x unsigned 6-bit (0..63),
so p+q <= 191 and |p-q| <= 128, both inside the table's domain.

The second operand here is already non-negative (a sub-cell coordinate), so
only the first operand's sign needs separate handling - extract it once,
multiply magnitudes via the table, reapply the sign at the end. No repeated
add/shift loop, no per-bit branch.

Two-layer discipline: LAYER 1 self-tests the raw magnitude multiply against
Python's `a*b` across the full domain (129 x 64 = 8,256 exhaustive cases,
not a sample) before LAYER 2 substitutes it into the real bearing kernel
and re-runs the exact same 53,112-case oracle z80_bearing_bench.py used.

    make span-qsquare-bench
"""
from __future__ import annotations
import collections
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "experiments" / "adaptive_polar_field"))
sys.path.insert(0, str(ROOT / "tools"))
from local_projection_field_poc import (  # noqa: E402
    load as load_field, relevant, corner_quant_depth, quant_leaf_record,
    shr0, GRID_W, GRID_H, CELL_Q4,
)

THR = 4.0
MIN_Q4 = 8

# --------------------------------------------------------------------------
# Memory map
# --------------------------------------------------------------------------
CODE = 0x0000
QS_TABLE = 0x1000          # 256 x u16 = 512 bytes, S(n) = floor(n*n/4)

SLOPE = 0xD000              # in: int8
COORD = 0xD001               # in: u8, 0..63
NSH = 0xD002                  # in: u8, 8-shift (2..5 for depth 0..3)
OUT = 0xD003                   # out: i16, shr0(slope*coord, shift)

ABS_A = 0xD010
NEGF = 0xD012
SUM = 0xD013
DIFF = 0xD015
PSUM = 0xD017
PDIFF = 0xD019
PROD_MAG = 0xD01B

LEAF_ADDR = 0xD030
BASE_LO, BASE_HI = 0xD032, 0xD033
SX, SY = 0xD034, 0xD035
LX0, LY0 = 0xD036, 0xD037
DEP = 0xD038
LX, LY = 0xD039, 0xD03A
LEAF_PTR = 0xD03B
MASKTAB, NSHTAB = 0xD100, 0xD104
MASK = 0xD040
IX_, IY_ = 0xD041, 0xD042
DEPW = 0xD044
FOUT = 0xD046
LEAF_TABLE = 0xE000

# --------------------------------------------------------------------------
# Minimal assembler + cycle-exact interpreter, self-contained (does not
# import the already-verified z80_bearing_bench module, so nothing here can
# accidentally perturb that kernel's own recorded result).
# --------------------------------------------------------------------------
NO_ARG = {
    "halt": 0x76, "ld a,h": 0x7C, "ld a,l": 0x7D, "ld h,a": 0x67, "ld l,a": 0x6F,
    "ld d,a": 0x57, "ld e,a": 0x5F, "ld a,d": 0x7A, "ld a,e": 0x7B,
    "ld b,a": 0x47, "ld a,b": 0x78, "ld a,c": 0x79, "ld c,a": 0x4F,
    "add hl,de": 0x19, "add hl,hl": 0x29, "or a": 0xB7, "xor a": 0xAF,
    "sub l": 0x95, "sub h": 0x94, "sbc a,a": 0x9F, "inc hl": 0x23,
    "ld a,(hl)": 0x7E, "ld e,(hl)": 0x5E, "ld d,(hl)": 0x56,
    "ld c,(hl)": 0x4E, "ld b,(hl)": 0x46,
    "ex de,hl": 0xEB, "inc h": 0x24,
    "add a,c": 0x81, "sub c": 0x91, "ld l,c": 0x69, "ld h,c": 0x61,
}
IMM8 = {"ld a,": 0x3E, "ld b,": 0x06, "ld c,": 0x0E, "ld d,": 0x16,
        "ld e,": 0x1E, "ld h,": 0x26, "ld l,": 0x2E, "add a,": 0xC6,
        "adc a,": 0xCE}
ABS = {"jp nc,": 0xD2, "jp c,": 0xDA, "jp z,": 0xCA, "jp nz,": 0xC2, "jp ": 0xC3}


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
    if line == "sbc hl,de":
        return bytes([0xED, 0x52])
    if line == "neg":
        return bytes([0xED, 0x44])
    if line.startswith("bit 7,"):
        r = {"a": 7, "b": 0, "c": 1, "d": 2, "e": 3, "h": 4, "l": 5}[line.split(",")[1].strip()]
        return bytes([0xCB, 0x40 | (7 << 3) | r])
    if line.startswith("djnz "):
        tgt = _val(line[5:], labels, sizing)
        d = 0 if sizing else (tgt - (pc + 2)) & 0xFF
        return bytes([0x10, d])
    if line in NO_ARG:
        return bytes([NO_ARG[line]])
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
    if line.startswith("ld (") and ",de" in line:
        v = _val(line[4:line.index("),de")], labels, sizing)
        return bytes([0xED, 0x53, v & 0xFF, (v >> 8) & 0xFF])
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


class Z80:
    def __init__(self, mem):
        self.m = bytearray(mem)
        self.a = self.h = self.l = self.d = self.e = self.b = self.c = 0
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
        elif op == 0x5E: self.e = m[self.hl]; t = 7
        elif op == 0x56: self.d = m[self.hl]; t = 7
        elif op == 0x4E: self.c = m[self.hl]; t = 7
        elif op == 0x46: self.b = m[self.hl]; t = 7
        elif op == 0x24: self.h = (self.h + 1) & 0xFF; t = 4
        elif op == 0x69: self.l = self.c; t = 4
        elif op == 0x61: self.h = self.c; t = 4
        elif op == 0x81:
            r = self.a + self.c
            self.cf = r > 0xFF
            self.a = r & 0xFF
            self.zf = (self.a == 0)
            t = 4
        elif op == 0x91: self._sub(self.c); t = 4
        elif op == 0xCE:
            v = m[self.pc]; self.pc += 1
            r = self.a + v + (1 if self.cf else 0)
            self.cf = r > 0xFF
            self.a = r & 0xFF
            self.zf = (self.a == 0)
            t = 7
        elif op == 0x95: self._sub(self.l); t = 4
        elif op == 0x94: self._sub(self.h); t = 4
        elif op == 0x9F:
            self.a = 0xFF if self.cf else 0x00
            self.zf = (self.a == 0)
            t = 4
        elif op == 0xAF: self.a = 0; self.zf = True; self.cf = False; t = 4
        elif op == 0xB7: self.zf = (self.a == 0); self.cf = False; t = 4
        elif op == 0xEB:
            self.d, self.h = self.h, self.d
            self.e, self.l = self.l, self.e
            t = 4
        elif op == 0x3E: self.a = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x06: self.b = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x0E: self.c = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x16: self.d = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x1E: self.e = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x26: self.h = m[self.pc]; self.pc += 1; t = 7
        elif op == 0x2E: self.l = m[self.pc]; self.pc += 1; t = 7
        elif op == 0xC6:
            v = m[self.pc]; self.pc += 1
            r = self.a + v
            self.cf = r > 0xFF
            self.a = r & 0xFF
            self.zf = (self.a == 0)
            t = 7
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
            elif sub == 0x53:  # ld (nn),de
                a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
                m[a] = self.e; m[a + 1] = self.d; t = 20
            elif sub == 0x44:  # neg
                before = self.a
                self.a = (0 - self.a) & 0xFF
                self.cf = before != 0
                self.zf = (self.a == 0)
                t = 8
            elif sub == 0x52:  # sbc hl,de
                r = self.hl - self.de - (1 if self.cf else 0)
                self.cf = r < 0
                self.zf = (r & 0xFFFF) == 0
                self.sethl(r)
                t = 15
            else:
                raise SystemExit(f"z80: unimplemented ED opcode 0x{sub:02X}")
        elif op == 0xCB:
            sub = m[self.pc]; self.pc += 1
            if sub in (0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7F):
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
        elif op == 0xDA:
            a = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            if self.cf: self.pc = a
            t = 10
        else:
            raise SystemExit(f"z80: unimplemented opcode 0x{op:02X} at pc={self.pc-1:#06x}")
        self.t += t


def build_qs_table():
    return b"".join(((n * n) // 4).to_bytes(2, "little") for n in range(256))


# Two page-aligned 256-byte BYTE PLANES on adjacent pages: S_lo at page P,
# S_hi at page P+1. An index is then a single byte in A:
#     ld l,a / ld h,P / ld e,(hl) / inc h / ld d,(hl)
# with no `add hl,hl` to form a word offset and no `add hl,de` to add a base.
# `inc h` walks from the low plane to the high plane precisely because the
# pages are adjacent - that adjacency is a load-bearing layout requirement,
# not a convenience, and is asserted at build time below.
QS_LO_PAGE = 0x10
QS_HI_PAGE = 0x11
assert QS_HI_PAGE == QS_LO_PAGE + 1, "byte planes must be on adjacent pages"
QS_LO = QS_LO_PAGE << 8
QS_HI = QS_HI_PAGE << 8


def build_qs_planes():
    vals = [(n * n) // 4 for n in range(256)]
    assert max(vals) <= 0xFFFF
    return bytes(v & 0xFF for v in vals), bytes(v >> 8 for v in vals)


# --------------------------------------------------------------------------
# LAYER 1: raw magnitude multiply via quarter-square table, p in [0,128],
# q in [0,63] - the exact domain the sign-stripped slope*coord product uses.
# --------------------------------------------------------------------------
MAGMUL = f"""
        ld hl,0x{ABS_A:04X}
        ld e,(hl)
        inc hl
        ld d,(hl)
        ex de,hl
        ld a,(0x{COORD:04X})
        ld e,a
        ld d,0
        add hl,de
        ld (0x{SUM:04X}),hl
        ld hl,0x{ABS_A:04X}
        ld e,(hl)
        inc hl
        ld d,(hl)
        ex de,hl
        ld a,(0x{COORD:04X})
        ld e,a
        ld d,0
        or a
        sbc hl,de
        jp nc,mm_pos
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
mm_pos:
        ld (0x{DIFF:04X}),hl
        ld hl,(0x{SUM:04X})
        add hl,hl
        ld de,0x{QS_TABLE:04X}
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld (0x{PSUM:04X}),de
        ld hl,(0x{DIFF:04X})
        add hl,hl
        ld de,0x{QS_TABLE:04X}
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld (0x{PDIFF:04X}),de
        ld hl,(0x{PSUM:04X})
        ld de,(0x{PDIFF:04X})
        or a
        sbc hl,de
        ld (0x{PROD_MAG:04X}),hl
        halt
"""


def selftest_magmul():
    code, _ = assemble(MAGMUL)
    fails = 0
    n = 0
    for p in range(0, 129):
        for q in range(0, 64):
            mem = bytearray(0x10000)
            mem[QS_TABLE:QS_TABLE + 512] = build_qs_table()
            mem[CODE:CODE + len(code)] = code
            mem[ABS_A] = p & 0xFF
            mem[ABS_A + 1] = (p >> 8) & 0xFF
            mem[COORD] = q
            cpu = Z80(mem)
            cpu.run(CODE)
            got = cpu.m[PROD_MAG] | (cpu.m[PROD_MAG + 1] << 8)
            n += 1
            if got != p * q:
                fails += 1
                if fails <= 5:
                    print(f"  FAIL: {p}*{q} want {p*q} got {got}")
    print("=== LAYER 1: quarter-square magnitude multiply self-test ===")
    print(f"cases={n} (EXHAUSTIVE: every p in [0,128] x every q in [0,63])  "
          f"fails={fails}")
    if fails:
        raise SystemExit(f"primitive is WRONG - {fails}/{n} failed")
    print("PASS - exact match to Python's a*b on every case\n")
    return code


# --------------------------------------------------------------------------
# LAYER 2: full signed slope * unsigned coord, then shr0-by-shift - the
# exact operation z80_bearing_bench.py's _product_and_shift performs, now
# via the table instead of the 6-iteration loop.
# --------------------------------------------------------------------------
QMUL_SHR0 = f"""
        ld a,(0x{SLOPE:04X})
        ld l,a
        ld h,0
        bit 7,a
        jp z,sx_pos
        xor a
        sub l
        ld l,a
        ld a,1
        ld (0x{NEGF:04X}),a
        jp sx_abs_done
sx_pos:
        xor a
        ld (0x{NEGF:04X}),a
sx_abs_done:
        ld (0x{ABS_A:04X}),hl
        ld hl,0x{ABS_A:04X}
        ld e,(hl)
        inc hl
        ld d,(hl)
        ex de,hl
        ld a,(0x{COORD:04X})
        ld e,a
        ld d,0
        add hl,de
        ld (0x{SUM:04X}),hl
        ld hl,0x{ABS_A:04X}
        ld e,(hl)
        inc hl
        ld d,(hl)
        ex de,hl
        ld a,(0x{COORD:04X})
        ld e,a
        ld d,0
        or a
        sbc hl,de
        jp nc,q_pos
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
q_pos:
        ld (0x{DIFF:04X}),hl
        ld hl,(0x{SUM:04X})
        add hl,hl
        ld de,0x{QS_TABLE:04X}
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld (0x{PSUM:04X}),de
        ld hl,(0x{DIFF:04X})
        add hl,hl
        ld de,0x{QS_TABLE:04X}
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld (0x{PDIFF:04X}),de
        ld hl,(0x{PSUM:04X})
        ld de,(0x{PDIFF:04X})
        or a
        sbc hl,de
        ld a,(0x{NSH:04X})
        ld b,a
qshl:
        add hl,hl
        djnz qshl
        ld a,h
        ld l,a
        ld h,0
        ld a,(0x{NEGF:04X})
        or a
        jp z,q_done
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
q_done:
        ld (0x{OUT:04X}),hl
        halt
"""


# --------------------------------------------------------------------------
# LAYER 3 (A12): the same operation, rebuilt around two realisations the
# first table version missed.
#
# 1. PAGE-ALIGNED BYTE PLANES kill the address arithmetic (see above).
# 2. EVERY INDEX IS A SINGLE BYTE. |slope| <= 128 and coord <= 63, so
#    sum <= 191 and |diff| <= 128 - all inside one register. The first
#    version computed abs, sum and diff in 16-bit register pairs and staged
#    each through memory (13-16 T per touch) for no reason at all. That,
#    not the table indexing, was the larger share of the waste.
#
# Sign is not carried in a flag byte either: the result's sign is just the
# slope's sign, so bit 7 of (SLOPE) is re-tested at the end. That costs 21 T
# and frees B entirely for the shift loop's djnz.
# --------------------------------------------------------------------------
QMUL_SHR0_FAST = f"""
        ld a,(0x{SLOPE:04X})
        bit 7,a
        jp z,f_abs_done
        neg
f_abs_done:
        ld c,a
        ld a,(0x{COORD:04X})
        add a,c
        ld l,a
        ld h,0x{QS_LO_PAGE:02X}
        ld e,(hl)
        inc h
        ld d,(hl)
        ld a,(0x{COORD:04X})
        sub c
        jp nc,f_diff_ok
        neg
f_diff_ok:
        ld l,a
        ld h,0x{QS_LO_PAGE:02X}
        ld c,(hl)
        inc h
        ld a,(hl)
        ld h,a
        ld l,c
        ex de,hl
        or a
        sbc hl,de
        ld a,(0x{NSH:04X})
        ld b,a
f_shl:
        add hl,hl
        djnz f_shl
        ld a,h
        ld l,a
        ld h,0
        ld a,(0x{SLOPE:04X})
        bit 7,a
        jp z,f_done
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
f_done:
        ld (0x{OUT:04X}),hl
        halt
"""


# --------------------------------------------------------------------------
# LAYER 4: unsigned 8x8 -> 16, the shape COLUMN-SOLVE needs.
#
# The bearing lookup's multiply has a second operand <= 63, so its sums fit
# in one byte and `ld l,a` indexes the table directly. Column-solve's do
# not: `invd*dot` is 255 x 127 (sum <= 382), `q*sec` is 253 x ~181 (sum <=
# 434), and `(inv1-inv0)*recip` is 255 x 255 (sum <= 510). Ranges read from
# the C reference and its generated tables, not assumed.
#
# So the table grows to 512 entries. S(511) = 65,280, which still fits a
# u16 - with 255 to spare, checked at build time rather than hoped for.
# Byte planes become 512 bytes each: S_lo across pages P and P+1, S_hi
# across P+2 and P+3 (1 KiB of ROM total).
#
# A12's trick survives the ninth index bit intact, because that bit is
# exactly the carry `add a,c` already produced:
#     add a,c / ld l,a / ld a,P / adc a,0 / ld h,a
# `ld` does not disturb flags, so the carry is still live two instructions
# later. That is 11 T more than the 8-bit-index form, not a new lookup
# strategy. The DIFF index needs none of it: |a-b| <= 255 for any two bytes,
# so it always lands in the first page and keeps the cheap `ld h,P`.
# --------------------------------------------------------------------------
UPLANE_LO_PAGE = 0x20      # S_lo spans pages 0x20,0x21
UPLANE_HI_PAGE = 0x22      # S_hi spans pages 0x22,0x23
assert UPLANE_HI_PAGE == UPLANE_LO_PAGE + 2, "S_lo occupies two pages"
UPLANE_LO = UPLANE_LO_PAGE << 8
UPLANE_HI = UPLANE_HI_PAGE << 8

UA, UB = 0xD050, 0xD051
UPROD = 0xD052


def build_uplanes():
    vals = [(n * n) // 4 for n in range(512)]
    assert max(vals) <= 0xFFFF, f"S(511)={max(vals)} overflows u16"
    return bytes(v & 0xFF for v in vals), bytes(v >> 8 for v in vals)


UMUL = f"""
        ld a,(0x{UB:04X})
        ld c,a
        ld a,(0x{UA:04X})
        add a,c
        ld l,a
        ld a,0x{UPLANE_LO_PAGE:02X}
        adc a,0
        ld h,a
        ld e,(hl)
        inc h
        inc h
        ld d,(hl)
        ld a,(0x{UA:04X})
        sub c
        jp nc,u_diff_ok
        neg
u_diff_ok:
        ld l,a
        ld h,0x{UPLANE_LO_PAGE:02X}
        ld c,(hl)
        inc h
        inc h
        ld a,(hl)
        ld h,a
        ld l,c
        ex de,hl
        or a
        sbc hl,de
        ld (0x{UPROD:04X}),hl
        halt
"""


def selftest_umul():
    code, _ = assemble(UMUL)
    lo, hi = build_uplanes()
    fails = 0
    ts = []
    for a in range(256):
        for b in range(256):
            mem = bytearray(0x10000)
            mem[UPLANE_LO:UPLANE_LO + 512] = lo
            mem[UPLANE_HI:UPLANE_HI + 512] = hi
            mem[CODE:CODE + len(code)] = code
            mem[UA], mem[UB] = a, b
            cpu = Z80(mem)
            cpu.run(CODE)
            ts.append(cpu.t)
            got = cpu.m[UPROD] | (cpu.m[UPROD + 1] << 8)
            if got != a * b:
                fails += 1
                if fails <= 5:
                    print(f"  FAIL: {a}*{b} want {a*b} got {got}")
    print(f"=== LAYER 4: unsigned 8x8 -> 16 for column-solve, {len(code)} bytes ===")
    print(f"cases=65536 (EXHAUSTIVE: the ENTIRE u8 x u8 domain)  fails={fails}")
    if fails:
        raise SystemExit(f"primitive is WRONG - {fails}/65536 failed")
    print(f"PASS  mean={stt.mean(ts):.1f} T  min={min(ts)}  max={max(ts)}")
    print(f"      ROM: 1 KiB (two 512-byte byte planes)\n")
    return stt.mean(ts)


def _stage_fast(code, slope, coord, shift):
    mem = bytearray(0x10000)
    lo, hi = build_qs_planes()
    mem[QS_LO:QS_LO + 256] = lo
    mem[QS_HI:QS_HI + 256] = hi
    mem[CODE:CODE + len(code)] = code
    mem[SLOPE] = slope & 0xFF
    mem[COORD] = coord
    mem[NSH] = 8 - shift
    return mem


def run_qmul_fast(code, slope, coord, shift):
    cpu = Z80(_stage_fast(code, slope, coord, shift))
    cpu.run(CODE)
    got = cpu.m[OUT] | (cpu.m[OUT + 1] << 8)
    return cpu.t, got - 0x10000 if got >= 0x8000 else got


def selftest_qmul_fast():
    code, _ = assemble(QMUL_SHR0_FAST)
    cases = []
    for shift in (3, 4, 5, 6):
        span = 1 << shift
        for slope in range(-128, 128):
            for coord in range(0, span):
                cases.append((slope, coord, shift))
    fails = 0
    ts = []
    for slope, coord, shift in cases:
        t, got = run_qmul_fast(code, slope, coord, shift)
        ts.append(t)
        want = shr0(slope * coord, shift)
        if got != want:
            fails += 1
            if fails <= 5:
                print(f"  FAIL: shr0({slope}*{coord},{shift}) want {want} got {got}")
    print(f"=== LAYER 3 (A12): page-aligned byte-plane kernel, {len(code)} bytes ===")
    print(f"cases={len(cases)} (EXHAUSTIVE: every int8 slope x every valid "
          f"coord, all 4 depths)  fails={fails}")
    if fails:
        raise SystemExit(f"kernel is WRONG - {fails}/{len(cases)} failed")
    print(f"PASS  mean={stt.mean(ts):.1f} T  min={min(ts)}  max={max(ts)}\n")
    return code, stt.mean(ts)


def selftest_qmul_shr0():
    code, _ = assemble(QMUL_SHR0)
    fails = 0
    cases = []
    for shift in (3, 4, 5, 6):
        span = 1 << shift
        for slope in range(-128, 128):
            for coord in (0, 1, span // 2, span - 1):
                cases.append((slope, coord, shift))
    ts = []
    for slope, coord, shift in cases:
        mem = bytearray(0x10000)
        mem[QS_TABLE:QS_TABLE + 512] = build_qs_table()
        mem[CODE:CODE + len(code)] = code
        mem[SLOPE] = slope & 0xFF
        mem[COORD] = coord
        mem[NSH] = 8 - shift
        cpu = Z80(mem)
        cpu.run(CODE)
        ts.append(cpu.t)
        got = cpu.m[OUT] | (cpu.m[OUT + 1] << 8)
        got = got - 0x10000 if got >= 0x8000 else got
        want = shr0(slope * coord, shift)
        if got != want:
            fails += 1
            if fails <= 5:
                print(f"  FAIL: shr0({slope}*{coord},{shift}) want {want} got {got}")
    print(f"=== LAYER 2: table-based multiply+shr0 kernel, {len(code)} bytes ===")
    print(f"cases={len(cases)}  fails={fails}")
    if fails:
        raise SystemExit(f"kernel is WRONG - {fails}/{len(cases)} failed")
    print(f"PASS  mean={stt.mean(ts):.1f} T  min={min(ts)}  max={max(ts)}\n")
    return code, stt.mean(ts)


def gather_leaves():
    d = load_field()
    entries = []
    hist = collections.Counter()
    for gy in range(GRID_H):
        for gx in range(GRID_W):
            corners, _ = relevant(d, gx, gy)
            for v in corners:
                dep, _ = corner_quant_depth(d, v, gx, gy, THR, MIN_Q4)
                hist[dep] += 1
                if dep is None:
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
                    continue
                entries.append((gx, gy, v, dep, recs))
    return entries, hist


def reference(recs, dep, lx, ly):
    n = 1 << dep
    shift = 6 - dep
    step = 1 << shift
    idx = (ly >> shift) * n + (lx >> shift)
    base, sx, sy = recs[idx]
    return (base + shr0(sx * (lx & (step - 1)), shift)
            + shr0(sy * (ly & (step - 1)), shift)) & 4095


def run_qmul(code, slope, coord, shift):
    mem = bytearray(0x10000)
    mem[QS_TABLE:QS_TABLE + 512] = build_qs_table()
    mem[CODE:CODE + len(code)] = code
    mem[SLOPE] = slope & 0xFF
    mem[COORD] = coord
    mem[NSH] = 8 - shift
    cpu = Z80(mem)
    cpu.run(CODE)
    got = cpu.m[OUT] | (cpu.m[OUT + 1] << 8)
    return cpu.t, got - 0x10000 if got >= 0x8000 else got


def main():
    selftest_magmul()
    qmul_code, mean_t_isolated = selftest_qmul_shr0()
    fast_code, mean_t_fast = selftest_qmul_fast()
    umul_t = selftest_umul()

    print("=== SUBSTITUTED INTO THE REAL BEARING KERNEL'S OWN ORACLE ===")
    entries, hist = gather_leaves()
    total = sum(hist.values())

    rng_probes = [(0, 0), (63, 63), (0, 63), (63, 0), (31, 32), (32, 31),
                  (17, 45), (45, 17)]
    import random
    rng = random.Random(7)
    probes = rng_probes + [(rng.randint(0, 63), rng.randint(0, 63)) for _ in range(4)]

    variants = (("word-table (A11)", qmul_code, run_qmul),
                ("byte-plane (A12)", fast_code, run_qmul_fast))
    means = {}
    for label, code, runner in variants:
        mismatches = 0
        ts_by_dep = collections.defaultdict(list)
        n = 0
        for gx, gy, v, dep, recs in entries:
            shift = 6 - dep
            for lx, ly in probes:
                step = 1 << shift
                base, sx, sy = recs[(ly >> shift) * (1 << dep) + (lx >> shift)]
                lx0, ly0 = lx & (step - 1), ly & (step - 1)
                tx, resx = runner(code, sx, lx0, shift)
                ty, resy = runner(code, sy, ly0, shift)
                got = (base + resx + resy) & 4095
                want = reference(recs, dep, lx, ly)
                n += 1
                ts_by_dep[dep].append(tx + ty)
                if got != want:
                    mismatches += 1
                    if mismatches <= 8:
                        print(f"  MISMATCH [{label}] cell=({gx},{gy}) corner={v} "
                              f"dep={dep} lx={lx} ly={ly}: want {want} got {got}")
        print(f"{label}: VERIFIED {n - mismatches}/{n} against the SAME "
              f"reference z80_bearing_bench.py used")
        if mismatches:
            raise SystemExit(f"FAIL: {mismatches}/{n} mismatches in {label}")
        weighted = sum(stt.mean(ts_by_dep[dep]) * hist[dep]
                       for dep in ts_by_dep if dep in hist)
        denom = sum(hist[dep] for dep in ts_by_dep if dep in hist)
        means[label] = (weighted / denom, ts_by_dep)

    print("\nT-states for BOTH products of one bearing lookup, by leaf depth:")
    for label, (_, ts_by_dep) in means.items():
        row = "  ".join(f"d{dep}={stt.mean(ts_by_dep[dep]):7.1f}"
                        for dep in sorted(ts_by_dep))
        print(f"  {label:20s} {row}")

    OLD_MEAN_T = 1499.0     # measured, z80_bearing_bench.py, shift-add primitive
    m_word = means["word-table (A11)"][0]
    m_byte = means["byte-plane (A12)"][0]
    print(f"\nmap-weighted mean per FULL bearing lookup (base + 2 products):")
    print(f"  shift-add loop      (span-bearing-bench)  {OLD_MEAN_T:8.1f} T   baseline")
    print(f"  quarter-square, word table + 16-bit math  {m_word:8.1f} T   "
          f"{(1-m_word/OLD_MEAN_T):+6.1%}")
    print(f"  quarter-square, byte planes + byte math   {m_byte:8.1f} T   "
          f"{(1-m_byte/OLD_MEAN_T):+6.1%}")
    print(f"\n  A12's own contribution over A11: {m_word-m_byte:.1f} T "
          f"({(1-m_byte/m_word):.1%}) - the address arithmetic AND the")
    print(f"  needless 16-bit staging of byte-sized quantities, together.")

    distinct = 9.12   # measured, z80_bearing_bench.py: distinct corners/update
    decode_clip, gate, emit, column_solve = 11036.0, 2339.0, 21756.0, 17729.0
    frame = 59736.0
    print(f"\n=== WHOLE-UPDATE BUDGET "
          f"({distinct} distinct corners/update, cached) ===")
    for label, per in (("shift-add loop", OLD_MEAN_T),
                       ("word table (A11)", m_word),
                       ("byte planes (A12)", m_byte)):
        line = per * distinct
        total = line + decode_clip + gate + column_solve + emit
        print(f"  {label:20s} bearing {line:8,.0f} T   "
              f"TOTAL {total:8,.0f} T   {frame/total:.2f} updates/frame")
    print(f"\n  column-solve above is still carried at its 17,729 T PROJECTION,")
    print(f"  built on the OLD shift-add primitive. Re-costed below.")

    # Re-cost column-solve's multiply component on the measured LAYER 4
    # primitive. Op counts from column_solve_workload.py (14,912 real poses);
    # per-op cost measured here. This is still a projection - the kernel does
    # not exist - but both of its inputs are now measurements rather than one
    # measurement and one hand-count.
    MUL8_OLD, MUL16_OLD = 432.0, 454.0    # derived costs used by the old projection
    m8, m16 = 7.78, 21.52                  # measured op counts per update
    OVERHEAD = 1.35                        # measured non-multiply factor
    old_mul_component = (m8 * MUL8_OLD + m16 * MUL16_OLD) * OVERHEAD
    new_mul_component = ((m8 + m16) * umul_t) * OVERHEAD
    print(f"\n=== COLUMN-SOLVE, RE-COSTED ON THE MEASURED PRIMITIVE ===")
    print(f"  multiplies/update       {m8 + m16:.2f}  "
          f"(7.78 8x8 + 21.52 16x8, measured over 14,912 poses)")
    print(f"  per multiply, shift-add   {MUL16_OLD:6.1f} T  [derived, never built]")
    print(f"  per multiply, LAYER 4     {umul_t:6.1f} T  [MEASURED, 65,536 cases]")
    print(f"  column-solve multiply component:")
    print(f"    old projection        {old_mul_component:9,.0f} T")
    print(f"    re-costed             {new_mul_component:9,.0f} T")
    cs_new = 17729.0 - old_mul_component + new_mul_component
    print(f"  column-solve total      {cs_new:9,.0f} T  (was 17,729 T)")

    best_bearing = m_byte * distinct
    tot = best_bearing + decode_clip + gate + cs_new + emit
    print(f"\n=== PROJECTED WHOLE-UPDATE, BOTH STAGES ON THE NEW PRIMITIVE ===")
    print(f"  bearing lookup (A12)  {best_bearing:9,.0f} T   [cycle-exact]")
    print(f"  decode-clip           {decode_clip:9,.0f} T   [cycle-exact]")
    print(f"  GATE                  {gate:9,.0f} T   [cycle-exact, OLD primitive]")
    print(f"  column-solve          {cs_new:9,.0f} T   [projected, measured inputs]")
    print(f"  emit                  {emit:9,.0f} T   [cycle-exact]")
    print(f"  ------------------------------------")
    print(f"  TOTAL                 {tot:9,.0f} T   {frame/tot:.2f} updates/frame")
    print(f"\n  Treat the column-solve line as a floor, as before. Its op counts")
    print(f"  and its per-op cost are both measured now, but the kernel that")
    print(f"  glues them together is not built, and in this project the glue")
    print(f"  has been where the surprises live - emit's hand-count missed by")
    print(f"  94%, decode-clip's by 190%, on exactly this kind of reasoning.")


if __name__ == "__main__":
    main()
