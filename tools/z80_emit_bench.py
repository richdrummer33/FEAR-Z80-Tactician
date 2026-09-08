#!/usr/bin/env python3
"""Cycle-exact benchmark of the span interpreter's EMIT stage.

Emit is the largest and most confident line in the interpreter cost model: it
turns a per-column list of (count, name-table word) runs into the 720-byte WRAM
name-table shadow. Everything upstream (decode, projection, column solve) is
smaller and more speculative, so this is the line worth measuring first. If
emit does not come in near budget, no amount of decoder cleverness rescues the
architecture.

There is no Z80 assembler, GBDK or Gearsystem in this environment, so this file
carries its own: a two-pass assembler over the opcode subset the kernel uses,
and a cycle-exact interpreter for exactly that subset. Any opcode the assembler
emits that the interpreter does not implement is a hard error, so a wrong
T-state total cannot be produced silently.

Workload is real. `span_workload_probe --dump` writes raw 360-word viewports
straight out of the host Polar oracle; this script run-length encodes each
column into the run list a decoder would hand the emit kernel, and verifies the
emitted shadow word-for-word against the oracle viewport. A timing is only
reported for a kernel that produced the correct screen.

    make span-emit-bench

Z80 timings from the standard tables (z80.info / Zilog UM0080).

DESIGN NOTE - why the stream is read through SP
-----------------------------------------------
The obvious allocation is HL = map cursor in the main bank, HL' = stream cursor
in the shadow bank. It does not work, and the failure is instructive: EXX
*exchanges* banks, so a run count loaded into B while the shadow bank is active
is swapped away the instant you switch back to emit. Only A and SP survive an
EXX.

So the stream is read with POP, which is unbanked and also the fastest read on
the part (2 bytes in 10 T = 5 T/byte). Run records are laid out so the pops
land in the right registers with no shuffling at all:

    byte 0: word low    -> C   via pop bc
    byte 1: run count   -> B   via pop bc    (B is the djnz counter)
    byte 2: pad         -> F   via pop af    (flags already tested by then)
    byte 3: word high   -> A   via pop af    (A is unbanked)

The column counter lives in A', reached with EX AF,AF' (4 T), because every
other register is spoken for. This costs one pad byte per run and removes EXX
from the emit loop entirely.
"""
from __future__ import annotations
import pathlib
import sys

# --------------------------------------------------------------------------
# Emulated memory map
# --------------------------------------------------------------------------
CODE = 0x0000
GMAP = 0xC000          # 20 x 18 words, row-major, 40-byte row stride
STREAM = 0xD000        # per-column run descriptors, read via POP
COLS, ROWS = 20, 18
ROW_STRIDE = COLS * 2                                      # 40
STEP = ROW_STRIDE - 1                                      # 39: HL sits on the high byte
COL_BACK = (0x10000 - (ROWS * ROW_STRIDE - 2)) & 0xFFFF    # -718

# --------------------------------------------------------------------------
# Assembler
# --------------------------------------------------------------------------
NO_ARG = {
    "inc hl": 0x23, "add hl,de": 0x19, "add hl,bc": 0x09, "or a": 0xB7,
    "exx": 0xD9, "halt": 0x76, "dec a": 0x3D, "ld (hl),c": 0x71,
    "ld (hl),a": 0x77, "ld (hl),b": 0x70, "ld b,(hl)": 0x46,
    "ld c,(hl)": 0x4E, "ld a,(hl)": 0x7E, "ld a,b": 0x78, "ld a,c": 0x79,
    "dec e": 0x1D, "pop bc": 0xC1, "pop af": 0xF1, "ex af,af'": 0x08,
}
IMM16 = {"ld bc,": 0x01, "ld de,": 0x11, "ld hl,": 0x21, "ld sp,": 0x31}
IMM8 = {"ld a,": 0x3E, "ld b,": 0x06, "ld c,": 0x0E, "ld e,": 0x1E}
REL = {"djnz ": 0x10, "jr z,": 0x28, "jr nz,": 0x20, "jr ": 0x18}
ABS = {"jp ": 0xC3}


def assemble(src: str, org: int = CODE):
    lines = [l.split(";")[0].strip() for l in src.splitlines()]
    lines = [l for l in lines if l]
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
    for pre, op in REL.items():
        if line.startswith(pre):
            tgt = _val(line[len(pre):], labels, sizing)
            return bytes([op, 0 if sizing else (tgt - (pc + 2)) & 0xFF])
    for pre, op in ABS.items():
        if line.startswith(pre):
            v = _val(line[len(pre):], labels, sizing)
            return bytes([op, v & 0xFF, (v >> 8) & 0xFF])
    for pre, op in IMM16.items():
        if line.startswith(pre):
            v = _val(line[len(pre):], labels, sizing)
            return bytes([op, v & 0xFF, (v >> 8) & 0xFF])
    for pre, op in IMM8.items():
        if line.startswith(pre) and "(" not in line:
            v = _val(line[len(pre):], labels, sizing)
            return bytes([op, v & 0xFF])
    raise SystemExit(f"assembler: unsupported instruction {line!r}")


# --------------------------------------------------------------------------
# Cycle-exact interpreter for exactly that subset
# --------------------------------------------------------------------------
class Z80:
    def __init__(self, mem):
        self.m = bytearray(mem)
        self.a = self.b = self.c = self.d = self.e = self.h = self.l = 0
        self.a2 = 0
        self.b2 = self.c2 = self.d2 = self.e2 = self.h2 = self.l2 = 0
        self.z = self.z2 = False
        self.sp = self.pc = self.t = 0

    @property
    def hl(self):
        return (self.h << 8) | self.l

    def sethl(self, v):
        v &= 0xFFFF
        self.h, self.l = v >> 8, v & 0xFF

    def run(self, start, limit=4_000_000):
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
        if op == 0x23:
            self.sethl(self.hl + 1); t = 6
        elif op == 0x19:
            self.sethl(self.hl + ((self.d << 8) | self.e)); t = 11
        elif op == 0x09:
            self.sethl(self.hl + ((self.b << 8) | self.c)); t = 11
        elif op == 0xB7:
            self.z = (self.a == 0); t = 4
        elif op == 0x3D:
            self.a = (self.a - 1) & 0xFF; self.z = (self.a == 0); t = 4
        elif op == 0x08:                                   # ex af,af'
            self.a, self.a2 = self.a2, self.a
            self.z, self.z2 = self.z2, self.z
            t = 4
        elif op == 0xD9:                                   # exx
            self.b, self.b2 = self.b2, self.b
            self.c, self.c2 = self.c2, self.c
            self.d, self.d2 = self.d2, self.d
            self.e, self.e2 = self.e2, self.e
            self.h, self.h2 = self.h2, self.h
            self.l, self.l2 = self.l2, self.l
            t = 4
        elif op == 0xC1:                                   # pop bc
            self.c = m[self.sp]; self.b = m[(self.sp + 1) & 0xFFFF]
            self.sp = (self.sp + 2) & 0xFFFF; t = 10
        elif op == 0xF1:                                   # pop af
            f = m[self.sp]; self.a = m[(self.sp + 1) & 0xFFFF]
            self.z = bool(f & 0x40)
            self.sp = (self.sp + 2) & 0xFFFF; t = 10
        elif op == 0x71:
            m[self.hl] = self.c; t = 7
        elif op == 0x77:
            m[self.hl] = self.a; t = 7
        elif op == 0x70:
            m[self.hl] = self.b; t = 7
        elif op == 0x46:
            self.b = m[self.hl]; t = 7
        elif op == 0x4E:
            self.c = m[self.hl]; t = 7
        elif op == 0x7E:
            self.a = m[self.hl]; t = 7
        elif op == 0x78:
            self.a = self.b; t = 4
        elif op == 0x79:
            self.a = self.c; t = 4
        elif op == 0x1D:
            self.e = (self.e - 1) & 0xFF; self.z = (self.e == 0); t = 4
        elif op in (0x01, 0x11, 0x21, 0x31):
            v = m[self.pc] | (m[self.pc + 1] << 8); self.pc += 2
            if op == 0x01: self.b, self.c = v >> 8, v & 0xFF
            elif op == 0x11: self.d, self.e = v >> 8, v & 0xFF
            elif op == 0x21: self.sethl(v)
            else: self.sp = v
            t = 10
        elif op in (0x3E, 0x06, 0x0E, 0x1E):
            v = m[self.pc]; self.pc += 1
            if op == 0x3E: self.a = v
            elif op == 0x06: self.b = v
            elif op == 0x0E: self.c = v
            else: self.e = v
            t = 7
        elif op == 0x10:                                   # djnz
            d = m[self.pc]; self.pc += 1
            self.b = (self.b - 1) & 0xFF
            if self.b:
                self.pc = (self.pc + (d - 256 if d > 127 else d)) & 0xFFFF; t = 13
            else:
                t = 8
        elif op in (0x28, 0x20):
            d = m[self.pc]; self.pc += 1
            if (self.z if op == 0x28 else not self.z):
                self.pc = (self.pc + (d - 256 if d > 127 else d)) & 0xFFFF; t = 12
            else:
                t = 7
        elif op == 0x18:
            d = m[self.pc]; self.pc += 1
            self.pc = (self.pc + (d - 256 if d > 127 else d)) & 0xFFFF; t = 12
        elif op == 0xC3:
            self.pc = m[self.pc] | (m[self.pc + 1] << 8); t = 10
        else:
            raise SystemExit(f"z80: unimplemented opcode 0x{op:02X}")
        self.t += t


# --------------------------------------------------------------------------
# The emit kernel
# --------------------------------------------------------------------------
KERNEL = f"""
        ld sp,{STREAM}
        ld hl,{GMAP}
        ld de,{STEP}
        ld a,{COLS}
        ex af,af'
run:
        pop bc
        ld a,b
        or a
        jr z,col_end
        pop af
fill:
        ld (hl),c
        inc hl
        ld (hl),a
        add hl,de
        djnz fill
        jr run
col_end:
        ld bc,{COL_BACK}
        add hl,bc
        ex af,af'
        dec a
        jr z,done
        ex af,af'
        jr run
done:
        halt
"""


def rle_column(words, col):
    runs, prev, n = [], None, 0
    for r in range(ROWS):
        w = words[r * COLS + col]
        if w == prev:
            n += 1
        else:
            if prev is not None:
                runs.append((n, prev))
            prev, n = w, 1
    runs.append((n, prev))
    return runs


def build_stream(words):
    """4-byte run records [lo, count, pad, hi]; 2-byte zero-count terminator."""
    out = bytearray()
    total = 0
    for c in range(COLS):
        runs = rle_column(words, c)
        total += len(runs)
        for n, w in runs:
            out += bytes([w & 0xFF, n, 0x00, (w >> 8) & 0xFF])
        out += bytes([0x00, 0x00])
    return bytes(out), total


def load_poses(path):
    poses, cur = [], None
    for line in pathlib.Path(path).read_text().splitlines():
        if line.startswith("pose"):
            cur = []
            poses.append(cur)
        elif cur is not None and line.strip():
            cur.extend(int(x) for x in line.split())
    return [p for p in poses if len(p) == COLS * ROWS]


def project_unrolled(runs_per_pose):
    """Analytic x4 partial unroll, from the measured run-length distribution.

    Rolled: each word costs 31 T of stores plus 13 T of djnz, except a run's
    last iteration which pays 8 T. Unrolled x4: three words in four pay only
    the 31 T, and each group of four pays one 16 T loop control.
    """
    rolled = unrolled = 0.0
    for runs in runs_per_pose:
        for n in runs:
            rolled += n * 44 - 5
            quads, rem = divmod(n, 4)
            unrolled += quads * (4 * 31 + 16) + rem * 44
    k = len(runs_per_pose)
    return rolled / k, unrolled / k


def main():
    dump = sys.argv[1] if len(sys.argv) > 1 else "build/emit_poses.txt"
    poses = load_poses(dump)
    if not poses:
        raise SystemExit(f"no poses in {dump}; run span-workload-probe --dump first")

    code, _ = assemble(KERNEL)
    print("=== SPAN INTERPRETER: EMIT KERNEL, CYCLE-EXACT ===")
    print(f"kernel    {len(code)} bytes of Z80, POP-streamed, rolled djnz fill")
    print(f"workload  {len(poses)} real viewports from the host Polar oracle")
    print(f"target    {COLS}x{ROWS} = {COLS*ROWS} words emitted unconditionally\n")

    tot_t = tot_runs = 0
    worst, best = 0, 10 ** 9
    runs_per_pose = []
    for words in poses:
        stream, nruns = build_stream(words)
        mem = bytearray(0x10000)
        mem[CODE:CODE + len(code)] = code
        mem[STREAM:STREAM + len(stream)] = stream
        cpu = Z80(mem)
        cpu.run(CODE)
        for r in range(ROWS):
            for c in range(COLS):
                p = GMAP + r * ROW_STRIDE + c * 2
                got = cpu.m[p] | (cpu.m[p + 1] << 8)
                if got != words[r * COLS + c]:
                    raise SystemExit(
                        f"MISMATCH row {r} col {c}: {got} != {words[r*COLS+c]}")
        tot_t += cpu.t
        tot_runs += nruns
        worst, best = max(worst, cpu.t), min(best, cpu.t)
        runs_per_pose.append([n for c in range(COLS) for n, _ in rle_column(words, c)])

    n = len(poses)
    mean = tot_t / n
    print(f"VERIFIED  all {n} viewports reproduced word-for-word\n")
    print(f"runs/update       mean={tot_runs/n:.2f}")
    print(f"T/update          mean={mean:,.0f}   min={best:,}   max={worst:,}")
    print(f"T per word        {mean/(COLS*ROWS):.2f}")

    r_mean, u_mean = project_unrolled(runs_per_pose)
    saved = r_mean - u_mean
    print(f"\nfill-only rolled (analytic)        {r_mean:,.0f} T")
    print(f"fill-only x4 unrolled (projected)  {u_mean:,.0f} T"
          f"   saves {saved:,.0f} T ({saved/r_mean:.1%} of fill)")
    print(f"projected emit with x4 unroll      {mean-saved:,.0f} T")

    budget = 11200
    print(f"\nISA-spec estimate                  {budget:,} T")
    print(f"measured, rolled                   {mean:,.0f} T  ({mean/budget-1:+.0%})")
    print(f"projected, x4 unrolled             {mean-saved:,.0f} T  "
          f"({(mean-saved)/budget-1:+.0%})")
    hz = 3579545.0
    print(f"\nat 3.579545 MHz   emit alone = {mean/hz*1000:.2f} ms, "
          f"{mean/(hz/60):.2f} video frames")


if __name__ == "__main__":
    main()
