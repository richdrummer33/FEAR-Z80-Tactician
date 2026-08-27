#!/usr/bin/env python3
"""Stage-27 candidate reciprocal phase-accumulator equivalence.

Stage 12 computes each column's depth byte as:
  next = current + step
  q    = high8( 2 * (current + next) )
       = high8( 4*current + 2*step )

Therefore initialize:
  phase = 4*current + 2*step
  delta = 4*step
and each subsequent q is simply high8(phase), phase += delta.

All arithmetic is modulo 16 bits, exactly matching the Z80.
"""
import random

def u16(v): return v & 0xffff

def baseline(inv,step,n):
    out=[]
    cur=u16(inv)
    step=u16(step)
    for _ in range(n):
        nxt=u16(cur+step)
        x=u16(nxt+cur)
        x=u16(x<<1)
        out.append((x>>8)&0xff)
        cur=nxt
    return out

def phase(inv,step,n):
    inv=u16(inv)
    step=u16(step)
    p=u16((inv<<2)+(step<<1))
    d=u16(step<<2)
    out=[]
    for _ in range(n):
        out.append((p>>8)&0xff)
        p=u16(p+d)
    return out

def main():
    rng=random.Random(0x27FA5E)
    for i in range(1_000_000):
        inv=rng.randrange(0x10000)
        step=rng.randrange(0x10000)
        n=rng.randrange(1,21)
        a=baseline(inv,step,n)
        b=phase(inv,step,n)
        if a!=b:
            raise SystemExit(f"Stage27 mismatch case={i} inv={inv:04x} step={step:04x} n={n}\nold={a}\nnew={b}")
    print("Stage27 candidate phase accumulator: 1000000 randomized sequences PASS")

if __name__=="__main__":
    main()
