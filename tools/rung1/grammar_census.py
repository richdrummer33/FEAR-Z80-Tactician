#!/usr/bin/env python3
"""Is the trajectory vocabulary a list, or a grammar?

Panel 8 of the adjudication dashboard showed that `up 2`, `up 3` and `up 4` each
have out-degree exactly one: a multi-row jump is always followed by a descent.
That is not what a free alphabet looks like. If short context makes most
transitions deterministic, the 432 retained trajectories may be surface forms of
a much smaller machine, and the right representation may be an automaton and an
initial state rather than a packed list of programs.

This measures it three ways, and takes no position on which representation wins:

  1  Short-context entropy. H(next | last k moves) for k = 0..4, both unweighted
     (the language's own structure) and weighted by chunk instances (what a
     compressor would actually see). Reported with the share of contexts whose
     successor is already unique.

  2  The minimal automaton. A trie over the 432 sequences accepts exactly that
     language; partition refinement (Moore) minimises it. The resulting state
     count is the honest answer to "how small is the machine", because it is
     invariant: no encoding can do better while accepting exactly this set.

  3  Byte cost of each candidate, so the comparison is concrete rather than
     rhetorical.

Usage: grammar_census.py <dir with traj_moves.csv>
"""
import csv, sys
from collections import Counter, defaultdict
from math import log2
from pathlib import Path

LABEL = ["down 1","next col","up 1","up 2","up 3","up 4","END","wrap"]

def load(d):
    seqs=[]
    for r in csv.DictReader(open(Path(d)/"traj_moves.csv")):
        sl=tuple(int(v) for v in r["slots"].split() if v!="-1")
        if sl: seqs.append((sl,int(r["count"])))
    return seqs

def entropy(seqs,k,weighted):
    """H(next | previous k moves), in bits. Contexts shorter than k at the start
    of a sequence are padded with a distinct start symbol, so the first move is
    conditioned on 'nothing yet' rather than silently dropped."""
    ctx=defaultdict(Counter)
    START=-1
    for sl,cnt in seqs:
        w=cnt if weighted else 1
        pad=(START,)*k+sl
        for i in range(len(sl)):
            ctx[pad[i:i+k]][sl[i]]+=w
    tot=sum(sum(c.values()) for c in ctx.values())
    h=0.0; det=0
    for c in ctx.values():
        n=sum(c.values())
        if len(c)==1: det+=n
        for v in c.values():
            if v: h+=(v/tot)*(-log2(v/n))
    return h,len(ctx),100.0*det/tot

def minimal_dfa(seqs):
    """Trie accepting exactly these sequences, then Moore partition refinement.
    States are numbered; -1 is the implicit dead state, which every string not in
    the language falls into and which never needs storing."""
    kids=[{}]; acc=[False]
    for sl,_ in seqs:
        s=0
        for a in sl:
            if a not in kids[s]:
                kids.append({}); acc.append(False); kids[s][a]=len(kids)-1
            s=kids[s][a]
        acc[s]=True
    n=len(kids)
    alpha=sorted({a for k in kids for a in k})
    # Moore partition refinement. The termination test is that the block count
    # stopped growing; comparing the label vectors directly is wrong, because the
    # blocks are renumbered every pass and can compare equal without having
    # converged, or unequal after they have.
    part=[1 if acc[s] else 0 for s in range(n)]
    blocks=max(part)+1
    while True:
        sig={}; newpart=[0]*n; nxt=0
        for s in range(n):
            # -1 stands for the implicit dead state, which distinguishes states
            # that differ only in which symbols they reject. Real block ids are
            # non-negative, so it can never collide with one.
            key=(part[s],)+tuple(part[kids[s][a]] if a in kids[s] else -1 for a in alpha)
            if key not in sig: sig[key]=nxt; nxt+=1
            newpart[s]=sig[key]
        if nxt==blocks: break
        part=newpart; blocks=nxt
    states=blocks
    trans={}
    for s in range(n):
        for a,t2 in kids[s].items(): trans[(part[s],a)]=part[t2]
    accept={part[s] for s in range(n) if acc[s]}
    return n,states,len(trans),len(alpha),part,trans,accept

def verify_dfa(seqs,part,trans,accept):
    """The minimised machine must accept every retained trajectory and nothing
    else. Acceptance is checked directly; 'nothing else' is checked by counting
    the distinct accepted strings as paths through the DAG and requiring that it
    equal the number of trajectories."""
    start=part[0]
    for sl,_ in seqs:
        s=start
        for a in sl:
            if (s,a) not in trans: return False,"rejects a retained trajectory",0
            s=trans[(s,a)]
        if s not in accept: return False,"does not accept a retained trajectory",0
    succ={}
    for (s,a),t2 in trans.items(): succ.setdefault(s,[]).append(t2)
    memo={}
    def paths(s,depth=0):
        if depth>256: raise RuntimeError("cycle: the language is not finite")
        if s in memo: return memo[s]
        n=1 if s in accept else 0
        for t2 in succ.get(s,[]): n+=paths(t2,depth+1)
        memo[s]=n; return n
    total=paths(start)
    return total==len(seqs),f"accepts {total} distinct strings",total

def main():
    d=sys.argv[1] if len(sys.argv)>1 else "build/rung1_dense"
    seqs=load(d)
    nmoves=sum(len(s) for s,_ in seqs)
    inst=sum(c for _,c in seqs)
    print(f"trajectory grammar census over {len(seqs)} retained trajectories")
    print(f"  {nmoves} moves total, {inst:,} chunk instances behind them\n")

    print("1  short-context entropy of the next move")
    print(f"  {'context':<10} {'contexts':>9}  {'H unweighted':>13} {'det%':>7}   {'H weighted':>11} {'det%':>7}")
    for k in range(5):
        hu,nu,du=entropy(seqs,k,False)
        hw,nw,dw=entropy(seqs,k,True)
        name="order 0" if k==0 else f"last {k}"
        print(f"  {name:<10} {nu:>9}  {hu:>13.4f} {du:>6.2f}%   {hw:>11.4f} {dw:>6.2f}%")
    print("  (det% is the share of transitions whose context already determines them)")

    print("\n2  first-order out-degree, the Panel 8 observation in numbers")
    out=defaultdict(Counter); first=Counter()
    for sl,cnt in seqs:
        first[sl[0]]+=cnt
        for a,b in zip(sl,sl[1:]): out[a][b]+=cnt
    for a in sorted(out):
        tgt=", ".join(f"{LABEL[b]} {100*v/sum(out[a].values()):.1f}%" for b,v in out[a].most_common())
        print(f"  {LABEL[a]:<9} out-degree {len(out[a])}   {tgt}")
    ends=Counter()
    for sl,cnt in seqs: ends[sl[-1]]+=cnt
    print(f"  starts: "+", ".join(f"{LABEL[a]} {100*v/sum(first.values()):.1f}%" for a,v in first.most_common()))
    print(f"  ends:   "+", ".join(f"{LABEL[a]} {100*v/sum(ends.values()):.1f}%" for a,v in ends.most_common()))

    print("\n3  the minimal automaton accepting exactly these trajectories")
    trie,states,trans,alpha,part,tmap,acc=minimal_dfa(seqs)
    ok,why,naccept=verify_dfa(seqs,part,tmap,acc)
    print(f"  trie states           {trie}")
    print(f"  minimal DFA states    {states}")
    print(f"  transitions           {trans}   over an alphabet of {alpha}")
    print(f"  reduction             {trie/states:.1f}x from the trie, "
          f"{len(seqs)/states:.1f} trajectories per state")
    print(f"  verified              {'OK' if ok else 'FAILED'}: {why}, "
          f"expected {len(seqs)}")
    if not ok: sys.exit("minimal_dfa does not accept exactly the retained set")

    print("\n4  byte cost of the three candidate representations")
    packed=(nmoves*3+7)//8
    idx=len(seqs)*3
    print(f"  packed trajectories   {packed:>6} B move payload + {idx} B index = {packed+idx} B")
    print(f"  minimal automaton     {trans*2:>6} B at 2 B per transition "
          f"(next state + symbol), {states} states")
    print(f"  closed-form walker    {0:>6} B of table; the raster shape is computed "
          f"from (iq, step, family, length)")
    print("\n  none of these is chosen here. The automaton number is the one that")
    print("  says how much state the raster walker actually has.")

    dda_section()

# --------------------------------------------------------------------------
# 5  what the raster walker's state actually is
# --------------------------------------------------------------------------
def shape_py(iq,step,fam,want,is_last):
    """The closed-form raster shape, transcribed from shape_of() in
    pose_to_raster_check.c. Returns the move sequence, or None if the chunk
    leaves the uint8 depth domain or needs a move outside the family."""
    ncols = want+1 if is_last else want+2
    for c in range(ncols):
        raw=(iq+c*step+32)>>6
        if raw<0 or raw>255: return None
    def y(c):
        h=(((iq+c*step+32)>>6)&0xFF)>>1
        return 71-h if fam==0 else 72+h
    lo=[];hi=[]
    for c in range(want if is_last else want+1):
        a,b=y(c),y(c+1)
        lo.append(min(a,b)>>3); hi.append(max(a,b)>>3)
    mv=[]
    for c in range(want):
        mv.extend([0]*(hi[c]-lo[c]))          # 0 = down one row
        if is_last and c==want-1: mv.append(6); break   # 6 = run terminator
        jump=lo[c+1]-hi[c]
        if jump>0 or jump<-4: return None
        mv.append(1 if jump==0 else 1-jump)   # 1=next col, 2..5 = next col up 1..4
    return tuple(mv)

def dda_section():
    """The move sequence is a function of (a mod 1024, step, length, family),
    where a = iq + 32 is the depth accumulator. That is provable, not just
    observed:

        h(c) = (iq + c*step + 32) >> 7          (the mask is a no-op in domain)
        y(c) = 71 - h(c)     (top)  or  72 + h(c)  (bottom)
        row(c) = y(c) >> 3

    Adding 1024 to a shifts h by exactly 8 and therefore every row by exactly 1.
    The moves are row DIFFERENCES, so they are unchanged. The walker's entire
    state is a 10-bit accumulator phase and a step: this is a Bresenham/DDA line
    walker, at exactly the precision the renderer permits.

    Checked here by construction, with no map data and no sampled poses."""
    print("\n5  what the raster walker's state actually is")
    print("   claim: the move sequence depends on iq only through (iq+32) mod 1024")
    tested=comparable=0
    bad=[]
    for step in list(range(-2000,2001,7)):
        for want in range(1,7):
            for fam in (0,2):
                for is_last in (0,1):
                    for a0 in range(0,1024,13):
                        base=shape_py(a0-32,step,fam,want,is_last)
                        tested+=1
                        for off in (1024,2048,4096,8192):
                            other=shape_py(a0-32+off,step,fam,want,is_last)
                            if base is None or other is None: continue
                            comparable+=1
                            if base!=other and len(bad)<5:
                                bad.append((a0,step,want,fam,is_last,off,base,other))
    print(f"   {comparable:,} in-domain comparisons across step, length, family,")
    print(f"   terminator and accumulator phase")
    if bad:
        for b in bad: print(f"   MISMATCH a0={b[0]} step={b[1]} want={b[2]} fam={b[3]} +{b[5]}")
        sys.exit("the accumulator-phase claim is false")
    print("   no mismatch: the state is a 10-bit accumulator phase plus the step.")
    print("   The 432 trajectories are surface forms of a DDA line walker, which")
    print("   is why the closed-form walker needs no table at all.")

if __name__=="__main__":
    main()
