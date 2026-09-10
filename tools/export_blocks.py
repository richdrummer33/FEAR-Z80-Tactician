#!/usr/bin/env python3
"""Emit every cell's baked block program as a flat text file.

The fused renderer is written in C so it can share the SHIPPED renderer's
own project_key/draw_run/materializer rather than a second transcription of
them. That leaves the block program as the one thing it needs from the
Python baker, so it crosses the boundary as data, exported here.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from span_block_bake import (  # noqa: E402
    load, build_block, OP_SPAN, OP_GATE, OP_END,
)
from local_projection_field_poc import GRID_W, GRID_H  # noqa: E402


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/blocks.txt"
    if len(sys.argv) > 2:
        import span_block_bake
        span_block_bake.ORDER_MODE = sys.argv[2]
        print(f"order mode: {sys.argv[2]}")
    d = load()
    n = recs_total = 0
    with open(out, "w") as f:
        for gy in range(GRID_H):
            for gx in range(GRID_W):
                ops, _ = build_block(d, gx, gy)
                if ops is None:
                    continue
                recs = []
                for op in ops:
                    if op[0] == OP_END:
                        break
                    if op[0] == OP_GATE:
                        recs.append(("G", op[1]))
                    elif op[0] == OP_SPAN:
                        recs.append(("S", op[1]))
                    else:
                        recs.append(("C", op[1]))
                f.write(f"CELL {gx} {gy} {len(recs)}\n")
                for kind, v in recs:
                    f.write(f"{kind} {v}\n")
                n += 1
                recs_total += len(recs)
    print(f"exported {n} cell blocks, {recs_total} instructions -> {out}")


if __name__ == "__main__":
    main()
