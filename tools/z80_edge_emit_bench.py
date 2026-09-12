"""EDGE_EMIT: what does an edge row cost if ALL derivation is already done?

A39 measured the edge path at 1,023 T per row written, against the interior
fill's 52 T for the same kind of store.  Rung 3 of the state-lattice proposal
claims that gap back by compiling the answer offline.  Its ceiling is only real
if the *emission* of a precompiled edge row is cheap - and EDGELUT already
showed once that address formation can eat most of a table's apparent prize.

So this prices emission alone.  No geometry, no state machine, no union: a
precompiled stream in, name-table words out.

Exactness is by construction and then checked.  The stream is captured from the
shipped EDGELUT3 kernel's own stores, in execution order, so replaying it must
reproduce the kernel's map byte for byte - and FULL replay is verified against
the pose oracle, not against the kernel, so a capture bug cannot hide.

Variants differ in ONE thing: how a record is addressed.
  EMIT_A  4-byte (dest, word) records walked with HL, swapped via `ex de,hl`
  EMIT_B  the same records read through SP with `pop` (auto-increment for free)
  EMIT_C  run-grouped (dest, count, words...) exploiting horizontal contiguity
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80                       # noqa: E402
import z80_materialize_run_bench as rb                  # noqa: E402
import z80_materialize_masked_bench as mb               # noqa: E402
import z80_edge_lut_bench as el                         # noqa: E402
import z80_semantic_profile as sp                       # noqa: E402

CODE = 0x0000
STREAM = 0x8000
NROWS = 0xBF00                       # row count for the djnz loops
ROWS, COLS = rb.ROWS, rb.COLS

# ---------------------------------------------------------------- capture ---
# de_loop stores with `ld (hl),a` (0x77); df_loop is register-resident after
# FILLLOOP_D and stores with `ld (hl),c` / `ld (hl),b`.  That opcode split is
# an exact discriminator between an edge write and a fill write.
EDGE_OP = 0x77
FILL_OPS = (0x71, 0x70)


def capture(img, runs, bgm):
    """Return (edge_writes, all_writes) as ordered (addr, word) pairs."""
    mem = bytearray(img)
    for i, v in enumerate(bgm):
        mem[mb.MAP + 2 * i] = v & 0xFF
        mem[mb.MAP + 2 * i + 1] = v >> 8
    for i in range(COLS * 3):
        mem[mb.COV + i] = 0
    edge, allw = [], []

    class C(Z80):
        def _step(self):
            op = self.m[self.pc]
            h = self.hl
            hit = (op == EDGE_OP or op in FILL_OPS) and \
                  mb.MAP <= h < mb.MAP + 2 * ROWS * COLS and \
                  not (h - mb.MAP) & 1
            Z80._step(self)
            if hit:
                w = self.m[h] | (self.m[h + 1] << 8)   # low byte stored; high
                self.pend.append((h, op))              # follows next store
    for iq, stp, c0, c1, prof, lr, rr, sh in reversed(runs):
        mem[rb.IQ] = iq & 0xFF; mem[rb.IQ + 1] = (iq >> 8) & 0xFF
        mem[rb.STEP] = stp & 0xFF; mem[rb.STEP + 1] = (stp >> 8) & 0xFF
        mem[rb.PROFILE], mem[rb.C0], mem[rb.C1] = prof, c0, c1
        mem[rb.LREAL], mem[rb.RREAL], mem[rb.SHADE] = lr, rr, sh
        cpu = C(mem); cpu.pend = []; cpu.run(CODE)
        mem = cpu.m
        for h, op in cpu.pend:
            w = mem[h] | (mem[h + 1] << 8)
            allw.append((h, w))
            if op == EDGE_OP:
                edge.append((h, w))
    final = [mem[mb.MAP + 2 * i] | (mem[mb.MAP + 2 * i + 1] << 8)
             for i in range(ROWS * COLS)]
    return edge, allw, final


# ---------------------------------------------------------------- encoders --
def enc_flat(writes):
    b = bytearray()
    for a, w in writes:
        b += bytes((a & 0xFF, a >> 8, w & 0xFF, w >> 8))
    return b, len(writes)


def enc_runs(writes):
    """(dest_lo, dest_hi, count, then count words).  A run is a maximal group
    whose destinations advance by exactly +2 - horizontally adjacent cells in
    the same name-table row."""
    b = bytearray()
    i, n = 0, len(writes)
    nruns = 0
    while i < n:
        j = i + 1
        while j < n and writes[j][0] == writes[j - 1][0] + 2 and j - i < 255:
            j += 1
        a = writes[i][0]
        b += bytes((a & 0xFF, a >> 8, j - i))
        for k in range(i, j):
            b += bytes((writes[k][1] & 0xFF, writes[k][1] >> 8))
        nruns += 1
        i = j
    return b, nruns


# ---------------------------------------------------------------- kernels ---
SRC_A = f"""
        ld hl,{STREAM}
        ld a,(0x{NROWS:04x})
        ld b,a
ea_loop:
        ld e,(hl)                    ; dest lo
        inc hl
        ld d,(hl)                    ; dest hi
        inc hl
        ld a,(hl)                    ; word lo
        inc hl
        ld c,(hl)                    ; word hi
        inc hl
        ex de,hl                     ; HL = dest, DE = stream
        ld (hl),a
        inc hl
        ld (hl),c
        ex de,hl                     ; HL = stream, DE = dest+1
        djnz ea_loop
        halt
"""

SRC_B = f"""
        ld a,(0x{NROWS:04x})
        ld b,a
        ld sp,{STREAM}
eb_loop:
        pop hl                       ; HL = dest
        pop de                       ; DE = word
        ld (hl),e
        inc hl
        ld (hl),d
        djnz eb_loop
        halt
"""

SRC_C = f"""
        ld a,(0x{NROWS:04x})
        ld c,a                       ; C = run count
        ld sp,{STREAM}
ec_run:
        pop hl                       ; HL = dest
        pop de                       ; E = count, D = first word low byte...
; the count byte is odd-sized, so it is padded: see enc_runs_padded
ec_word:
        pop de
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        djnz ec_word
        dec c
        jp nz,ec_run
        halt
"""


def enc_runs_padded(writes):
    """Same grouping as enc_runs, but the header is (dest_lo, dest_hi,
    count, pad) so every `pop` stays word-aligned."""
    b = bytearray()
    i, n, nruns = 0, len(writes), 0
    while i < n:
        j = i + 1
        while j < n and writes[j][0] == writes[j - 1][0] + 2 and j - i < 255:
            j += 1
        a = writes[i][0]
        b += bytes((a & 0xFF, a >> 8, j - i, 0))
        for k in range(i, j):
            b += bytes((writes[k][1] & 0xFF, writes[k][1] >> 8))
        nruns += 1
        i = j
    return b, nruns


SRC_C = f"""
        ld a,(0x{NROWS:04x})
        ld c,a                       ; C = run count
        ld sp,{STREAM}
ec_run:
        pop hl                       ; HL = dest
        pop de                       ; E = count, D = pad
        ld a,e
        ld b,a
ec_word:
        pop de
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        djnz ec_word
        dec c
        jp nz,ec_run
        halt
"""


def run_stream(src, chunks, base_map):
    """`chunks` is a list of (bytes, record_count), each count <= 255 because
    the inner loop is a `djnz`.  Chunking costs one extra prologue per chunk
    (about 24 T per 255 records) and is counted honestly in the total."""
    code, _ = assemble(src, CODE)
    mem = bytearray(0x10000)
    mem[CODE:CODE + len(code)] = code
    for i, v in enumerate(base_map):
        mem[mb.MAP + 2 * i] = v & 0xFF
        mem[mb.MAP + 2 * i + 1] = v >> 8
    total = 0
    for blob, count in chunks:
        mem[STREAM:STREAM + len(blob)] = blob
        mem[NROWS] = count
        cpu = Z80(mem)
        cpu.run(CODE)
        total += cpu.t
        mem = cpu.m
    got = [mem[mb.MAP + 2 * i] | (mem[mb.MAP + 2 * i + 1] << 8)
           for i in range(ROWS * COLS)]
    return total, got, len(code)


def chunk_flat(writes, per=255):
    return [enc_flat(writes[i:i + per]) for i in range(0, len(writes), per)]


def chunk_runs(writes, per=255):
    out, i = [], 0
    while i < len(writes):
        j, nr = i, 0
        while j < len(writes) and nr < per:
            k = j + 1
            while k < len(writes) and writes[k][0] == writes[k - 1][0] + 2 \
                    and k - j < 255:
                k += 1
            nr += 1
            j = k
        out.append(enc_runs_padded(writes[i:j]))
        i = j
    return out


def main():
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 120
    dump = ROOT / "build" / "coverage_pose_oracle.txt"
    lines = [l.split() for l in dump.read_text().splitlines() if l.strip()]
    step = max(1, len(lines) // limit)
    cases = []
    for f in lines[::step]:
        n = int(f[0])
        runs = [tuple(int(v) for v in f[1 + 8 * k: 9 + 8 * k]) for k in range(n)]
        cases.append((runs, [int(v) for v in f[1 + 8 * n:]]))

    img, _, _ = sp.base_image(sp.instrumented_source(), el.load_table())
    bgm = mb.background_map()

    stats = {k: [] for k in ("A", "B", "C", "FULLB")}
    nedge, nall, nruns_c, over = [], [], [], 0
    fails = {k: 0 for k in stats}
    maxedge = 0

    for runs, want in cases:
        edge, allw, ref = capture(img, runs, bgm)
        if ref != want:
            raise SystemExit("capture replay diverged from the pose oracle")
        nedge.append(len(edge)); nall.append(len(allw))
        maxedge = max(maxedge, len(edge))

        # ---- edge-only emission, measured against the kernel's own edge cells
        edge_ref = list(bgm)
        for a, w in allw:                      # full replay gives the truth map
            edge_ref[(a - mb.MAP) // 2] = w
        # edge-only streams are scored on the cells they claim, so the base map
        # is the truth map with those cells knocked back to background
        base = list(edge_ref)
        for a, _ in edge:
            base[(a - mb.MAP) // 2] = bgm[(a - mb.MAP) // 2]

        # The edge stream is scored on what IT asks for: base, then the edge
        # writes applied in order.  Architectural exactness is proved
        # separately by FULLB, which is checked against the pose oracle.
        expect = list(base)
        for a, w in edge:
            expect[(a - mb.MAP) // 2] = w

        ch = chunk_flat(edge)
        t, got, _ = run_stream(SRC_A, ch, base)
        stats["A"].append(t / max(1, len(edge)))
        if got != expect: fails["A"] += 1

        t, got, _ = run_stream(SRC_B, ch, base)
        stats["B"].append(t / max(1, len(edge)))
        if got != expect: fails["B"] += 1

        chr_ = chunk_runs(edge)
        t, got, _ = run_stream(SRC_C, chr_, base)
        stats["C"].append(t / max(1, len(edge)))
        nruns_c.append(sum(c for _, c in chr_))
        if got != expect: fails["C"] += 1

        # ---- and the whole map from one stream, as a total-replay floor
        t, got, _ = run_stream(SRC_B, chunk_flat(allw), bgm)
        stats["FULLB"].append(t / max(1, len(allw)))
        if got != want: fails["FULLB"] += 1

    n = len(stats["A"])
    print(f"EDGE_EMIT - {n} poses replayed ({over} skipped: over 255 records)")
    print(f"  edge rows/pose {stt.mean(nedge):6.1f}  (max {maxedge})"
          f"   all words/pose {stt.mean(nall):6.1f}")
    if nruns_c:
        print(f"  horizontal runs/pose {stt.mean(nruns_c):5.1f}"
              f"   mean run length {stt.mean(nedge)/stt.mean(nruns_c):4.2f} cells")
    print()
    print(f"{'variant':8} {'T/edge row':>11} {'exact':>8}  what it is")
    labels = {
        "A": "flat (dest,word), HL walk + ex de,hl",
        "B": "flat (dest,word), read through SP with pop",
        "C": "run-grouped (dest,count,words...) via SP",
    }
    for k in ("A", "B", "C"):
        if not stats[k]:
            continue
        tag = "EXACT" if not fails[k] else f"{fails[k]} WRONG"
        print(f"EMIT_{k:3} {stt.mean(stats[k]):11.1f} {tag:>8}  {labels[k]}")
    print(f"\nfull-map replay (edge + fill) through EMIT_B: "
          f"{stt.mean(stats['FULLB']):.1f} T/word, "
          f"{'EXACT' if not fails['FULLB'] else str(fails['FULLB'])+' WRONG'}")

    print("\n--- against the measured EDGELUT3 edge path ---")
    cur = 1023.0
    for k in ("A", "B", "C"):
        if not stats[k]:
            continue
        v = stt.mean(stats[k])
        print(f"  EMIT_{k}: {v:6.1f} T/row vs {cur:.0f} T/row  -> {cur/v:5.2f}x"
              f"   edge path 66,252 -> {66252.0*v/cur:8,.0f} T/pose")
    best = min(stt.mean(stats[k]) for k in ("A", "B", "C") if stats[k])

    # ---- what is still in the way, from A39's measured stages --------------
    EDGE_PATH = 66252.0          # stages 6 + 7 + 8
    GEOM      = 28163.0          # stage 4, endpoint geometry
    EXTENTS   = 25357.0          # stage 5, row extents
    TARGET    = 58600.0          # materializer implied by a 3x sequence line
    cur_mat   = 175827.0
    emit_cost = EDGE_PATH * best / cur
    ladder = [
        ("EDGELUT3 materializer, measured", cur_mat),
        (f"edge path -> EMIT_B at {best:.1f} T/row", cur_mat - EDGE_PATH + emit_cost),
        ("+ endpoint geometry tabulated on (profile, h)",
         cur_mat - EDGE_PATH + emit_cost - GEOM),
        ("+ row extents tabulated on (profile, hmin, hmax)",
         cur_mat - EDGE_PATH + emit_cost - GEOM - EXTENTS),
    ]
    print("\n--- budget ladder to the 3x line ---")
    for label, v in ladder:
        print(f"  {label:46} {v:10,.0f} T")
    print(f"  {'3x sequence-baseline line':46} {TARGET:10,.0f} T")
    end = ladder[-1][1]
    verdict = "CLEARS" if end <= TARGET else "MISSES"
    print(f"\n  {verdict} the line by {abs(end-TARGET):,.0f} T "
          f"({100.0*(end-TARGET)/TARGET:+.1f}%)")
    return 1 if any(fails.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
