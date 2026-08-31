#!/usr/bin/env python3
"""Validate the terminal framebuffer success oracle in streamed-room ROM output."""
from pathlib import Path
import sys


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


def main(path: Path) -> int:
    w, h, pix = read_ppm(path)
    bad = 0
    min_r = min_b = 255
    max_g = 0
    for i in range(0, len(pix), 3):
        r, g, b = pix[i], pix[i + 1], pix[i + 2]
        min_r = min(min_r, r)
        min_b = min(min_b, b)
        max_g = max(max_g, g)
        if r < 240 or g > 16 or b < 240:
            bad += 1
    print(
        f"STREAM_SUCCESS_MAGENTA bad={bad}/{w*h} "
        f"min_r={min_r} max_g={max_g} min_b={min_b}"
    )
    if bad:
        return 1
    print("STREAM_SUCCESS_MARKER_EXACT=1")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} FINAL_FRAME.ppm")
    raise SystemExit(main(Path(sys.argv[1])))
