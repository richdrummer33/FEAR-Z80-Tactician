#!/usr/bin/env python3
"""Rebuild the lightweight front-envelope header/dispatcher around prebaked C payloads.

Used only to accelerate renderer iteration: the immutable quarter-cell index and
program C arrays come from an earlier successful bake artifact, while this script
re-emits the tiny ABI wrapper expected by the current renderer.
"""
from __future__ import annotations
import argparse,re
from pathlib import Path

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--dir",required=True)
    ap.add_argument("--dispatch-bank",type=int,default=31)
    args=ap.parse_args()
    d=Path(args.dir)

    idx=sorted(d.glob("e1env_idx_*.c"))
    prog=sorted(d.glob("e1env_prog_*.c"))
    if not idx or not prog:
        raise SystemExit("missing prebaked index/program C sources")

    counts=[]
    for p in prog:
        t=p.read_text()
        m=re.search(r"static const uint16_t k_off\[(\d+)\]",t)
        if not m: raise SystemExit(f"no k_off count in {p}")
        counts.append(int(m.group(1)))
    total=sum(counts)
    if total!=421:
        raise SystemExit(f"program-count drift: {total}")

    hdr=[
      "#ifndef E1ENV_GENERATED_H",
      "#define E1ENV_GENERATED_H",
      "#include <stdint.h>",
      "#include <gbdk/platform.h>",
      "#define E1ENV_WORLD_MIN_X 16",
      "#define E1ENV_WORLD_MIN_Y 24",
      "#define E1ENV_CELL_Q4 4u",
      "#define E1ENV_COLS 384u",
      "#define E1ENV_ROWS 224u",
      "#define E1ENV_ROWS_PER_INDEX_BANK 16u",
      "#define E1ENV_PROGRAM_COUNT 421u",
      "#define E1ENV_FALLBACK 65535u",
      "#define E1ENV_MAX_PROGRAM_BYTES 64u",
    ]
    for p in idx:
        fn=p.stem
        hdr.append(f"uint16_t {fn}(uint16_t i) BANKED;")
    for p in prog:
        fn=p.stem
        hdr.append(f"uint8_t {fn}(uint16_t local, uint8_t *dst) BANKED;")
    hdr += [
      "uint8_t e1env_fetch_program_q4(int16_t xq,int16_t yq,uint8_t *dst) BANKED;",
      "#endif",""
    ]
    (d/"e1env_generated.h").write_text("\n".join(hdr))

    dsp=[
      f"#pragma bank {args.dispatch_bank}",
      "#include <stdint.h>",
      "#include <gbdk/platform.h>",
      '#include "e1env_generated.h"',
      "",
      "uint8_t e1env_fetch_program_q4(int16_t xq,int16_t yq,uint8_t *dst) BANKED {",
      "    int16_t rx=(int16_t)(xq-(E1ENV_WORLD_MIN_X<<4));",
      "    int16_t ry=(int16_t)(yq-(E1ENV_WORLD_MIN_Y<<4));",
      "    uint16_t gx,gy,local,pid=E1ENV_FALLBACK;",
      "    uint8_t band;",
      "    if(rx<0||ry<0) return 0xffu;",
      "    gx=(uint16_t)rx>>2; gy=(uint16_t)ry>>2;",
      "    if(gx>=E1ENV_COLS||gy>=E1ENV_ROWS) return 0xffu;",
      "    local=(uint16_t)(((gy&15u)*E1ENV_COLS)+gx);",
      "    band=(uint8_t)(gy>>4);",
      "    switch(band) {",
    ]
    for i,p in enumerate(idx):
        dsp.append(f"    case {i}u: pid={p.stem}(local); break;")
    dsp += [
      "    default: return 0xffu;",
      "    }",
      "    if(pid==E1ENV_FALLBACK) return 0xffu;",
    ]
    base=0
    for i,(p,n) in enumerate(zip(prog,counts)):
        end=base+n
        prefix="if" if i==0 else "else if"
        dsp.append(f"    {prefix}(pid<{end}u) return {p.stem}((uint16_t)(pid-{base}u),dst);")
        base=end
    dsp += ["    return 0xffu;","}",""]
    (d/"e1env_dispatch.c").write_text("\n".join(dsp))
    print(f"E1ENV_REHEADER_PASS index_banks={len(idx)} program_banks={len(prog)} programs={total} dispatch_bank={args.dispatch_bank}")

if __name__=="__main__":
    main()
