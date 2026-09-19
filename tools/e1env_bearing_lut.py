#!/usr/bin/env python3
"""Generate an exact first-quadrant bearing LUT for the exact-Q4 renderer.

The current bearing_q12() spends most of its time computing an 8-bit ratio
with two generic 16-bit multiplies, then indexing k_tspf_atan_q12. After the
existing scale-down step both magnitudes are bytes, so the entire first-
quadrant answer is a pure 256x256 function.

A complete uint16 table is 128 KiB. We keep bank dispatch cheap by using eight
32-row groups. Each bank stores 31 rows (15,872 bytes) plus one tiny BANKED
lookup function; the final row of every group falls back to the original
arithmetic. Thus bank selection is simply ax8>>5 and local row ax8&31, no
division table and no approximation. Every LUT value is bit-identical to the
existing C arithmetic.
"""
from __future__ import annotations
import argparse
import re
from pathlib import Path


def values(text: str, name: str):
    m = re.search(
        r"static\s+const\s+[^;=]+?\b" + re.escape(name)
        + r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};",
        text, re.S)
    if not m:
        raise SystemExit(f"missing array {name}")
    return [int(x, 0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+", m.group(1))]


def ratio_q8(n: int, d: int, recip):
    if not d:
        return 0
    rec = recip[d]
    p_lo = n * (rec & 0xff)
    p_hi = n * ((rec >> 8) & 0xff)
    q = p_hi + ((p_lo + 128) >> 8)
    return min(255, q)


def quadrant_angle(ax: int, ay: int, recip, atan):
    if ax == 0 and ay == 0:
        return 0
    if ax >= ay:
        return atan[ratio_q8(ay, ax, recip)]
    return 1024 - atan[ratio_q8(ax, ay, recip)]


def emit_u16(name: str, vals, per=12):
    out = [f"static const uint16_t {name}[{len(vals)}] = {{"]
    for i in range(0, len(vals), per):
        out.append("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",")
    out.append("};")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map-inc", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--bank-base", type=int, default=243)
    args = ap.parse_args()

    text = Path(args.map_inc).read_text()
    recip = values(text, "k_tspf_recip8_q16")
    atan = values(text, "k_tspf_atan_q12")
    if len(recip) < 256 or len(atan) < 256:
        raise SystemExit("bearing source tables must contain at least 256 entries")

    outdir = Path(args.out_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    hdr = [
        "#ifndef E1ENV_BEARING_LUT_H",
        "#define E1ENV_BEARING_LUT_H",
        "#include <stdint.h>",
        "#include <gbdk/platform.h>",
        "#define E1ENV_BEARING_LUT 1u",
    ]

    total = 0
    for group in range(8):
        # Rows group*32 .. group*32+30. Local row 31 uses arithmetic fallback.
        rows = range(group * 32, group * 32 + 31)
        table = [
            quadrant_angle(ax, ay, recip, atan)
            for ax in rows
            for ay in range(256)
        ]
        total += len(table) * 2
        fn = f"e1env_bearing_{group}"
        src = [
            f"#pragma bank {args.bank_base + group}",
            "#include <stdint.h>",
            "#include <gbdk/platform.h>",
            "",
            emit_u16("k_bearing", table),
            "",
            f"uint16_t {fn}(uint16_t i) BANKED {{ return k_bearing[i]; }}",
            "",
        ]
        (outdir / f"e1env_bearing_{group}.c").write_text("\n".join(src))
        hdr.append(f"uint16_t {fn}(uint16_t i) BANKED;")

    hdr += ["#endif", ""]
    (outdir / "e1env_bearing_lut.h").write_text("\n".join(hdr))
    print(
        f"E1ENV_BEARING_LUT banks=8 bank_range={args.bank_base}..{args.bank_base+7} "
        f"data_bytes={total} exact_rows=248 fallback_rows=8")


if __name__ == "__main__":
    main()
