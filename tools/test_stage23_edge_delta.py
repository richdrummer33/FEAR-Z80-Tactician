#!/usr/bin/env python3
"""Stage-23 retained edge-delta equivalence stress test.

For same-style/same-clip symmetric FULL columns, compare:
  A) Stage-21 full rematerialization + Stage-18 stale restore
  B) Stage-23 new-edge + newly-entered-fill delta + the SAME stale restore

The persistent name-table result must be identical.
"""
import random

ROWS=18
BASE=[0]*9+[2]+[1]*8
ATTR_VFLIP_PALETTE=0x0C00
EDGE_BASE=0x1000

def sfloor8(v):
    return v//8

def signed_minmax(a,b):
    ra,rb=sfloor8(a),sfloor8(b)
    return (min(ra,rb),max(ra,rb))

def coverage(top_min,clip0,clip1):
    first=max(0,clip0,top_min)
    last=min(17,clip1,17-top_min)
    if first>last:
        return set()
    return set(range(first,last+1))

def edge_word(top_l,top_r,row,shade):
    # Symbolic but injective over the hardware-facing edge state we care about.
    slope=top_r-top_l
    local=top_l-row*8
    return EDGE_BASE | ((shade&3)<<12) | ((slope+7)&0x0f)<<8 | ((local+15)&0x1f)

def full_word(shade,border):
    return 3 + 12*shade + border

def draw_edges(m,top_l,top_r,shade,clip0,clip1):
    tmin,tmax=signed_minmax(top_l,top_r)
    for r in range(max(0,tmin),min(17,tmax)+1):
        if clip0<=r<=clip1:
            w=edge_word(top_l,top_r,r,shade)
            m[r]=w
        br=17-r
        if clip0<=br<=clip1:
            w=edge_word(top_l,top_r,r,shade)
            m[br]=w|ATTR_VFLIP_PALETTE

def draw_full(m,top_l,top_r,shade,border,clip0,clip1):
    tmin,tmax=signed_minmax(top_l,top_r)
    draw_edges(m,top_l,top_r,shade,clip0,clip1)
    first=max(clip0,0,tmax+1)
    last=min(clip1,17,16-tmax)
    if first<=last:
        fw=full_word(shade,border)
        for r in range(first,last+1):
            m[r]=fw
    return coverage(tmin,clip0,clip1)

def stale_restore(m,old_cov,new_cov):
    for r in old_cov-new_cov:
        m[r]=BASE[r]

def draw_delta(m,old_l,old_r,new_l,new_r,shade,border,clip0,clip1):
    _,old_max=signed_minmax(old_l,old_r)
    new_min,new_max=signed_minmax(new_l,new_r)

    # Always emit the new edge vocabulary.
    draw_edges(m,new_l,new_r,shade,clip0,clip1)

    # Only expansion toward the screen edge creates newly-visible interior rows
    # (and converts old edge rows into fill). Contraction is handled by stale restore.
    # Reconcile symmetric row PAIRS. Do not pre-clamp the top row to the
    # aperture: top may be clipped while its mirrored bottom mate is visible.
    first=max(0,new_max+1)
    last=min(8,old_max)
    if first<=last:
        fw=full_word(shade,border)
        for r in range(first,last+1):
            if clip0<=r<=clip1:
                m[r]=fw
            br=17-r
            if clip0<=br<=clip1:
                m[br]=fw

    return coverage(new_min,clip0,clip1)

def main():
    rng=random.Random(0x23ED6E)
    cases=250_000
    expansion=contraction=same_rows=0
    for i in range(cases):
        shade=rng.randrange(3)
        border=rng.randrange(4)
        clip0=rng.randrange(0,10)
        clip1=rng.randrange(max(clip0,8),18)

        old_l=rng.randrange(-56,72)
        old_r=old_l+rng.randrange(-7,8)
        new_l=rng.randrange(-56,72)
        new_r=new_l+rng.randrange(-7,8)

        old_min,old_max=signed_minmax(old_l,old_r)
        new_min,new_max=signed_minmax(new_l,new_r)
        if new_max<old_max: expansion+=1
        elif new_max>old_max: contraction+=1
        else: same_rows+=1

        old_map=BASE.copy()
        old_cov=draw_full(old_map,old_l,old_r,shade,border,clip0,clip1)

        ref=old_map.copy()
        new_cov=draw_full(ref,new_l,new_r,shade,border,clip0,clip1)
        stale_restore(ref,old_cov,new_cov)

        got=old_map.copy()
        got_cov=draw_delta(got,old_l,old_r,new_l,new_r,shade,border,clip0,clip1)
        stale_restore(got,old_cov,got_cov)

        if got!=ref:
            raise SystemExit(
                f"Stage23 mismatch case={i}\n"
                f"old=({old_l},{old_r}) new=({new_l},{new_r}) "
                f"shade={shade} border={border} clip={clip0}..{clip1}\n"
                f"old_cov={sorted(old_cov)} new_cov={sorted(new_cov)}\n"
                f"ref={ref}\ngot={got}"
            )

    print(f"Stage23 edge-delta equivalence: {cases} randomized cases PASS "
          f"(expand={expansion}, contract={contraction}, same-max={same_rows})")

if __name__=="__main__":
    main()
