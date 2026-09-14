"""DPSOLVE: the column-solve kernel for the path the Game Gear actually takes.

A49 measured `screen_depth_plane` succeeding on 99.10% of runs, which means
A14's kernel - and every budget line derived from it - prices the FALLBACK.
This builds the target path as a twin of that kernel: the `wall_d_q4`,
`inv_for_dq4` and `angle_x` stages are the SAME code, and only the stage that
turns `invd` into `(iq, step)` is swapped.  Two `inv_at_invd` calls and the Q6
ramp come out; the depth-plane solve goes in.

Verified against build/target_solve_oracle.txt, which is emitted by the same
probe whose dp_solve is diffed against the shipped source by
tools/target_solve_equiv.py.
"""
from __future__ import annotations
import pathlib
import re
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble, Z80                       # noqa: E402
import z80_column_solve_bench as cs                     # noqa: E402

CLSTAB = 0x3200        # uint8[17]  k_tspf_depth_normal_class
NFTAB  = 0x3220        # int8[7]    per-yaw, loaded once per frame on target
SFTAB  = 0x3230        # int8[7]
ENDQ   = 0xD040
INVMID = 0xD042
CLS    = 0xD044
CSGN   = 0xD045

# ---- the two stages that come OUT ----
CUT_START = "        ld hl,({LO:#06x})\n        ld ({REL:#06x}),hl".format(
    LO=cs.LO, REL=cs.REL)
CUT_END = "; ---- angle_x on both endpoints"

RAMP_START = "        ld a,({INV0:#06x})\n        ld l,a\n        ld h,0\n" \
             "        add hl,hl".format(INV0=cs.INV0)

DP = f"""
; ================= depth-plane solve (the TARGET path) ==================
; cls = k_tspf_depth_normal_class[sid];  nf,sf = the per-yaw coefficients the
; runtime caches once per frame.  iq = (invd*nf)>>1, step = (invd*sf)>>4, both
; signed.  Then walk iq from the reference column 10 to c0, accumulate endq
; over n columns, reject a run whose plane crosses zero, and fold the sign.
        ld a,({cs.SID:#06x})
        ld l,a
        ld h,0
        ld de,{CLSTAB:#06x}
        add hl,de
        ld a,(hl)                    ; cls
        ld ({CLS:#06x}),a            ; NOT in C: smul uses C as its sign flag
        ld l,a
        ld h,0
        ld de,{NFTAB:#06x}
        add hl,de
        ld a,(hl)                    ; nf
        call dp_mul                  ; HL = invd * nf, invd UNSIGNED
        ld b,1
        call shrn_signed
        ld ({cs.IQ:#06x}),hl
        ld a,({CLS:#06x})
        ld l,a
        ld h,0
        ld de,{SFTAB:#06x}
        add hl,de
        ld a,(hl)                    ; sf
        call dp_mul
        ld b,4
        call shrn_signed
        ld ({cs.STEP:#06x}),hl

; ---- walk iq from column 10 to c0 ----
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
; ---- endq = iq + n*step ----
        ld a,({cs.NCOL:#06x})
        ld b,a
dp_end:
        add hl,de
        djnz dp_end
        ld ({ENDQ:#06x}),hl
; ---- reject a sign crossing inside the run ----
; The C is `if ((iq<0 && endq>0) || (iq>0 && endq<0)) return 0;` and ZERO is
; neither, so an XOR-of-sign-bits test is NOT equivalent: it rejects iq==0 with
; endq>0, which the C accepts.  That cost 5 rows of 1,208.  Written literally.
        ld a,({cs.IQ+1:#06x})
        ld c,a                       ; C = high byte of iq
        bit 7,c
        jp z,dp_iq_nonneg
; iq < 0: reject only if endq > 0
        bit 7,h
        jp nz,dp_neg                 ; endq < 0 too -> negate both
        ld a,h
        or l
        jp z,dp_neg                  ; endq == 0 -> not > 0, so no reject
        jp dp_reject
dp_iq_nonneg:
        ld a,({cs.IQ:#06x})
        or c
        jp nz,dp_iq_pos
; iq == 0: never rejects, but the C still negates when endq < 0, because its
; second test is `if (iq<0 || endq<0)`.  Skipping that cost 3 rows of 1,208.
        bit 7,h
        jp nz,dp_neg
        jp dp_pos
dp_iq_pos:
; iq > 0: reject only if endq < 0
        bit 7,h
        jp z,dp_pos
        jp dp_reject
dp_reject:
        xor a
        ld ({cs.NCOL:#06x}),a        ; n = 0 marks "fell back", as cs_n does
        halt
dp_neg:
; ---- negate iq, endq and step together ----
        ld hl,({cs.IQ:#06x})
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
        ld ({cs.IQ:#06x}),hl
        ld hl,({ENDQ:#06x})
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
        ld ({ENDQ:#06x}),hl
        ld hl,({cs.STEP:#06x})
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
        ld ({cs.STEP:#06x}),hl
dp_pos:
; ---- inv_mid = clamp(((iq+endq)/2 + 32) >> 6, 255) ----
        ld hl,({cs.IQ:#06x})
        ld de,({ENDQ:#06x})
        add hl,de
        ld b,1
        call shrn_signed
        ld de,32
        add hl,de
        ld b,6
        call shrn_signed
        ld a,h
        or a
        jp nz,dp_clamp
        ld a,l
        ld ({INVMID:#06x}),a
        halt
dp_clamp:
        ld a,255
        ld ({INVMID:#06x}),a
        halt

; ---- invd (UNSIGNED u8) * coef (SIGNED i8) -> HL ----
; `smul` reads BOTH operands as i8, so invd = 255 became -1 and every
; near-wall run came out ~256x small.  The magnitude goes through umul and the
; coefficient's sign is applied once at the end.
dp_mul:
        ld c,0
        bit 7,a
        jp z,dpm_pos
        neg
        ld c,1
dpm_pos:
        ld ({cs.MB:#06x}),a
        ld a,c
        ld ({CSGN:#06x}),a
        ld a,({cs.INVD:#06x})
        ld ({cs.MA:#06x}),a
        call umul
        ld hl,({cs.MPROD:#06x})
        ld a,({CSGN:#06x})
        or a
        ret z
        xor a
        sub l
        ld l,a
        sbc a,a
        sub h
        ld h,a
        ret
"""


def _search(text, pattern, what, start=0):
        m = re.search(pattern, text[start:], re.M)
        if m is None:
                raise ValueError(f"{what} not found")
        return start + m.start(), start + m.end()


def build_target(src):
        i, _ = _search(src,
                                   rf"^\s*ld hl,\({cs.LO:#06x}\)\s*$\n"
                                   rf"\s*ld \({cs.REL:#06x}\),hl\s*$",
                                   "target-solve cut start")
        j, _ = _search(src,
                                   r"^; ---- angle_x on both endpoints, then the Q6 ramp .*----$",
                                   "target-solve cut end", i)
        src = src[:i] + src[j:]                 # drop the two inv_at calls
        k, _ = _search(src,
                                   rf"^\s*ld a,\({cs.INV0:#06x}\)\s*$\n"
                                   r"\s*ld l,a\s*$\n"
                                   r"\s*ld h,0\s*$\n"
                                   r"\s*add hl,hl\s*$",
                                   "target-solve ramp start")
        tail, _ = _search(src, r"^; ------.*$", "target-solve ramp end", k)
        src = src[:k] + DP + src[tail:]
        return src


def load_tables():
    T = cs.load_tables()
    txt = "\n".join(p.read_text() for p in sorted(
        (ROOT / "build" / "generated" / "polar_depthplane").glob("*")))
    import re
    m = re.search(r"k_tspf_depth_normal_class\[17\]\s*=\s*\{(.*?)\}", txt, re.S)
    T["cls"] = [int(x) for x in re.findall(r"-?\d+", m.group(1))]
    nf = re.search(r"k_depth_nf_q7\[7\]\[256\]\s*=\s*\{(.*?)\n\};", txt, re.S)
    sf = re.search(r"k_depth_stepfac_q4\[7\]\[256\]\s*=\s*\{(.*?)\n\};", txt, re.S)
    def rows(blob):
        return [[int(x) for x in re.findall(r"-?\d+", r)]
                for r in re.findall(r"\{([^{}]*)\}", blob)]
    T["nf"], T["sf"] = rows(nf.group(1)), rows(sf.group(1))
    return T


def main():
    budget = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
    T = load_tables()
    src = build_target(cs.SRC)
    code, _ = assemble(src, cs.CODE)
    dump = ROOT / "build" / "target_solve_oracle.txt"
    rows = [l.split() for l in dump.read_text().splitlines() if l.strip()]
    step = max(1, len(rows) // budget)
    rows = rows[::step]

    base = cs.build_mem(T)
    for i, v in enumerate(T["cls"]):
        base[CLSTAB + i] = v & 0xFF
    fails, ts = 0, []
    shown = 0
    for r in rows:
        (px, py, yaw, sid, invd, c0, c1, n, mid, iq, stp, lo, hi) = (int(v) for v in r)
        mem = bytearray(base)
        mem[cs.CODE:cs.CODE + len(code)] = code
        for i in range(7):
            mem[NFTAB + i] = T["nf"][i][yaw] & 0xFF
            mem[SFTAB + i] = T["sf"][i][yaw] & 0xFF
        mem[cs.SID] = sid
        cs.w16(mem, cs.XQ4, px); cs.w16(mem, cs.YQ4, py)
        cs.w16(mem, cs.YAWQ, (yaw << 4) & 0xFFFF)
        cs.w16(mem, cs.LO, lo & 0xFFFF); cs.w16(mem, cs.HI, hi & 0xFFFF)
        cpu = Z80(mem); cpu.run(cs.CODE)
        ts.append(cpu.t)
        def rd16(a):
            v = cpu.m[a] | (cpu.m[a + 1] << 8)
            return v - 0x10000 if v >= 0x8000 else v
        got = (cpu.m[cs.INVD], cpu.m[cs.C0], cpu.m[cs.C1], cpu.m[cs.NCOL],
               cpu.m[INVMID], rd16(cs.IQ), rd16(cs.STEP))
        want = (invd, c0, c1, n, mid, iq, stp)
        if got != want:
            fails += 1
            if shown < 6:
                shown += 1
                print(f"  MISMATCH ({px},{py},yaw={yaw}) sid={sid}: "
                      f"want {want} got {got}")

    mean = stt.mean(ts)
    print(f"=== DPSOLVE (Game Gear depth-plane path), {len(code)} bytes ===")
    print(f"{len(rows)} oracle rows, {fails} mismatches "
          f"{'EXACT' if not fails else '*** WRONG ***'}")
    print(f"\nT/span  mean {mean:,.1f}  min {min(ts):,}  max {max(ts):,}")
    spans = 4.30
    print(f"column-solve at {spans} spans/update: {mean*spans:,.0f} T/update")
    print(f"  A14 fallback kernel, measured    26,891 T/update")
    print(f"  A49 modelled target              ~13,300 T/update")
    print(f"  delta vs the budget line         {mean*spans-26891:+,.0f} T/update")
    print(f"\nwhole update 223,266 -> {223266 - 26891 + mean*spans:,.0f} T")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
