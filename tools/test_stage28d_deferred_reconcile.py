#!/usr/bin/env python3
"""Stage 28D deferred final-state lifetime reconciliation.

The persistent name-table shadow starts with last frame's retained FULL and
exception output. Current renderers write current retained FULL first and
exception/portal geometry afterwards. Cleanup must remove stale old ownership
WITHOUT temporarily restoring cells that current geometry already owns.

Expected final state is independent of prior-frame ownership:
  base -> current retained -> current exception (exception drawn later wins).
"""
import random

COLS=20
ROWS=18
BASE=[0 if r<9 else (2 if r==9 else 1) for r in range(ROWS)]

def rand_mask(rng, p=45):
    if rng.randrange(100)>=p:
        return 0
    a=rng.randrange(ROWS)
    b=rng.randrange(ROWS)
    if a>b: a,b=b,a
    return ((1<<(b-a+1))-1)<<a

def paint(masks, frame, kind, out):
    tag=(0x4000 if kind=="ret" else 0x6000) | ((frame & 0xff)<<5)
    for c,mask in enumerate(masks):
        for r in range(ROWS):
            if mask & (1<<r):
                out[c][r]=tag|r

def expected(cur_ret,cur_exc,frame):
    out=[BASE[:] for _ in range(COLS)]
    paint(cur_ret,frame,"ret",out)
    paint(cur_exc,frame,"exc",out)
    return out

def main():
    rng=random.Random(0x28D00D)
    frames=150_000
    prev_ret=[0]*COLS
    prev_exc=[0]*COLS
    shadow=[BASE[:] for _ in range(COLS)]

    overlap=stale_ret=stale_exc=0
    for frame in range(1,frames+1):
        cur_ret=[rand_mask(rng,55) for _ in range(COLS)]
        cur_exc=[rand_mask(rng,42) for _ in range(COLS)]

        # Current rendering order: retained common geometry, then exceptions.
        paint(cur_ret,frame,"ret",shadow)
        paint(cur_exc,frame,"exc",shadow)

        # Deferred retained cleanup. Never erase current retained or exception.
        for c in range(COLS):
            stale=prev_ret[c] & ~cur_ret[c]
            for r in range(ROWS):
                bit=1<<r
                if not (stale & bit):
                    continue
                stale_ret += 1
                if cur_exc[c] & bit:
                    overlap += 1
                    continue
                shadow[c][r]=BASE[r]

        # Existing exception cleanup runs after retained cleanup and similarly
        # must not erase current retained or current exception state.
        for c in range(COLS):
            stale=prev_exc[c] & ~cur_exc[c]
            for r in range(ROWS):
                bit=1<<r
                if not (stale & bit):
                    continue
                stale_exc += 1
                if cur_ret[c] & bit:
                    continue
                shadow[c][r]=BASE[r]

        want=expected(cur_ret,cur_exc,frame)
        if shadow!=want:
            for c in range(COLS):
                if shadow[c]!=want[c]:
                    raise SystemExit(
                        f"mismatch frame={frame} col={c}\n"
                        f"got ={shadow[c]}\nwant={want[c]}\n"
                        f"prev_ret={prev_ret[c]:05x} prev_exc={prev_exc[c]:05x}\n"
                        f"cur_ret ={cur_ret[c]:05x} cur_exc ={cur_exc[c]:05x}"
                    )
            raise SystemExit(f"mismatch frame={frame}")

        prev_ret=cur_ret
        prev_exc=cur_exc

    print(
        f"Stage28D deferred final reconciliation: {frames} frames PASS "
        f"(stale-ret={stale_ret}, protected-overlap={overlap}, "
        f"stale-exc={stale_exc})"
    )

if __name__=="__main__":
    main()
