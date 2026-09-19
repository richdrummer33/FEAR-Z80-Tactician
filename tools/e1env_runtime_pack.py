#!/usr/bin/env python3
"""Post-process restored exact-Q4 envelope tables for the hot runtime.

This deliberately does NOT rebake topology. It preserves every program ID,
boundary vertex and record size, but uses the three spare high bits of each
owner byte (surface IDs are <= 29) for facts the Z80 otherwise rediscovers:

  bit 5: left envelope boundary is a physical endpoint of this wall
  bit 6: right envelope boundary is a physical endpoint of this wall
  bit 7: right endpoint is shared by the next wall; suppress this duplicate
         border and let the next wall own the single connected-corner line

NO_WALL remains 0xff.

The generated dispatcher is also given a one-program cache. Exact Q4 positions
change every movement tick, but topology program IDs are spatially coherent;
if the newly fetched PID matches the previous PID, the already-copied WRAM
program is reused and the second banked program-load call is skipped.
"""
from __future__ import annotations
import argparse
import re
from pathlib import Path

PACKED_MACRO = "E1ENV_PACKED_OWNER_FLAGS"


def numbers(body: str):
    return [int(x, 0) for x in re.findall(r"-?0x[0-9A-Fa-f]+|-?\d+", body)]


def array(text: str, name: str):
    m = re.search(
        r"static\s+const\s+[^;=]+?\b" + re.escape(name)
        + r"\s*\[[^\]]+\]\s*=\s*\{(.*?)\};",
        text, re.S)
    if not m:
        raise SystemExit(f"missing array {name}")
    return numbers(m.group(1))


def emit_array(ctype: str, name: str, vals, per: int):
    out = [f"static const {ctype} {name}[{len(vals)}] = {{"]
    for i in range(0, len(vals), per):
        out.append("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",")
    out.append("};")
    return "\n".join(out)


def pack_program_sources(gen: Path):
    hdr = gen / "e1env_generated.h"
    htext = hdr.read_text()
    if PACKED_MACRO in htext:
        print("owner flags already packed")
        return 0, 0

    mtext = (gen / "optimized_renderer_map_data.inc").read_text()
    keys = array(mtext, "k_tspf_keys")
    endpoints = [((w >> 5) & 31, (w >> 10) & 31) for w in keys]

    packed_spans = 0
    joined = 0
    for p in sorted(gen.glob("e1env_prog_*.c")):
        text = p.read_text()
        bank_m = re.search(r"#pragma bank\s+(\d+)", text)
        fn_m = re.search(r"uint8_t\s+(e1env_prog_\d+)\s*\(", text)
        if not bank_m or not fn_m:
            raise SystemExit(f"cannot parse {p}")
        bank = int(bank_m.group(1))
        fn = fn_m.group(1)
        offsets = array(text, "k_off")
        stream = array(text, "k_stream")

        for off in offsets:
            n = stream[off]
            pairs = [
                (stream[off + 1 + 2 * i], stream[off + 2 + 2 * i])
                for i in range(n)
            ]
            for i, (v0, sid) in enumerate(pairs):
                if sid == 0xff:
                    continue
                if sid >= len(endpoints):
                    raise SystemExit(f"{p}: invalid surface id {sid}")
                v1 = pairs[(i + 1) % n][0]
                next_sid = pairs[(i + 1) % n][1]
                a, b = endpoints[sid]
                left_phys = (v0 == a or v0 == b)
                right_phys = (v1 == a or v1 == b)
                suppress_right = False
                if right_phys and next_sid != 0xff:
                    na, nb = endpoints[next_sid]
                    suppress_right = (v1 == na or v1 == nb)

                owner = sid
                if left_phys:
                    owner |= 0x20
                if right_phys:
                    owner |= 0x40
                if suppress_right:
                    owner |= 0x80
                    joined += 1
                stream[off + 2 + 2 * i] = owner
                packed_spans += 1

        src = [
            f"#pragma bank {bank}",
            "#include <stdint.h>",
            "#include <gbdk/platform.h>",
            "",
            emit_array("uint16_t", "k_off", offsets, 12),
            "",
            emit_array("uint8_t", "k_stream", stream, 20),
            "",
            f"""uint8_t {fn}(uint16_t local, uint8_t *dst) BANKED {{
    uint16_t off=k_off[local];
    uint8_t n=k_stream[off], bytes=(uint8_t)(1u+(uint8_t)(n<<1)), i;
    for(i=0u;i<bytes;++i) dst[i]=k_stream[off+i];
    return n;
}}""",
            "",
        ]
        p.write_text("\n".join(src))

    needle = "#define E1ENV_MAX_PROGRAM_BYTES 64u"
    if needle not in htext:
        raise SystemExit("generated header missing E1ENV_MAX_PROGRAM_BYTES")
    hdr.write_text(htext.replace(
        needle, needle + f"\n#define {PACKED_MACRO} 1u"))
    return packed_spans, joined


def cache_dispatch(gen: Path):
    p = gen / "e1env_dispatch.c"
    text = p.read_text()
    if "g_e1env_last_pid" in text:
        print("dispatcher PID cache already installed")
        return

    inc = '#include "e1env_generated.h"'
    if inc not in text:
        raise SystemExit("cannot locate dispatcher generated header include")
    text = text.replace(
        inc, inc + "\n\nstatic uint16_t g_e1env_last_pid=E1ENV_FALLBACK;",
        1)

    needle = "    if(pid==E1ENV_FALLBACK) return 0xffu;"
    if needle not in text:
        raise SystemExit("cannot locate dispatcher fallback gate")
    text = text.replace(
        needle,
        needle
        + "\n    if(pid==g_e1env_last_pid) return dst[0];"
        + "\n    g_e1env_last_pid=pid;",
        1)
    p.write_text(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--generated-dir", required=True)
    args = ap.parse_args()
    gen = Path(args.generated_dir)
    packed, joined = pack_program_sources(gen)
    cache_dispatch(gen)
    print(
        f"E1ENV_RUNTIME_PACK spans={packed} connected_right_suppressed={joined} "
        "pid_cache=1")


if __name__ == "__main__":
    main()
