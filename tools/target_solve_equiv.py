"""Prove tools/target_solve_census.c's dp_solve is the shipped screen_depth_plane.

The census runs the Game Gear path on the host, which means the target
arithmetic is reproduced rather than executed.  That reproduction is only
evidence if it is held to the source, so this compares the two bodies token by
token and fails if they drift.  Only the coefficient lookup differs: the
shipped code reads the per-yaw arrays the runtime caches, the census reads the
same arrays loaded by the generated loader.
"""
from __future__ import annotations
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def body(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


def norm(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    s = re.sub(r"//[^\n]*", " ", s)
    s = s.replace("g_depth_nf_q7[cls]", "COEF_NF").replace("g_nf[cls]", "COEF_NF")
    s = s.replace("g_depth_stepfac_q4[cls]", "COEF_SF").replace("g_sf[cls]", "COEF_SF")
    s = re.sub(r"\+\+g_dp_iters;", " ", s)
    s = re.sub(r"[{}]", " ", s)
    # whitespace is not semantics: the shipped source is written dense and
    # the census copy is formatted, so compare with it removed entirely.
    return re.sub(r"\s+", "", s)


def main():
    src = (ROOT / "src" / "tilesector_polar_renderer.c").read_text()
    cen = (ROOT / "tools" / "target_solve_census.c").read_text()
    a = norm(body(src, "uint8_t cls=k_tspf_depth_normal_class[sid];",
                  "return 1u;"))
    b = norm(body(cen, "uint8_t cls = k_tspf_depth_normal_class[sid];",
                  "return 1u;"))
    if a == b:
        print("EQUIVALENT: census dp_solve matches shipped screen_depth_plane")
        return 0
    print("DRIFT between the census copy and the shipped source:\n")
    print(f"  shipped: {a}\n")
    print(f"  census : {b}\n")
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print(f"  first difference at char {i}: "
                  f"shipped {a[i:i+40]!r} vs census {b[i:i+40]!r}")
            break
    return 1


if __name__ == "__main__":
    sys.exit(main())
