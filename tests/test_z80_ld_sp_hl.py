#!/usr/bin/env python3
"""`ld sp,hl` (0xF9, 6 T) - the instruction the compiled-edge-program
architecture needs to point the stack at a dispatched program.

Checked against the Z80's documented behaviour, not just "it assembles":
  - encodes to the single byte 0xF9
  - costs exactly 6 T-states
  - copies HL into SP
  - affects NO flags
  - does not disturb HL itself
  - the copied SP is immediately usable by `pop`, which is the whole point
  - round-trips through the full 16-bit range including the 0x0000 wrap

    python3 tests/test_z80_ld_sp_hl.py
"""
from __future__ import annotations
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80, AsmError            # noqa: E402

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)
    return cond


def run(src, org=0x0000, setup=None, mem=None):
    code, labels = assemble(src, org)
    m = bytearray(0x10000) if mem is None else bytearray(mem)
    m[org:org + len(code)] = code
    cpu = Z80(m)
    if setup:
        setup(cpu)
    cpu.run(org)
    return cpu, labels, len(code)


# 1. encoding and size
code, _ = assemble("ld sp,hl\nhalt\n", 0)
check(code[0] == 0xF9, f"encoding: expected 0xF9, got {code[0]:#04x}")
check(len(code) == 2, f"size: expected 1 byte + halt, got {len(code)}")

# 2. timing: 6 T for the instruction, 4 T for halt
cpu, _, _ = run("ld sp,hl\nhalt\n")
check(cpu.t == 10, f"timing: expected 6+4=10 T, got {cpu.t}")

# 3. it copies HL into SP, and leaves HL alone
cpu, _, _ = run("ld hl,0x8123\nld sp,hl\nhalt\n")
check(cpu.sp == 0x8123, f"copy: sp={cpu.sp:#06x}, expected 0x8123")
check(cpu.hl == 0x8123, f"hl clobbered: {cpu.hl:#06x}")

# 4. no flag is touched. Set all three modelled flags, then check they survive.
def set_flags(c):
    c.cf = c.zf = c.sf = True


cpu, _, _ = run("ld hl,0x0001\nld sp,hl\nhalt\n", setup=set_flags)
check(cpu.cf and cpu.zf and cpu.sf, "flags: ld sp,hl must not affect flags")


# 5. the copied SP is usable by pop - the actual use in the architecture.
mem = bytearray(0x10000)
mem[0x9000] = 0xCD
mem[0x9001] = 0xAB
mem[0x9002] = 0x34
mem[0x9003] = 0x12
cpu, _, _ = run("ld hl,0x9000\nld sp,hl\npop de\npop bc\nhalt\n", mem=mem)
check(cpu.de == 0xABCD, f"pop de after ld sp,hl: {cpu.de:#06x}, expected 0xABCD")
check(cpu.bc == 0x1234, f"pop bc after ld sp,hl: {cpu.bc:#06x}, expected 0x1234")
check(cpu.sp == 0x9004, f"sp after two pops: {cpu.sp:#06x}, expected 0x9004")

# 6. full-range round trip, including 0x0000 and 0xFFFF
for v in (0x0000, 0x0001, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF):
    cpu, _, _ = run(f"ld hl,{v}\nld sp,hl\nhalt\n")
    check(cpu.sp == v, f"round trip {v:#06x}: got {cpu.sp:#06x}")

# 7. the reverse direction is NOT silently accepted as this opcode.
try:
    assemble("ld hl,sp\nhalt\n", 0)
    fails.append("ld hl,sp should not assemble (no such Z80 instruction)")
except AsmError:
    pass

if fails:
    print("FAIL")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("ld sp,hl: encoding, 6 T timing, HL->SP copy, flag preservation,")
print("pop usability, full-range round trip, reverse-direction rejection - ALL PASS")
