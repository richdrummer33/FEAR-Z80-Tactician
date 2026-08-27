#!/usr/bin/env python3
"""Stage-26 candidate endpoint-envelope proof.

For a candidate span overlapping one existing uniform winner segment:
- safe reject if existing quantized depth is strictly nearer at BOTH endpoints.
- safe replace if candidate is >=2 quantized reciprocal units nearer at BOTH endpoints.
Otherwise fall back to the exact Stage-12 per-column rule.

The +2 win margin is deliberate: ties keep the existing winner.
"""
import random

def qmid(inv, step):
    return ((inv + (inv + step)) >> 7) & 0xff

def seq(inv, step, n):
    out=[]
    cur=inv
    for _ in range(n):
        out.append(qmid(cur,step))
        cur+=step
    return out

def classify(oldq,newq):
    if oldq[0] > newq[0] and oldq[-1] > newq[-1]:
        return "reject"
    d0=newq[0]-oldq[0]
    d1=newq[-1]-oldq[-1]
    if d0>=2 and d1>=2:
        return "replace"
    return "fallback"

def main():
    rng=random.Random(0x26E11E)
    counts={"reject":0,"replace":0,"fallback":0}
    for i in range(1_000_000):
        n=rng.randint(1,20)
        old_inv=rng.randint(8,240)<<6
        new_inv=rng.randint(8,240)<<6
        old_step=rng.randint(-384,384)
        new_step=rng.randint(-384,384)

        # Keep values in the unsigned reciprocal domain over this tiny span.
        old_vals=[old_inv+old_step*k for k in range(n+1)]
        new_vals=[new_inv+new_step*k for k in range(n+1)]
        if min(old_vals)<0 or max(old_vals)>0xffff or min(new_vals)<0 or max(new_vals)>0xffff:
            continue

        oldq=seq(old_inv,old_step,n)
        newq=seq(new_inv,new_step,n)
        mode=classify(oldq,newq)
        counts[mode]+=1

        if mode=="reject":
            # Candidate must never strictly beat existing; ties are existing wins.
            if any(nq>oq for oq,nq in zip(oldq,newq)):
                raise SystemExit(f"unsafe reject case={i} old={oldq} new={newq}")
        elif mode=="replace":
            if any(nq<=oq for oq,nq in zip(oldq,newq)):
                raise SystemExit(f"unsafe replace case={i} old={oldq} new={newq}")

    total=sum(counts.values())
    print(f"Stage26 endpoint envelope: {total} randomized uniform overlaps PASS "
          f"(reject={counts['reject']}, replace={counts['replace']}, fallback={counts['fallback']})")

if __name__=="__main__":
    main()
