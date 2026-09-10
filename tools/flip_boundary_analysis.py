#!/usr/bin/env python3
"""Are depth-order flip boundaries AFFINE half-planes?

If a flipping pair's two sign regions are linearly separable in local (lx,ly),
the runtime never has to sort that pair: the baker emits one selector of the
class already used for visibility GATEs (`a*lx + b*ly + c >= 0`) and the
interpreter evaluates a sign test it can already do in ~860 T.

EXACT TEST, NOT A FITTED ONE
----------------------------
Two finite point sets are linearly separable exactly when their convex hulls
are disjoint. That is decided here with a monotone-chain hull plus the
separating-axis theorem over both hulls' edge normals - a decision procedure
with a yes/no answer.

A fitted classifier (perceptron, SVM) was deliberately NOT used: failure to
converge is not proof of inseparability, and this project has already been
bitten once by a test that could only ever report success.

Ties ('=') are excluded from both sets. A tie means equal depth, so either
assignment is a valid convention - forcing it to a side would manufacture a
failure that does not exist.

Also reports whether a single separator serves ALL yaw slices of a pair. A
separator that moves with yaw is not expressible as a selector in (lx,ly)
alone, which is the difference between "precompile the order" and "still need
a runtime decision".

    make flip-boundary
"""
from __future__ import annotations
import collections
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def hull(pts):
    """Andrew monotone chain. Returns hull vertices CCW."""
    pts = sorted(set(pts))
    if len(pts) <= 2:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lo = []
    for p in pts:
        while len(lo) >= 2 and cross(lo[-2], lo[-1], p) <= 0:
            lo.pop()
        lo.append(p)
    up = []
    for p in reversed(pts):
        while len(up) >= 2 and cross(up[-2], up[-1], p) <= 0:
            up.pop()
        up.append(p)
    return lo[:-1] + up[:-1]


def _axes(poly):
    if len(poly) == 1:
        return []
    if len(poly) == 2:
        dx, dy = poly[1][0] - poly[0][0], poly[1][1] - poly[0][1]
        return [(-dy, dx)]
    out = []
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        out.append((-(b[1] - a[1]), b[0] - a[0]))
    return out


def separable(A, B):
    """Exact linear separability of two integer point sets in 2D.

    Returns (bool, axis). Disjoint convex hulls <=> separable. Uses SAT over
    both hulls' edge normals, which is complete for convex polygons."""
    if not A or not B:
        return True, None
    ha, hb = hull(A), hull(B)
    for ax in _axes(ha) + _axes(hb):
        pa = [ax[0] * x + ax[1] * y for x, y in ha]
        pb = [ax[0] * x + ax[1] * y for x, y in hb]
        if max(pa) < min(pb) or max(pb) < min(pa):
            return True, ax
    return False, None


def main():
    path = ROOT / "build" / "flip_maps.txt"
    if not path.exists():
        raise SystemExit(f"missing {path} - run `make flip-boundary`")

    per_pair = collections.defaultdict(list)
    for line in path.read_text().splitlines():
        if not line.startswith("PAIR "):
            continue
        _, gx, gy, ka, kb, yaw, m = line.split(maxsplit=6)
        per_pair[(int(gx), int(gy), int(ka), int(kb))].append((int(yaw), m))

    multi = sum(1 for v in per_pair.values() if len(v) > 1)
    slices_sep = slices_tot = 0
    pair_all_sep = pair_any_fail = 0
    pair_one_sep_all_yaw = 0
    axis_examples = []

    for key, entries in per_pair.items():
        allsep = True
        common = None
        common_ok = True
        for yaw, m in entries:
            A = [(i % 64, i // 64) for i, c in enumerate(m) if c == '<']
            B = [(i % 64, i // 64) for i, c in enumerate(m) if c == '>']
            ok, ax = separable(A, B)
            slices_tot += 1
            if ok:
                slices_sep += 1
                if len(axis_examples) < 5 and ax:
                    axis_examples.append((key, yaw, ax, len(A), len(B)))
            else:
                allsep = False
            # does ONE axis serve every yaw slice of this pair?
            if ok and ax is not None:
                if common is None:
                    common = ax
                else:
                    pa = [common[0] * x + common[1] * y for x, y in A] or [0]
                    pb = [common[0] * x + common[1] * y for x, y in B] or [0]
                    if not (max(pa) < min(pb) or max(pb) < min(pa)):
                        common_ok = False
            else:
                common_ok = False
        if allsep:
            pair_all_sep += 1
            if common_ok and len(entries) > 1:
                pair_one_sep_all_yaw += 1
        else:
            pair_any_fail += 1

    print("=== ARE DEPTH-ORDER FLIP BOUNDARIES AFFINE? ===")
    print("exact convex-hull separability over the full 64x64 local grid\n")
    print(f"two-sign slices tested        {slices_tot}")
    print(f"  linearly separable          {slices_sep}   "
          f"({100.0 * slices_sep / max(1, slices_tot):.2f}%)")
    print(f"  NOT separable               {slices_tot - slices_sep}   "
          f"({100.0 * (slices_tot - slices_sep) / max(1, slices_tot):.2f}%)\n")
    n = len(per_pair)
    print(f"distinct flipping pairs       {n}")
    print(f"  with >1 yaw slice           {multi}   "
          f"({100.0 * multi / max(1, n):.1f}%)  <- only these actually TEST")
    print(f"                                  whether one axis serves all yaws")
    print(f"  every yaw slice separable   {pair_all_sep}   "
          f"({100.0 * pair_all_sep / max(1, n):.2f}%)")
    print(f"  ...AND one axis serves all  {pair_one_sep_all_yaw}   "
          f"(of {multi} multi-slice pairs: "
          f"{100.0 * pair_one_sep_all_yaw / max(1, multi):.2f}%)")
    print(f"     ^ only these are expressible as ONE baked selector in (lx,ly).")
    print(f"       The rest need a per-yaw selector, or a runtime decision.")
    print(f"  at least one slice fails    {pair_any_fail}   "
          f"({100.0 * pair_any_fail / max(1, n):.2f}%)")
    if axis_examples:
        print("\nexample separating normals (a,b) for `a*lx + b*ly + c`:")
        for key, yaw, ax, na, nb in axis_examples:
            print(f"  cell({key[0]:2d},{key[1]:2d}) keys {key[2]:3d}/{key[3]:3d} "
                  f"yaw {yaw:3d}  axis {ax}   |A|={na} |B|={nb}")


if __name__ == "__main__":
    main()
