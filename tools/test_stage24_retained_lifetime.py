#!/usr/bin/env python3
"""Stage-24 retained FULL lifetime separation equivalence.

Compare the old generic coverage-owner model with:
  - retained FULL old/new range reconciliation before current materialization;
  - exception-only Stage-18 coverage;
  - stale exception restore suppressed where a CURRENT retained FULL owns a row.

Covers FULL->FULL, FULL->exception/empty, exception->FULL, and exception churn.
"""
import random

ROWS=18
BASE=[0]*9+[2]+[1]*8
ATTR_VFLIP_PALETTE=0x0C00
EDGE_BASE=0x1000
EXC_BASE=0x6000

def mm(l,r):
    a,b=l//8,r//8
    return min(a,b),max(a,b)

def full_cov(l,r,c0,c1):
    tmin,_=mm(l,r)
    first=max(0,c0,tmin)
    last=min(17,c1,17-tmin)
    return set(range(first,last+1)) if first<=last else set()

def edge_word(l,r,row,shade):
    return EDGE_BASE|((shade&3)<<12)|(((r-l)+7)&15)<<8|((l-row*8+15)&31)

def draw_full(m,d):
    l,r,shade,border,c0,c1=d
    tmin,tmax=mm(l,r)
    for row in range(max(0,tmin),min(17,tmax)+1):
        w=edge_word(l,r,row,shade)
        if c0<=row<=c1: m[row]=w
        br=17-row
        if c0<=br<=c1: m[br]=w|ATTR_VFLIP_PALETTE
    lo=max(0,c0,tmax+1)
    hi=min(17,c1,16-tmax)
    if lo<=hi:
        fw=3+12*shade+border
        for row in range(lo,hi+1): m[row]=fw
    return full_cov(l,r,c0,c1)

def exc_cov(a,b):
    return set(range(a,b+1)) if a<=b else set()

def draw_exc(m,a,b,tag):
    cov=exc_cov(a,b)
    for row in cov: m[row]=EXC_BASE|(tag<<5)|row
    return cov

def restore(m,rows,protect=frozenset()):
    for row in rows:
        if row not in protect: m[row]=BASE[row]

def rand_full(rng):
    l=rng.randrange(-56,72)
    r=rng.randrange(max(-56,l-7),min(71,l+7)+1)
    shade=rng.randrange(3)
    border=rng.randrange(4)
    c0=rng.randrange(0,10)
    c1=rng.randrange(max(c0,8),18)
    return (l,r,shade,border,c0,c1)

def main():
    rng=random.Random(0x24C0DE)
    cases=200_000
    counts={}
    for i in range(cases):
        old_kind=rng.choice(("none","full","exc"))
        new_kind=rng.choice(("none","full","exc"))
        counts[(old_kind,new_kind)]=counts.get((old_kind,new_kind),0)+1

        old_full=rand_full(rng) if old_kind=="full" else None
        new_full=rand_full(rng) if new_kind=="full" else None
        old_exc=(rng.randrange(0,18),rng.randrange(0,18)) if old_kind=="exc" else None
        new_exc=(rng.randrange(0,18),rng.randrange(0,18)) if new_kind=="exc" else None
        if old_exc and old_exc[0]>old_exc[1]: old_exc=(old_exc[1],old_exc[0])
        if new_exc and new_exc[0]>new_exc[1]: new_exc=(new_exc[1],new_exc[0])

        # Build the previous persistent shadow.
        old_map=BASE.copy()
        old_generic_cov=set()
        if old_full:
            old_generic_cov=draw_full(old_map,old_full)
        elif old_exc:
            old_generic_cov=draw_exc(old_map,*old_exc,1)

        # Reference: all geometry participates in one generic coverage lifetime.
        ref=old_map.copy()
        cur_generic_cov=set()
        if new_full:
            cur_generic_cov=draw_full(ref,new_full)
        elif new_exc:
            cur_generic_cov=draw_exc(ref,*new_exc,2)
        restore(ref,old_generic_cov-cur_generic_cov)

        # Stage24 model: FULL owns its own retained lifetime; generic coverage
        # contains exceptions only.
        got=old_map.copy()
        old_full_cov=full_cov(*old_full[:2],old_full[4],old_full[5]) if old_full else set()
        new_full_cov=full_cov(*new_full[:2],new_full[4],new_full[5]) if new_full else set()
        old_exc_cov=exc_cov(*old_exc) if old_exc else set()
        new_exc_cov=exc_cov(*new_exc) if new_exc else set()

        # FULL stale state is reconciled BEFORE current non-FULL/portal work.
        if old_full:
            if new_full:
                restore(got,old_full_cov-new_full_cov)
            else:
                restore(got,old_full_cov)

        if new_full:
            draw_full(got,new_full)
        elif new_exc:
            draw_exc(got,*new_exc,2)

        # Exception stale cleanup must not clobber a new retained FULL.
        restore(got,old_exc_cov-new_exc_cov,protect=new_full_cov)

        if got!=ref:
            raise SystemExit(
                f"Stage24 mismatch case={i} {old_kind}->{new_kind}\n"
                f"old_full={old_full} new_full={new_full} "
                f"old_exc={old_exc} new_exc={new_exc}\n"
                f"ref={ref}\ngot={got}"
            )

    summary=", ".join(f"{a}->{b}:{n}" for (a,b),n in sorted(counts.items()))
    print(f"Stage24 retained lifetime equivalence: {cases} randomized transitions PASS ({summary})")

if __name__=="__main__":
    main()
