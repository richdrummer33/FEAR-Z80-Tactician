#!/usr/bin/env python3
"""PROGJOIN: the compiled-edge-program chain, executed end to end for the
first time.

WHAT THIS CLOSES
----------------
Every previous compiled-edge-program result stopped short of the thing it
claimed. The 20-byte playback loop was real and cycle-counted, but it replayed
cells CAPTURED FROM THE SHIPPED RENDERER - no generated program was ever
consumed. The rank dispatcher was never committed as Z80 code at all, so its
381 T could not be reproduced. And the two were never joined, because the join
needs `ld sp,hl`, which the interpreter did not implement.

This runs the whole chain on real corpus inputs:

    corpus run-edge  ->  rank dispatch  ->  a REAL baked program
                     ->  `ld sp,hl`     ->  playback
                     ->  name-table cells
                     ->  compared against the renderer's own draw_edge
                     ->  complete cycle count

The programs come from `tools/edge_progjoin_bake.c`, which writes actual bytes,
and are built from the MODEL's formulas - not from renderer output. So a
mismatch here is a real defect in the architecture, which is the point.

WHAT IT DOES NOT CLOSE
----------------------
This is the EDGE PATH - the five materializer stages the architecture replaces,
68% of the materializer. The column walk, interior fill, border path and row
addressing are untouched and NOT re-integrated here, so this is not yet a
complete materializer, and the comparison is against the renderer's edge cells
rather than a whole 20x18 name table. Nothing is assembled into a ROM.

    make progjoin
"""
from __future__ import annotations
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80                      # noqa: E402

BUILD = ROOT / "build" / "progjoin"


class Overflow(Exception):
    """This window's tables do not fit the Z80's 64 KiB address space.

    A harness limit, not an architectural one - the real dispatch table is
    megabytes and needs banking regardless. The driver splits the window."""


CODE = 0x0000
ROWS, COLS = 18, 20
# GUARD BAND. Compiled programs are position-independent and so cannot carry
# the row clamp draw_edge applies at the viewport edges. Measured over the
# corpus, unclamped rows span exactly [-7, 24] and the renderer's clipped output
# is EXACTLY the unclipped cells with out-of-range rows dropped (39,570/39,570
# columns). So instead of clipping in the loop, the name table sits inside a
# taller buffer and off-screen cells land in scratch rows nobody reads.
# Cost: zero cycles, zero table growth, +560 bytes of WRAM.
GUARD = 7
BUF_ROWS = ROWS + 2 * GUARD          # 32 rows
MAP = 0xC200                         # buffer base
WIN = MAP + GUARD * COLS * 2         # the real 20x18 name table
BUF_BYTES = BUF_ROWS * COLS * 2

# scratch
V_FAM, V_FAMC, V_MASK, V_SLOT = 0xBF00, 0xBF01, 0xBF02, 0xBF03
V_U, V_RANK, V_BASE, V_WANT = 0xBF04, 0xBF05, 0xBF06, 0xBF07
V_COUNT, V_NREM = 0xBF08, 0xBF09
V_STEP, V_IQ, V_ACC, V_CUR = 0xBF0A, 0xBF0C, 0xBF0E, 0xBF10
V_BLK, V_BODY, SAVESP = 0xBF12, 0xBF14, 0xBF16
FAMMASK = 0xBF20


def load_manifest():
    m = {}
    for line in open(BUILD / "progjoin_manifest.txt"):
        f = line.split()
        if f[0] == "M":
            m.setdefault("M", {})[int(f[1])] = int(f[2])
        else:
            m[f[0]] = int(f[1])
    return m


def kernel_source(C, stepmap, desc, thresh, blocks, bodies):
    """dispatch -> ld sp,hl -> playback, looping the chunks of one run-edge.

    The destination cursor lives in memory between chunks rather than in HL,
    because every 16-bit index here needs HL and the interpreter does not
    implement `exx` (it assembles it but raises on execution). A real target
    would keep the cursor in HL' and save the two loads per chunk.
    """
    # C+1 thresholds: the chaining delta depends on the lookahead column's
    # RIGHT endpoint, i.e. height C+1, so the selector reads C+2 heights.
    rank = ""
    for k in range(C + 1):
        rank += "        cp (hl)\n        jr c,rank_done\n        inc b\n"
        if k != C:
            rank += "        inc hl\n"
    return f"""
        ld (0x{SAVESP:04x}),sp
        ld a,(0x{V_FAM:04x})            ; famc = fam*C, once per run-edge
        ld l,a
        ld h,0
        add hl,hl
        ld d,h
        ld e,l
        add hl,hl
        add hl,de
        ld a,l
        ld (0x{V_FAMC:04x}),a
        ld a,(0x{V_FAM:04x})            ; mask = M-1 for this family
        ld l,a
        ld h,0
        ld de,0x{FAMMASK:04x}
        add hl,de
        ld a,(hl)
        ld (0x{V_MASK:04x}),a
edge_loop:
        ld a,(0x{V_NREM:04x})           ; want = min(C, columns remaining)
        cp {C + 1}
        jr c,w_ok
        ld a,{C}
w_ok:
        ld (0x{V_WANT:04x}),a
        ld de,(0x{V_STEP:04x})          ; slot = STEPMAP[step + 2048]
        ld hl,0x{stepmap + 2048:04x}
        add hl,de
        ld a,(hl)
        ld (0x{V_SLOT:04x}),a
        ld hl,(0x{V_IQ:04x})            ; a = iq + 32
        ld de,32
        add hl,de
        ld (0x{V_ACC:04x}),hl
        ld a,l                       ; u = a & 127
        and 127
        ld (0x{V_U:04x}),a
        ld a,(0x{V_SLOT:04x})           ; thresholds at THRESH + slot*8
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,0x{thresh:04x}
        add hl,de
        ld b,0                       ; rank = #thresholds <= u, sorted so
        ld a,(0x{V_U:04x})              ; the first failure ends it
{rank}rank_done:
        ld a,b
        ld (0x{V_RANK:04x}),a
        ld hl,(0x{V_ACC:04x})           ; base = (a >> 7) & mask
        ld a,l
        add a,a
        ld a,h
        adc a,a
        ld hl,0x{V_MASK:04x}
        and (hl)
        ld (0x{V_BASE:04x}),a
        ld a,(0x{V_SLOT:04x})           ; desc idx = slot*32 + famc + (want-1)
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld a,(0x{V_FAMC:04x})
        ld e,a
        ld d,0
        add hl,de
        ld a,(0x{V_WANT:04x})
        dec a
        ld e,a
        ld d,0
        add hl,de
        add hl,hl
        ld de,0x{desc:04x}
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld hl,0x{blocks:04x}
        add hl,de
        ld (0x{V_BLK:04x}),hl
        ld a,(0x{V_BASE:04x})           ; entry = base*16 + rank*2
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld a,(0x{V_RANK:04x})
        add a,a
        ld e,a
        ld d,0
        add hl,de
        ld de,(0x{V_BLK:04x})
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld hl,0x{bodies:04x}
        add hl,de
        ld (0x{V_BODY:04x}),hl
        ld a,(0x{V_WANT:04x})           ; count = body[want] (prefix sums)
        ld e,a
        ld d,0
        add hl,de
        ld a,(hl)
        ld (0x{V_COUNT:04x}),a
        or a
        jr z,chunk_done
        ld hl,(0x{V_BODY:04x})          ; program starts after the C+1 header
        ld de,{C + 1}
        add hl,de
        ld sp,hl                     ; <<< the join
        ld hl,(0x{V_CUR:04x})
wp_loop:
        pop de
        ld (hl),e
        inc hl
        ld (hl),d
        pop bc
        add hl,bc
        dec a
        jp nz,wp_loop
        ld (0x{V_CUR:04x}),hl
chunk_done:
        ld a,(0x{V_WANT:04x})           ; iq += want*step
        ld b,a
        ld hl,(0x{V_IQ:04x})
        ld de,(0x{V_STEP:04x})
adv_loop:
        add hl,de
        djnz adv_loop
        ld (0x{V_IQ:04x}),hl
        ld a,(0x{V_NREM:04x})
        ld b,a
        ld a,(0x{V_WANT:04x})
        neg
        add a,b
        ld (0x{V_NREM:04x}),a
        jp nz,edge_loop
        ld sp,(0x{SAVESP:04x})
        halt
"""


def run_window(acc, quiet=False):
    man = load_manifest()
    C = man["C"]

    blobs = {}
    for nm in ("stepmap", "desc", "thresh", "blocks", "bodies"):
        blobs[nm] = (BUILD / f"progjoin_{nm}.bin").read_bytes()
        assert len(blobs[nm]) == man[nm], f"{nm} size mismatch"

    # lay the tables out below the name table
    addr, base = {}, 0x0400
    for nm in ("stepmap", "desc", "thresh", "blocks", "bodies"):
        addr[nm] = base
        base += (len(blobs[nm]) + 0xFF) & ~0xFF
    if base >= 0xBF00:
        raise Overflow(base)

    src = kernel_source(C, addr["stepmap"], addr["desc"], addr["thresh"],
                        addr["blocks"], addr["bodies"])
    code, labels = assemble(src, CODE)
    assert CODE + len(code) < 0x0400, "code overruns the table area"

    # Region boundaries for label-attributed cycle counting. A least-squares
    # split of per-edge / per-dispatch / per-cell is ill-conditioned here -
    # most run-edges have exactly one chunk, so those two columns are nearly
    # collinear. Attributing each instruction's own cycles by PC is exact.
    REGIONS = [
        (CODE, labels["edge_loop"], "per-edge setup"),
        (labels["edge_loop"], labels["wp_loop"], "dispatch"),
        (labels["wp_loop"], labels["chunk_done"], "playback"),
        (labels["chunk_done"], CODE + len(code), "chunk advance"),
    ]

    WP = labels["wp_loop"]

    class Traced(Z80):
        def _step(self):
            t0 = self.t
            pc = self.pc
            if pc == WP:
                self.played += 1
            Z80._step(self)
            d = self.t - t0
            for lo, hi, nm in REGIONS:
                if lo <= pc < hi:
                    self.region[nm] = self.region.get(nm, 0) + d
                    return
            self.region["other"] = self.region.get("other", 0) + d

    img = bytearray(0x10000)
    img[CODE:CODE + len(code)] = code
    for nm, blob in blobs.items():
        img[addr[nm]:addr[nm] + len(blob)] = blob
    for f in range(5):
        img[FAMMASK + f] = man["M"][f] - 1

    cases = [l.split() for l in open(BUILD / "progjoin_cases.txt") if l.strip()]

    total_t = 0
    n_edges = n_chunks = n_cells = n_played = 0
    wrong_cells = wrong_edges = stray = 0
    rows = []
    region_tot = {}

    for f in cases:
        i = 0
        fam, step, iq0, c0, ncol, sh = (int(f[i + k]) for k in range(6))
        i += 6
        first_dest = int(f[i]); i += 1
        ncs = int(f[i]); i += 1
        wants = [int(f[i + k]) for k in range(ncs)]; i += ncs
        ncell = int(f[i]); i += 1
        expect = []
        for k in range(ncell):
            expect.append((int(f[i + 2 * k]), int(f[i + 2 * k + 1])))
        i += 2 * ncell

        mem = bytearray(img)
        # sentinel the whole buffer AND a margin either side, so a write that
        # escapes the guard band is caught rather than silently tolerated
        for a in range(MAP - 256, MAP + BUF_BYTES + 256):
            mem[a] = 0x5A
        mem[V_FAM] = fam
        mem[V_STEP] = step & 0xFF
        mem[V_STEP + 1] = (step >> 8) & 0xFF
        mem[V_IQ] = iq0 & 0xFF
        mem[V_IQ + 1] = (iq0 >> 8) & 0xFF
        mem[V_NREM] = ncol
        cur = WIN + first_dest
        mem[V_CUR] = cur & 0xFF
        mem[V_CUR + 1] = (cur >> 8) & 0xFF

        cpu = Traced(mem)
        cpu.region = {}
        cpu.played = 0
        cpu.run(CODE)
        total_t += cpu.t
        n_edges += 1
        n_chunks += ncs
        n_cells += ncell
        n_played += cpu.played


        want_map = {WIN + d: w for d, w in expect}
        bad = 0
        for a, w in want_map.items():
            got = cpu.m[a] | (cpu.m[a + 1] << 8)
            if got != w:
                bad += 1
        # strays: anything written inside the REAL 20x18 window that the
        # renderer did not write. Guard rows are scratch and ignored.
        st = 0
        for a in range(WIN, WIN + ROWS * COLS * 2, 2):
            if a in want_map:
                continue
            if cpu.m[a] != 0x5A or cpu.m[a + 1] != 0x5A:
                st += 1
        esc = 0
        for a in range(MAP - 256, MAP):
            if cpu.m[a] != 0x5A:
                esc += 1
        for a in range(MAP + BUF_BYTES, MAP + BUF_BYTES + 256):
            if cpu.m[a] != 0x5A:
                esc += 1
        acc["escape"] = acc.get("escape", 0) + esc
        st += esc
        if (bad or st) and acc.get("dumped", 0) < 1:
            acc["dumped"] = 1
            print(f"  FIRST FAULT: fam={fam} step={step} iq0={iq0} c0={c0} "
                  f"ncol={ncol} ncs={ncs} wants={wants} escape={esc}")
            print(f"    expected {ncell} cells: {expect[:8]}")
            got = []
            for a in range(WIN, WIN + ROWS * COLS * 2, 2):
                if cpu.m[a] != 0x5A or cpu.m[a + 1] != 0x5A:
                    got.append(((a - WIN), cpu.m[a] | (cpu.m[a + 1] << 8)))
            print(f"    wrote    {len(got)} cells: {got[:8]}")
        wrong_cells += bad
        stray += st
        if bad or st:
            wrong_edges += 1
        rows.append((cpu.t, ncs, ncell))
        for k, v in cpu.region.items():
            region_tot[k] = region_tot.get(k, 0) + v

    acc["t"] = acc.get("t", 0) + total_t
    acc["edges"] = acc.get("edges", 0) + n_edges
    acc["chunks"] = acc.get("chunks", 0) + n_chunks
    acc["cells"] = acc.get("cells", 0) + n_cells
    acc["played"] = acc.get("played", 0) + n_played
    acc["wrong_cells"] = acc.get("wrong_cells", 0) + wrong_cells
    acc["stray"] = acc.get("stray", 0) + stray
    acc["wrong_edges"] = acc.get("wrong_edges", 0) + wrong_edges
    acc["code"] = len(code)
    acc["C"] = C
    acc["tables"] = max(acc.get("tables", 0),
                        sum(len(b) for b in blobs.values()))
    for k, v in region_tot.items():
        acc.setdefault("region", {})
        acc["region"][k] = acc["region"].get(k, 0) + v
    for nm in ("stepmap", "desc", "thresh", "blocks", "bodies"):
        acc.setdefault("blob", {})
        acc["blob"][nm] = max(acc["blob"].get(nm, 0), len(blobs[nm]))


def main():
    import subprocess
    nwin = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    wpose = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    bake = ROOT / "build" / "edge_progjoin_bake"
    oracle = ROOT / "build" / "coverage_pose_oracle.txt"
    acc, bake_stats = {}, {}
    splits = [0]

    def do(start, npose):
        if npose < 1:
            return
        r = subprocess.run([str(bake), str(oracle), "6", str(npose),
                            str(BUILD), str(start)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout); print(r.stderr)
            raise SystemExit(f"bake failed at pose {start}")
        stats = {}
        for line in r.stdout.splitlines():
            f = line.split()
            if "fully bakeable" in line:
                stats["ok"] = int(f[2])
            elif "GUARD-BAND ABSORBED" in line:
                stats["off"] = int(f[3])
            elif "invdepth clamp" in line:
                stats["clamp"] = int(f[3])
            elif line.startswith("run-edges"):
                stats["edges"] = int(f[1])
        try:
            run_window(acc)
        except Overflow:
            if npose == 1:
                raise SystemExit(f"a single pose overflows at {start}")
            h = npose // 2
            splits[0] += 1
            do(start, h)
            do(start + h, npose - h)
            return
        for k, v in stats.items():
            bake_stats[k] = bake_stats.get(k, 0) + v

    for w in range(nwin):
        do(w * wpose, wpose)

    total_t = acc["t"]
    n_edges, n_chunks, n_cells = acc["edges"], acc["chunks"], acc["cells"]
    n_played = acc.get("played", 0)
    print("=== PROGJOIN - dispatch + ld sp,hl + playback, on corpus inputs ===")
    print(f"kernel {acc['code']} bytes, C = {acc['C']} columns per program")
    print(f"{nwin} windows x {wpose} poses = {nwin*wpose} poses"
          f"  ({splits[0]} split to fit the address space)\n")
    print("largest window's emitted tables (bytes actually written to disk):")
    for nm in ("stepmap", "desc", "thresh", "blocks", "bodies"):
        print(f"  {nm:8} {acc['blob'][nm]:7,}")
    print(f"  TOTAL    {acc['tables']:7,}\n")

    be = bake_stats.get("edges", 0)
    print(f"run-edges in corpus       {be:,}")
    print(f"  bakeable / executed     {n_edges:,}  ({100.0*n_edges/max(be,1):.2f}%)")
    print(f"  off-screen, absorbed    {bake_stats.get('off',0):,}"
          f"  ({100.0*bake_stats.get('off',0)/max(be,1):.2f}%)"
          f"   <- by the guard band, at zero cycles")
    print(f"  excluded, invd clamp    {bake_stats.get('clamp',0):,}"
          f"  ({100.0*bake_stats.get('clamp',0)/max(be,1):.2f}%)")
    print(f"dispatches                {n_chunks:,}")
    print(f"cells landing on screen    {n_cells:,}")
    print(f"cells played (incl. guard) {n_played:,}"
          f"  ({100.0*(n_played-n_cells)/max(n_played,1):.1f}% absorbed off-screen)")

    print("\nCORRECTNESS, against the renderer's own draw_edge")
    print(f"  wrong cells             {acc['wrong_cells']}")
    print(f"  stray writes            {acc['stray']}")
    print(f"  run-edges with a fault  {acc['wrong_edges']}")
    verdict = "EXACT" if (acc["wrong_cells"] == 0 and acc["stray"] == 0) else "MISMATCH"
    print(f"  verdict                 {verdict}")

    print("\nCOST, cycle-counted by tools/z80core.py")
    print(f"  total                   {total_t:,} T")
    print(f"  per run-edge            {total_t / max(n_edges,1):,.1f} T")

    print("\n  where the cycles go (attributed by PC, not fitted)")
    reg = acc.get("region", {})
    for nm in ("per-edge setup", "dispatch", "playback", "chunk advance", "other"):
        v = reg.get(nm, 0)
        if not v:
            continue
        if nm == "dispatch":
            per = f"{v / max(n_chunks,1):8.1f} T per dispatch"
        elif nm == "playback":
            per = f"{v / max(n_played,1):8.1f} T per cell played"
        elif nm == "chunk advance":
            per = f"{v / max(n_chunks,1):8.1f} T per chunk"
        else:
            per = f"{v / max(n_edges,1):8.1f} T per run-edge"
        print(f"    {nm:16} {v:10,} T  {100.0*v/total_t:5.1f}%   {per}")

    disp = reg.get("dispatch", 0) / max(n_chunks, 1)
    play = reg.get("playback", 0) / max(n_played, 1)
    adv = reg.get("chunk advance", 0) / max(n_chunks, 1)
    setup = reg.get("per-edge setup", 0) / max(n_edges, 1)
    print("\n  AGAINST THE COMPOSED FIGURES")
    print(f"    dispatch   measured {disp:7.1f} T  vs   381 T claimed "
          f"({disp/381.0:.2f}x)  [381 T had no committed source]")
    print(f"    playback   measured {play:7.1f} T  vs  71.1 T composed "
          f"({play/71.1:.2f}x)")
    print(f"    chunk advance {adv:7.1f} T and per-edge setup {setup:.1f} T "
          f"appear in NO composed figure")
    return 0 if verdict == "EXACT" else 1


if __name__ == "__main__":
    sys.exit(main())
