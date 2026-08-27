#!/usr/bin/env python3
"""Stage-28 sparse exception lifetime equivalence.

Reference:
  Stage-24 exception-only coverage with a dense 20-column end scan.

Candidate:
  20-bit previous/current active masks; current 18-bit coverage is lazily
  cleared only on a column's first exception mark, and only active columns are
  reconciled at end-frame.

The persistent symbolic name-table result and previous exception coverage must
match over long randomized multi-frame sequences, including retained-FULL
protection against stale exception restores.
"""
import random

COLS = 20
ROWS = 18
BASE = tuple([0] * 9 + [2] + [1] * 8)
FULL_TAG = 0x4000
EXC_TAG = 0x6000


def span_mask(a, b):
    if a > b:
        a, b = b, a
    return ((1 << (b - a + 1)) - 1) << a


def rows(mask):
    return [r for r in range(ROWS) if mask & (1 << r)]


def main():
    rng = random.Random(0x28EAC7)
    frames = 100_000

    dense_prev = [0] * COLS
    sparse_prev = [0] * COLS
    sparse_cur_storage = [0x3FFFF] * COLS
    sparse_prev_active = 0

    dense_map = [list(BASE) for _ in range(COLS)]
    sparse_map = [list(BASE) for _ in range(COLS)]

    active_sum = 0
    stale_sum = 0

    for frame in range(frames):
        marks = [[] for _ in range(COLS)]
        full = [0] * COLS

        for c in range(COLS):
            if rng.randrange(100) < 38:
                for _ in range(1 + rng.randrange(3)):
                    a = rng.randrange(ROWS)
                    b = rng.randrange(ROWS)
                    marks[c].append((a, b))

            if rng.randrange(100) < 55:
                a = rng.randrange(0, 10)
                b = rng.randrange(max(a, 8), ROWS)
                full[c] = span_mask(a, b)

        dense_cur = [0] * COLS
        sparse_cur_active = 0

        for c in range(COLS):
            for r in rows(full[c]):
                dense_map[c][r] = FULL_TAG | r
                sparse_map[c][r] = FULL_TAG | r

            first_sparse_mark = True
            for a, b in marks[c]:
                m = span_mask(a, b)
                dense_cur[c] |= m

                if first_sparse_mark:
                    sparse_cur_storage[c] = 0
                    sparse_cur_active |= 1 << c
                    first_sparse_mark = False

                sparse_cur_storage[c] |= m

                for r in rows(m):
                    dense_map[c][r] = EXC_TAG | ((frame & 0xFF) << 5) | r
                    sparse_map[c][r] = EXC_TAG | ((frame & 0xFF) << 5) | r

        active_sum += sparse_cur_active.bit_count()

        for c in range(COLS):
            stale = dense_prev[c] & ~dense_cur[c]
            stale_sum += stale.bit_count()
            for r in rows(stale):
                if not (full[c] & (1 << r)):
                    dense_map[c][r] = BASE[r]
            dense_prev[c] = dense_cur[c]

        union = sparse_prev_active | sparse_cur_active
        for c in range(COLS):
            bit = 1 << c
            if not (union & bit):
                continue

            prev = sparse_prev[c] if (sparse_prev_active & bit) else 0
            cur = sparse_cur_storage[c] if (sparse_cur_active & bit) else 0
            stale = prev & ~cur

            for r in rows(stale):
                if not (full[c] & (1 << r)):
                    sparse_map[c][r] = BASE[r]

            sparse_prev[c] = cur

        sparse_prev_active = sparse_cur_active

        if sparse_prev != dense_prev:
            raise SystemExit(
                f"coverage mismatch frame={frame}\n"
                f"dense={dense_prev}\nsparse={sparse_prev}"
            )

        if sparse_map != dense_map:
            for c in range(COLS):
                if sparse_map[c] != dense_map[c]:
                    raise SystemExit(
                        f"map mismatch frame={frame} col={c}\n"
                        f"dense={dense_map[c]}\nsparse={sparse_map[c]}\n"
                        f"marks={marks[c]} full={full[c]:05x}"
                    )
            raise SystemExit(f"map mismatch frame={frame}")

    print(
        "Stage28 sparse exception lifetime: "
        f"{frames} randomized multi-frame transitions PASS "
        f"(avg current exception columns={active_sum/frames:.2f}, "
        f"avg stale cells={stale_sum/frames:.2f})"
    )


if __name__ == "__main__":
    main()
