#!/usr/bin/env python3
"""Exact semantic host-vs-Game-Gear regression for thick porthole proof frames."""
from pathlib import Path
import sys

HOST = {
    (0, 0, 0): 0,
    (16, 16, 48): 1,
    (64, 64, 96): 2,
    (96, 112, 144): 3,
    (144, 160, 192): 4,
    (208, 224, 240): 5,
}
GG = {
    (0, 0, 0): 0,
    (16, 16, 49): 1,
    (32, 32, 49): 2,
    (49, 68, 98): 3,
    (98, 117, 156): 4,
    (172, 186, 222): 5,
}

PAIRS = (
    ("bundle0_route01_frame80.ppm", "native-interior-frame80.ppm"),
    ("bundle0_route01_frame112.ppm", "native-interior-frame112.ppm"),
    ("bundle1_route01_frame80.ppm", "native-exterior-frame80.ppm"),
    ("bundle1_route01_frame112.ppm", "native-exterior-frame112.ppm"),
)


def read_ppm(path: Path):
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6":
        raise SystemExit(f"{path}: expected P6 PPM")
    w, h = map(int, parts[1].split())
    if parts[2].strip() != b"255":
        raise SystemExit(f"{path}: expected maxval 255")
    pix = parts[3]
    if len(pix) != w * h * 3:
        raise SystemExit(f"{path}: pixel payload size mismatch")
    return w, h, pix


def semantics(pix: bytes, palette, path: Path):
    out = bytearray(len(pix) // 3)
    for i in range(0, len(pix), 3):
        rgb = (pix[i], pix[i + 1], pix[i + 2])
        if rgb not in palette:
            raise SystemExit(f"{path}: unknown RGB {rgb}")
        out[i // 3] = palette[rgb]
    return out


def main(host_dir: Path, runtime_dir: Path) -> int:
    total = 0
    for host_name, runtime_name in PAIRS:
        hp = host_dir / host_name
        gp = runtime_dir / runtime_name
        hw, hh, hpix = read_ppm(hp)
        gw, gh, gpix = read_ppm(gp)
        if (hw, hh) != (gw, gh):
            raise SystemExit(f"{host_name}: dimensions {(hw, hh)} != {(gw, gh)}")
        hs = semantics(hpix, HOST, hp)
        gs = semantics(gpix, GG, gp)
        mismatches = sum(a != b for a, b in zip(hs, gs))
        total += mismatches
        print(
            f"PORTHOLE_SEMANTIC_COMPARE host={host_name} runtime={runtime_name} "
            f"mismatch={mismatches}/{len(hs)} "
            f"percent={100.0 * mismatches / len(hs):.6f}"
        )
    print(f"PORTHOLE_SEMANTIC_EXACT={int(total == 0)} total_mismatch={total}")
    return 0 if total == 0 else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} HOST_DIR RUNTIME_DIR")
    raise SystemExit(main(Path(sys.argv[1]), Path(sys.argv[2])))
