#!/usr/bin/env python3
"""TEMP_BOUNDARY: the Z80 A/B for A29/A30's temporal boundary representation.

WHY A NEW ORACLE
----------------
Every materializer bench so far verifies ONE pose against
`coverage_pose_oracle.txt`. A temporal kernel carries state ACROSS poses, so a
single-pose oracle cannot see the mechanism at all - the same mistake A26 had
to fix when coverage state started crossing runs. The unit of verification here
is a consecutive SEQUENCE of poses along a real trajectory, dumped by
`temporal_boundary_probe.c` at a chosen cadence.

    make temporal-bench

WHAT THIS FILE ESTABLISHES FIRST
--------------------------------
Before any temporal kernel exists, the sequence oracle itself has to be proved.
`BASELINE_DDA` renders every pose of the sequence from scratch with the
unmodified DDA_G kernel and must reproduce the dumped name table exactly. If it
does not, the oracle's run parameters are wrong and nothing built on it means
anything. That check runs first and hard-fails.

The A/B that follows compares, on the identical sequence:

  BASELINE_DDA     - DDA_G, full render every pose, map re-initialized
  TEMP_BOUNDARY_A  - map persists, retained per-column boundary state, columns
                     whose state is unchanged are skipped entirely

One mechanism, one rung, as A24/A26/A27 were done.
"""
from __future__ import annotations
import pathlib
import statistics as stt
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from z80core import assemble                            # noqa: E402
import z80_materialize_masked_bench as mb               # noqa: E402
import z80_materialize_dda_bench as dda                 # noqa: E402

CODE = 0x0000
ROWS, COLS = mb.ROWS, mb.COLS


def load_sequence(path, max_traj=None):
    """Parse the pose-sequence oracle into a list of trajectories."""
    trajs, cur = [], None
    for line in path.read_text().splitlines():
        f = line.split()
        if not f:
            continue
        new_traj, n = int(f[0]), int(f[1])
        runs, keys, p = [], [], 2
        for _ in range(n):
            runs.append(tuple(int(v) for v in f[p:p + 8]))
            keys.append(int(f[p + 8]))
            p += 9
        want = [int(v) for v in f[p:]]
        assert len(want) == ROWS * COLS, (len(want), n)
        if new_traj or cur is None:
            cur = []
            trajs.append(cur)
            if max_traj and len(trajs) > max_traj:
                trajs.pop()
                break
        cur.append((runs, keys, want))
    return [t for t in trajs if len(t) >= 2]


def main():
    dump = ROOT / "build" / "temporal_seq_oracle.txt"
    if not dump.exists():
        raise SystemExit(f"missing {dump} - run `make temporal-bench` "
                         "(it emits the oracle first)")
    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    trajs = load_sequence(dump, limit)
    poses = sum(len(t) for t in trajs)
    if not poses:
        raise SystemExit("sequence oracle has no usable trajectory")

    low, high = mb.tables()
    bgm = mb.background_map()

    code, _ = assemble(dda.SRC_DDA, CODE)
    img = bytearray(0x10000)
    img[CODE:CODE + len(code)] = code
    img[mb.LOWTAB:mb.LOWTAB + len(low)] = low
    img[mb.HIGHTAB:mb.HIGHTAB + len(high)] = high

    print(f"sequence oracle: {len(trajs)} trajectories, {poses} poses, "
          f"{poses - len(trajs)} consecutive pairs")

    fails, ts, cols = 0, [], 0
    for t in trajs:
        for runs, _keys, want in t:
            tt, got = mb.run_pose(img, runs, False, bgm)
            ts.append(tt)
            cols += sum(r[3] - r[2] + 1 for r in runs)
            if got != want:
                fails += 1
                if fails <= 2:
                    bad = [i for i in range(ROWS * COLS) if got[i] != want[i]]
                    print(f"  MISMATCH cells {len(bad)} first "
                          f"r{bad[0] // COLS} c{bad[0] % COLS} "
                          f"want {want[bad[0]]} got {got[bad[0]]}")
    if fails:
        raise SystemExit(
            f"FAIL: the sequence oracle does not match DDA_G on "
            f"{fails}/{poses} poses. The oracle's run parameters are wrong; "
            f"fix that before building any temporal kernel on it.")

    print(f"ORACLE VERIFIED: DDA_G reproduces all {poses} poses exactly")
    print(f"BASELINE_DDA   {stt.mean(ts):9.1f} T/update   "
          f"{cols / poses:.2f} columns/update")
    return 0


if __name__ == "__main__":
    sys.exit(main())
