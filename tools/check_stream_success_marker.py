#!/usr/bin/env python3
"""Validate the terminal framebuffer success oracle in streamed-room ROM output."""
from pathlib import Path
import sys

BLACK = (0, 0, 0)
SUCCESS = (172, 186, 222)  # GG expansion of palette-0 RGB(10,11,13)


def read_ppm(path: Path):
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6":
        raise SystemExit(f"{path}: expected P6 PPM")
    w, h = map(int, parts[1].split())
    if (w, h) != (160, 144):
        raise SystemExit(f"{path}: unexpected dimensions {(w, h)}")
    if parts[2].strip() != b"255":
        raise SystemExit(f"{path}: expected maxval 255")
    pix = parts[3]
    if len(pix) != w * h * 3:
        raise SystemExit(f"{path}: pixel payload size mismatch")
    return w, h, pix


def pixel(pix: bytes, w: int, x: int, y: int):
    i = (y * w + x) * 3
    return tuple(pix[i:i+3])


def main(path: Path) -> int:
    w, _h, pix = read_ppm(path)
    bad = []
    for y in range(8):
        for x in range(8):
            expected = SUCCESS if (x == y or x + y == 7) else BLACK
            actual = pixel(pix, w, x, y)
            if actual != expected:
                bad.append((x, y, expected, actual))
    print(f"STREAM_SUCCESS_MARKER mismatch={len(bad)}/64")
    if bad:
        for row in bad[:8]:
            print(f"mismatch x={row[0]} y={row[1]} expected={row[2]} actual={row[3]}")
        return 1
    print("STREAM_SUCCESS_MARKER_EXACT=1")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} FINAL_FRAME.ppm")
    raise SystemExit(main(Path(sys.argv[1])))
