#!/usr/bin/env python3
"""Stage-25 active-mask stale-column equivalence.

The old Stage-24 finalizer scanned all 20 retained columns and used generation
tags to find prior FULL columns that were not refreshed. Stage 25 carries the
same lifetime set as a 20-bit active mask and visits only stale bits.
"""
import random

MASK=(1<<20)-1

def stage24(prev_full, cur_full, replacement):
    # replacement = depth-0 non-FULL columns, retired before they draw.
    retired_pre=prev_full & replacement
    prev_after=prev_full & ~replacement
    stale_final=prev_after & ~cur_full
    return retired_pre|stale_final, cur_full & MASK

def stage25(prev_active, cur_active, replacement):
    retired=prev_active & replacement
    prev_active &= ~replacement
    stale=prev_active & ~cur_active
    retired |= stale
    prev_active=cur_active & MASK
    return retired & MASK, prev_active

def main():
    rng=random.Random(0x25AC71)
    for i in range(500_000):
        prev=rng.getrandbits(20)&MASK
        # Foreground FULL and foreground non-FULL are mutually exclusive.
        cur=rng.getrandbits(20)&MASK
        repl=(rng.getrandbits(20)&MASK) & ~cur
        a=stage24(prev,cur,repl)
        b=stage25(prev,cur,repl)
        if a!=b:
            raise SystemExit(f"Stage25 mismatch {i}: prev={prev:05x} cur={cur:05x} repl={repl:05x} old={a} new={b}")
    print("Stage25 active-mask lifetime: 500000 randomized transitions PASS")

if __name__=="__main__":
    main()
