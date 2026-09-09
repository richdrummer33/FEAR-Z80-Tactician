#!/usr/bin/env python3
"""Shared cycle-exact Z80 assembler + interpreter for the span-interpreter benches.

The decode-clip, GATE, bearing and quarter-square benches each carry their own
hand-listed opcode table, which was fine at 20 instructions and is not fine at
the size the column-solve kernel needs. Two of the bugs those benches caught
were *assembler* bugs (prefix-match ordering, `XOR A` parsed as `XOR <label>`),
which is exactly what hand-listing invites.

So this generates the opcode map from the Z80's own regular encoding instead of
enumerating it:

    ld r,r'    0x40 | dst<<3 | src        (r: b c d e h l (hl) a  ->  0..7)
    <alu> a,r  0x80 | op<<3 | src         (add adc sub sbc and xor or cp)
    <alu> a,n  0xC6 | op<<3
    inc r      0x04 | r<<3   dec r  0x05 | r<<3
    ld r,n     0x06 | r<<3
    CB <rot> r 0x00 | op<<3 | r           (rlc rrc rl rr sla sra sll srl)
    CB bit b,r 0x40 | b<<3 | r            (res 0x80, set 0xC0)

One generator, one timing table, no per-instruction transcription. The
interpreter is driven from the same decomposition, so an instruction cannot be
assembled into a form the interpreter does not execute identically.

DELIBERATELY NOT RETROFITTED into the already-verified benches. Those recorded
their numbers against their own copies; swapping the substrate underneath a
published measurement invalidates it. When they are consolidated onto this
module, the check is that every bench reproduces its recorded T-state figure
exactly. Until then, duplication is the cheaper error.
"""
from __future__ import annotations

R8 = {"b": 0, "c": 1, "d": 2, "e": 3, "h": 4, "l": 5, "(hl)": 6, "a": 7}
R16 = {"bc": 0, "de": 1, "hl": 2, "sp": 3}
ALU = {"add a": 0, "adc a": 1, "sub": 2, "sbc a": 3,
       "and": 4, "xor": 5, "or": 6, "cp": 7}
ROT = {"rlc": 0, "rrc": 1, "rl": 2, "rr": 3,
       "sla": 4, "sra": 5, "sll": 6, "srl": 7}
CC = {"nz": 0, "z": 1, "nc": 2, "c": 3, "po": 4, "pe": 5, "p": 6, "m": 7}


class AsmError(Exception):
    pass


def _num(tok, labels, sizing):
    tok = tok.strip()
    if tok in labels:
        return labels[tok]
    try:
        return int(tok, 0) & 0xFFFF
    except ValueError:
        pass
    # allow simple `label+n` / `label-n`
    for sep in ("+", "-"):
        if sep in tok[1:]:
            a, b = tok.rsplit(sep, 1)
            if a.strip() in labels:
                v = labels[a.strip()]
                d = int(b.strip(), 0)
                return (v + d if sep == "+" else v - d) & 0xFFFF
    if sizing:
        return 0
    raise AsmError(f"unknown operand {tok!r}")


def _split(line):
    """Split `op a,b` into (mnemonic, [operands]) without splitting `(nn)`."""
    line = line.strip()
    if " " not in line:
        return line, []
    op, rest = line.split(None, 1)
    parts, depth, cur = [], 0, ""
    for ch in rest:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    parts.append(cur.strip())
    return op.lower(), [p.strip() for p in parts]


def encode(line, pc, labels, sizing):
    op, a = _split(line)
    n = len(a)

    def val(i):
        return _num(a[i], labels, sizing)

    def is_ind(t):
        return t.startswith("(") and t.endswith(")")

    def inner(t):
        return t[1:-1].strip()

    # --- no-operand ---
    simple = {"halt": (0x76, 4), "nop": (0x00, 4), "ex de,hl": (0xEB, 4),
              "exx": (0xD9, 4), "ret": (0xC9, 10), "scf": (0x37, 4),
              "ccf": (0x3F, 4), "cpl": (0x2F, 4), "daa": (0x27, 4),
              "ex af,af'": (0x08, 4),
              "rlca": (0x07, 4), "rrca": (0x0F, 4),
              "rla": (0x17, 4), "rra": (0x1F, 4)}
    if n == 0 and op in simple:
        return bytes([simple[op][0]])
    if n == 1 and f"{op} {a[0]}" in simple:
        return bytes([simple[f"{op} {a[0]}"][0]])
    if n == 2 and f"{op} {a[0]},{a[1]}" in simple:
        return bytes([simple[f"{op} {a[0]},{a[1]}"][0]])
    if op == "neg":
        return bytes([0xED, 0x44])

    # --- 16-bit block ---
    if op == "ld" and n == 2:
        d, s = a[0], a[1]
        if d in R16 and is_ind(s):
            v = _num(inner(s), labels, sizing)
            if d == "hl":
                return bytes([0x2A, v & 0xFF, v >> 8])
            return bytes([0xED, 0x4B | (R16[d] << 4), v & 0xFF, v >> 8])
        if is_ind(d) and s in R16:
            v = _num(inner(d), labels, sizing)
            if s == "hl":
                return bytes([0x22, v & 0xFF, v >> 8])
            return bytes([0xED, 0x43 | (R16[s] << 4), v & 0xFF, v >> 8])
        if d in R16:
            v = _num(s, labels, sizing)
            return bytes([0x01 | (R16[d] << 4), v & 0xFF, v >> 8])
        if d == "a" and is_ind(s) and inner(s) not in ("hl", "bc", "de"):
            v = _num(inner(s), labels, sizing)
            return bytes([0x3A, v & 0xFF, v >> 8])
        if is_ind(d) and s == "a" and inner(d) not in ("hl", "bc", "de"):
            v = _num(inner(d), labels, sizing)
            return bytes([0x32, v & 0xFF, v >> 8])
        if d in R8 and s in R8:
            if d == "(hl)" and s == "(hl)":
                raise AsmError("ld (hl),(hl)")
            return bytes([0x40 | (R8[d] << 3) | R8[s]])
        if d in R8:
            return bytes([0x06 | (R8[d] << 3), _num(s, labels, sizing) & 0xFF])
        raise AsmError(line)

    if op in ("add", "adc", "sbc") and n == 2 and a[0] == "hl" and a[1] in R16:
        rr = R16[a[1]]
        if op == "add":
            return bytes([0x09 | (rr << 4)])
        return bytes([0xED, (0x4A if op == "adc" else 0x42) | (rr << 4)])

    if op in ("inc", "dec") and n == 1:
        if a[0] in R16:
            return bytes([(0x03 if op == "inc" else 0x0B) | (R16[a[0]] << 4)])
        if a[0] in R8:
            return bytes([(0x04 if op == "inc" else 0x05) | (R8[a[0]] << 3)])
        raise AsmError(line)

    if op in ("push", "pop") and n == 1:
        rr = {"bc": 0, "de": 1, "hl": 2, "af": 3}[a[0]]
        return bytes([(0xC5 if op == "push" else 0xC1) | (rr << 4)])

    # --- 8-bit ALU ---
    if n == 0:
        raise AsmError(f"unsupported instruction {line!r}")
    key = op if n == 1 else f"{op} {a[0]}"
    src = a[-1]
    if key in ALU:
        code = ALU[key]
        if src in R8:
            return bytes([0x80 | (code << 3) | R8[src]])
        return bytes([0xC6 | (code << 3), _num(src, labels, sizing) & 0xFF])

    # --- CB ---
    if op in ROT and n == 1 and a[0] in R8:
        return bytes([0xCB, (ROT[op] << 3) | R8[a[0]]])
    if op in ("bit", "res", "set") and n == 2 and a[1] in R8:
        base = {"bit": 0x40, "res": 0x80, "set": 0xC0}[op]
        return bytes([0xCB, base | (int(a[0]) << 3) | R8[a[1]]])

    # --- flow ---
    if op in ("jp", "call") and n in (1, 2):
        base_u, base_c = (0xC3, 0xC2) if op == "jp" else (0xCD, 0xC4)
        if n == 1:
            if a[0] == "(hl)":
                return bytes([0xE9])
            v = val(0)
            return bytes([base_u, v & 0xFF, v >> 8])
        v = _num(a[1], labels, sizing)
        return bytes([base_c | (CC[a[0]] << 3), v & 0xFF, v >> 8])
    if op == "ret" and n == 1:
        return bytes([0xC0 | (CC[a[0]] << 3)])
    if op in ("jr", "djnz"):
        tgt = _num(a[-1], labels, sizing)
        d = 0 if sizing else (tgt - (pc + 2))
        if not sizing and not -128 <= d <= 127:
            raise AsmError(f"jr out of range ({d}) in {line!r}")
        d &= 0xFF
        if op == "djnz":
            return bytes([0x10, d])
        if n == 1:
            return bytes([0x18, d])
        return bytes([{"nz": 0x20, "z": 0x28, "nc": 0x30, "c": 0x38}[a[0]], d])

    raise AsmError(f"unsupported instruction {line!r}")


def assemble(src, org=0x0000):
    raw = []
    for line in src.splitlines():
        line = line.split(";")[0].rstrip()
        if line.strip():
            raw.append(line)
    labels, out = {}, b""
    for sizing in (True, False):
        pc, buf = org, bytearray()
        for line in raw:
            st = line.strip()
            if st.endswith(":") and " " not in st[:-1]:
                labels[st[:-1]] = pc
                continue
            enc = encode(st, pc, labels, sizing)
            buf += enc
            pc += len(enc)
        out = bytes(buf)
    return out, labels


class Z80:
    """Cycle-exact interpreter for the subset `encode` emits.

    Flags: only S/Z/C/PV-as-needed are modelled, and only where the kernels
    read them. An instruction whose flag effects are not modelled raises rather
    than silently returning a wrong-but-plausible answer.
    """

    def __init__(self, mem):
        self.m = bytearray(mem)
        self.r = dict(a=0, b=0, c=0, d=0, e=0, h=0, l=0)
        self.sp = 0xFFF0
        self.pc = 0
        self.t = 0
        self.cf = self.zf = self.sf = False

    # register-file access with (hl) as index 6
    _RN = ("b", "c", "d", "e", "h", "l", None, "a")

    def _get(self, i):
        if i == 6:
            return self.m[self.hl]
        return self.r[self._RN[i]]

    def _put(self, i, v):
        if i == 6:
            self.m[self.hl] = v & 0xFF
        else:
            self.r[self._RN[i]] = v & 0xFF

    @property
    def hl(self): return (self.r["h"] << 8) | self.r["l"]
    @property
    def de(self): return (self.r["d"] << 8) | self.r["e"]
    @property
    def bc(self): return (self.r["b"] << 8) | self.r["c"]

    def set16(self, name, v):
        v &= 0xFFFF
        if name == "sp":
            self.sp = v
            return
        hi, lo = name[0], name[1]
        self.r[hi], self.r[lo] = v >> 8, v & 0xFF

    def get16(self, name):
        if name == "sp":
            return self.sp
        return (self.r[name[0]] << 8) | self.r[name[1]]

    def run(self, start, limit=2_000_000):
        self.pc = start
        for _ in range(limit):
            if self.m[self.pc] == 0x76:
                self.t += 4
                return
            self._step()
        raise RuntimeError("z80: runaway - kernel never halted")

    def _rd(self):
        v = self.m[self.pc]
        self.pc = (self.pc + 1) & 0xFFFF
        return v

    def _rd16(self):
        return self._rd() | (self._rd() << 8)

    def _szf(self, v):
        self.zf = (v & 0xFF) == 0
        self.sf = bool(v & 0x80)

    def _alu(self, code, v):
        a = self.r["a"]
        if code == 0:      r = a + v;                       self.cf = r > 0xFF
        elif code == 1:    r = a + v + self.cf;             self.cf = r > 0xFF
        elif code == 2:    r = a - v;                       self.cf = r < 0
        elif code == 3:    r = a - v - self.cf;             self.cf = r < 0
        elif code == 4:    r = a & v;                       self.cf = False
        elif code == 5:    r = a ^ v;                       self.cf = False
        elif code == 6:    r = a | v;                       self.cf = False
        else:              r = a - v;                       self.cf = r < 0
        self._szf(r)
        if code != 7:
            self.r["a"] = r & 0xFF
        return

    def _step(self):
        m = self.m
        op = self._rd()

        if op == 0x00: self.t += 4; return
        if op == 0xEB:
            self.r["d"], self.r["h"] = self.r["h"], self.r["d"]
            self.r["e"], self.r["l"] = self.r["l"], self.r["e"]
            self.t += 4; return
        if op == 0x37: self.cf = True; self.t += 4; return
        if op == 0x3F: self.cf = not self.cf; self.t += 4; return
        if op == 0x2F: self.r["a"] ^= 0xFF; self.t += 4; return
        if op in (0x07, 0x0F, 0x17, 0x1F):
            a = self.r["a"]
            if op == 0x07:   c = a >> 7;  a = ((a << 1) | c) & 0xFF
            elif op == 0x0F: c = a & 1;   a = ((a >> 1) | (c << 7)) & 0xFF
            elif op == 0x17: c = a >> 7;  a = ((a << 1) | self.cf) & 0xFF
            else:            c = a & 1;   a = ((a >> 1) | (self.cf << 7)) & 0xFF
            self.r["a"] = a
            self.cf = bool(c)
            self.t += 4
            return

        # ld r,r'
        if 0x40 <= op < 0x80:
            d, s = (op >> 3) & 7, op & 7
            self._put(d, self._get(s))
            self.t += 7 if (d == 6 or s == 6) else 4
            return
        # alu a,r
        if 0x80 <= op < 0xC0:
            code, s = (op >> 3) & 7, op & 7
            self._alu(code, self._get(s))
            self.t += 7 if s == 6 else 4
            return
        # alu a,n
        if op & 0xC7 == 0xC6:
            self._alu((op >> 3) & 7, self._rd())
            self.t += 7
            return
        # ld r,n
        if op & 0xC7 == 0x06:
            d = (op >> 3) & 7
            self._put(d, self._rd())
            self.t += 10 if d == 6 else 7
            return
        # inc/dec r  (carry preserved, per Z80)
        if op & 0xC7 in (0x04, 0x05):
            d = (op >> 3) & 7
            v = self._get(d) + (1 if op & 1 == 0 else -1)
            self._put(d, v)
            self._szf(v)
            self.t += 11 if d == 6 else 4
            return
        # ld rr,nn
        if op & 0xCF == 0x01:
            self.set16(["bc", "de", "hl", "sp"][(op >> 4) & 3], self._rd16())
            self.t += 10
            return
        # add hl,rr
        if op & 0xCF == 0x09:
            v = self.hl + self.get16(["bc", "de", "hl", "sp"][(op >> 4) & 3])
            self.cf = v > 0xFFFF
            self.set16("hl", v)
            self.t += 11
            return
        # inc/dec rr
        if op & 0xCF in (0x03, 0x0B):
            nm = ["bc", "de", "hl", "sp"][(op >> 4) & 3]
            self.set16(nm, self.get16(nm) + (1 if op & 0x08 == 0 else -1))
            self.t += 6
            return
        if op == 0x32: m[self._rd16()] = self.r["a"]; self.t += 13; return
        if op == 0x3A: self.r["a"] = m[self._rd16()]; self.t += 13; return
        if op == 0x22:
            addr = self._rd16(); m[addr] = self.r["l"]; m[addr + 1] = self.r["h"]
            self.t += 16; return
        if op == 0x2A:
            addr = self._rd16(); self.set16("hl", m[addr] | (m[addr + 1] << 8))
            self.t += 16; return
        # push/pop
        if op & 0xCF == 0xC5:
            nm = ["bc", "de", "hl", "af"][(op >> 4) & 3]
            v = self.get16(nm) if nm != "af" else ((self.r["a"] << 8) | self._flags())
            self.sp = (self.sp - 2) & 0xFFFF
            m[self.sp] = v & 0xFF; m[self.sp + 1] = v >> 8
            self.t += 11; return
        if op & 0xCF == 0xC1:
            v = m[self.sp] | (m[self.sp + 1] << 8)
            self.sp = (self.sp + 2) & 0xFFFF
            nm = ["bc", "de", "hl", "af"][(op >> 4) & 3]
            if nm == "af":
                self.r["a"] = v >> 8; self._unflags(v & 0xFF)
            else:
                self.set16(nm, v)
            self.t += 10; return
        # jp / call / ret
        if op == 0xC3: self.pc = self._rd16(); self.t += 10; return
        if op & 0xC7 == 0xC2:
            addr = self._rd16()
            if self._cc((op >> 3) & 7):
                self.pc = addr
            self.t += 10; return
        if op == 0xCD:
            addr = self._rd16()
            self.sp = (self.sp - 2) & 0xFFFF
            m[self.sp] = self.pc & 0xFF; m[self.sp + 1] = self.pc >> 8
            self.pc = addr; self.t += 17; return
        if op & 0xC7 == 0xC4:
            addr = self._rd16()
            if self._cc((op >> 3) & 7):
                self.sp = (self.sp - 2) & 0xFFFF
                m[self.sp] = self.pc & 0xFF; m[self.sp + 1] = self.pc >> 8
                self.pc = addr; self.t += 17
            else:
                self.t += 10
            return
        if op == 0xC9:
            self.pc = m[self.sp] | (m[self.sp + 1] << 8)
            self.sp = (self.sp + 2) & 0xFFFF
            self.t += 10; return
        if op & 0xC7 == 0xC0:
            if self._cc((op >> 3) & 7):
                self.pc = m[self.sp] | (m[self.sp + 1] << 8)
                self.sp = (self.sp + 2) & 0xFFFF
                self.t += 11
            else:
                self.t += 5
            return
        if op == 0xE9: self.pc = self.hl; self.t += 4; return
        # jr / djnz
        if op in (0x18, 0x20, 0x28, 0x30, 0x38, 0x10):
            d = self._rd()
            d = d - 256 if d > 127 else d
            if op == 0x10:
                self.r["b"] = (self.r["b"] - 1) & 0xFF
                taken = self.r["b"] != 0
                self.t += 13 if taken else 8
            elif op == 0x18:
                taken = True; self.t += 12
            else:
                cc = {0x20: 0, 0x28: 1, 0x30: 2, 0x38: 3}[op]
                taken = self._cc(cc)
                self.t += 12 if taken else 7
            if taken:
                self.pc = (self.pc + d) & 0xFFFF
            return
        # CB
        if op == 0xCB:
            sub = self._rd()
            kind, r = sub >> 6, sub & 7
            v = self._get(r)
            extra = 15 if r == 6 else 8
            if kind == 0:
                rot = (sub >> 3) & 7
                if rot == 0:   c = v >> 7; v = ((v << 1) | c) & 0xFF
                elif rot == 1: c = v & 1;  v = ((v >> 1) | (c << 7)) & 0xFF
                elif rot == 2: c = v >> 7; v = ((v << 1) | self.cf) & 0xFF
                elif rot == 3: c = v & 1;  v = ((v >> 1) | (self.cf << 7)) & 0xFF
                elif rot == 4: c = v >> 7; v = (v << 1) & 0xFF
                elif rot == 5: c = v & 1;  v = ((v >> 1) | (v & 0x80)) & 0xFF
                elif rot == 6: c = v >> 7; v = ((v << 1) | 1) & 0xFF
                else:          c = v & 1;  v = (v >> 1) & 0xFF
                self.cf = bool(c)
                self._szf(v)
                self._put(r, v)
            elif kind == 1:
                self.zf = (v >> ((sub >> 3) & 7)) & 1 == 0
            elif kind == 2:
                self._put(r, v & ~(1 << ((sub >> 3) & 7)))
            else:
                self._put(r, v | (1 << ((sub >> 3) & 7)))
            self.t += extra
            return
        # ED
        if op == 0xED:
            sub = self._rd()
            if sub == 0x44:
                a = self.r["a"]
                self.r["a"] = (0 - a) & 0xFF
                self.cf = a != 0
                self._szf(self.r["a"])
                self.t += 8; return
            if sub & 0xCF == 0x42 or sub & 0xCF == 0x4A:      # sbc/adc hl,rr
                rr = self.get16(["bc", "de", "hl", "sp"][(sub >> 4) & 3])
                if sub & 0x08:
                    v = self.hl + rr + self.cf
                    self.cf = v > 0xFFFF
                else:
                    v = self.hl - rr - self.cf
                    self.cf = v < 0
                self.set16("hl", v)
                self.zf = (v & 0xFFFF) == 0
                self.sf = bool(v & 0x8000)
                self.t += 15; return
            if sub & 0xCF == 0x43:                            # ld (nn),rr
                addr = self._rd16()
                v = self.get16(["bc", "de", "hl", "sp"][(sub >> 4) & 3])
                m[addr] = v & 0xFF; m[addr + 1] = v >> 8
                self.t += 20; return
            if sub & 0xCF == 0x4B:                            # ld rr,(nn)
                addr = self._rd16()
                self.set16(["bc", "de", "hl", "sp"][(sub >> 4) & 3],
                           m[addr] | (m[addr + 1] << 8))
                self.t += 20; return
            raise RuntimeError(f"z80: unimplemented ED {sub:#04x}")

        raise RuntimeError(f"z80: unimplemented opcode {op:#04x} at {self.pc-1:#06x}")

    def _cc(self, i):
        return [not self.zf, self.zf, not self.cf, self.cf,
                False, False, not self.sf, self.sf][i]

    def _flags(self):
        return (0x80 if self.sf else 0) | (0x40 if self.zf else 0) | (1 if self.cf else 0)

    def _unflags(self, f):
        self.sf = bool(f & 0x80); self.zf = bool(f & 0x40); self.cf = bool(f & 1)


def s16(v):
    return v - 0x10000 if v >= 0x8000 else v


def s8(v):
    return v - 0x100 if v >= 0x80 else v
