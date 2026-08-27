#!/usr/bin/env python3
import re
import sys
from pathlib import Path

PHASES = [
    ("loop", r"^  loop\s+avg=\s*([0-9.]+) T"),
    ("active", r"^  active-work\s+avg=\s*([0-9.]+) T"),
    ("render", r"^  render/build\s+avg=\s*([0-9.]+) T"),
    ("vram", r"^  VRAM-upload\s+avg=\s*([0-9.]+) T"),
]
SUB = [
    "clear/lifetime", "q4-transform", "candidate-build", "solid-raster",
    "portal-control", "portal-face", "retained-life",
]

def parse(path):
    text = Path(path).read_text()
    m = re.search(r"TileSector Gearsystem profile: loops=(\d+)", text)
    if not m:
        raise SystemExit(f"missing loop count in {path}")
    loops = int(m.group(1))
    out = {"loops": loops}
    hm = re.search(r"^  map-hash\s+frames=(\d+) fnv64=([0-9A-Fa-f]+)$", text, re.M)
    if hm:
        out["map_hash_frames"] = int(hm.group(1))
        out["map_hash"] = hm.group(2).upper()
    for key, pat in PHASES:
        m = re.search(pat, text, re.M)
        if m:
            out[key] = float(m.group(1))
    for name in SUB:
        m = re.search(
            rf"^\s+{re.escape(name)}\s+total=\s*(\d+) T",
            text, re.M
        )
        if m:
            out[name] = float(m.group(1)) / loops
    return out

def main():
    if len(sys.argv) < 4 or (len(sys.argv)-1) % 3:
        raise SystemExit(
            "usage: compare_profile_matrix.py LABEL CONTROL.txt REPAIR.txt "
            "[LABEL CONTROL.txt REPAIR.txt ...]"
        )
    rows = []
    for i in range(1, len(sys.argv), 3):
        label, cpath, rpath = sys.argv[i:i+3]
        c, r = parse(cpath), parse(rpath)
        if "map_hash" in c or "map_hash" in r:
            if c.get("map_hash_frames") != r.get("map_hash_frames"):
                raise SystemExit(
                    f"{label}: map-hash frame-count mismatch "
                    f"{c.get('map_hash_frames')} != {r.get('map_hash_frames')}"
                )
            if c.get("map_hash") != r.get("map_hash"):
                raise SystemExit(
                    f"{label}: ROM name-table sequence mismatch "
                    f"{c.get('map_hash')} != {r.get('map_hash')}"
                )
        rows.append((label, c, r))

    keys = ["loop","active","render","clear/lifetime","candidate-build",
            "solid-raster","portal-control","portal-face","retained-life","vram"]
    print("PROFILE MATRIX DELTA: repair - Stage23 control")
    for label, c, r in rows:
        print(f"\n[{label}]")
        if c.get("map_hash"):
            print(f"  map-equivalence   PASS  frames={c['map_hash_frames']} hash={c['map_hash']}")
        for key in keys:
            if key not in c and key not in r:
                continue
            cv, rv = c.get(key, 0.0), r.get(key, 0.0)
            d = rv - cv
            pct = (100.0*d/cv) if cv else 0.0
            print(f"  {key:16s} {cv:9.1f} -> {rv:9.1f}  "
                  f"delta={d:+9.1f} T  {pct:+6.2f}%")

if __name__ == "__main__":
    main()
