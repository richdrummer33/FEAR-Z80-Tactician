#!/usr/bin/env python3
"""Fail if any copy of a duplicated fixed-point helper has drifted.

The ratio helper existed verbatim in two places -- the polar renderer and the
lattice floor-light runtime spliced in by an apply script. When the wrap-to-zero
bug was fixed in one, a corrected bug survived in the other. Sharing a single
implementation is awkward because the copies live in separately banked
translation units emitted by different generators, so this enforces the weaker
but checkable property: every copy must carry the saturating form, and no copy
may reintroduce the bare cast.

This is a lint over source text, not a behavioural test; the behavioural vectors
live in tools/rung1/helper_vectors.h and run inside the geometry reference gate.
Both exist because either alone would have missed this bug: the vectors only
cover the copy they are compiled against, and the lint only covers the shape.
"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIP = {".git", "build", ".toolchain"}

# the tail of the ratio helper, in any copy
BARE = re.compile(r"return\s*\(uint8_t\)\s*q\s*;")
SAT  = re.compile(r"return\s*\(uint8_t\)\s*\(\s*q\s*>\s*255u?\s*\?\s*255u?\s*:\s*q\s*\)\s*;")
BODY = re.compile(r"ratio_q8_sat\s*\([^)]*\)\s*\{(.*?)\n\}", re.S)
# the old name as CODE (a call or definition), not a historical mention in prose
OLDNAME = re.compile(r"ratio_q8_exact\s*\(")

def sources():
    for p in ROOT.rglob("*"):
        if p.is_dir() or any(s in p.parts for s in SKIP):
            continue
        if p.suffix in (".c", ".h", ".s", ".py"):
            yield p

fail = []
copies = 0
for p in sources():
    if p.name == Path(__file__).name:
        continue
    text = p.read_text(errors="ignore")
    if OLDNAME.search(text):
        fail.append(f"{p.relative_to(ROOT)}: calls or defines ratio_q8_exact; the helper "
                    f"saturates and that name invites removing the clamp")
    for m in BODY.finditer(text):
        copies += 1
        body = m.group(1)
        if BARE.search(body) and not SAT.search(body):
            fail.append(f"{p.relative_to(ROOT)}: a copy of the ratio helper returns "
                        f"(uint8_t)q with no saturation -- n == d wraps 256 to 0")
        elif not SAT.search(body):
            fail.append(f"{p.relative_to(ROOT)}: a copy of the ratio helper does not "
                        f"carry the recognised saturating return; check it by hand")

print(f"checked {copies} copies of the ratio helper across the tree")
for f in fail:
    print(f"HELPER_DRIFT {f}")
if fail:
    sys.exit(1)
print("DUPLICATED_HELPERS_OK every copy saturates; no stale names")
