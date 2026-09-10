"""Cadence sweep over the A29 temporal-boundary probe.

The A29 corpus ran at U=4, four 60 Hz motion ticks between rendered states,
which is what the CURRENT 194,761 T materializer can afford. That is the wrong
workload to design the temporal renderer against, because the optimization is a
feedback loop: a faster renderer sees a smaller camera delta, which crosses
fewer boundaries, which produces fewer events, which makes it faster again.

So sweep U and report the same metrics, and bucket by |dyaw| as well since the
cadence only matters through the pose delta it produces. At the shipped turn
rate of 3 yaw units per tick, U=1..4 is dyaw 3, 6, 9, 12, or 4.22 to 16.88
degrees.

usage: python3 tools/temporal_cadence_sweep.py [all|<regime index>]
"""
import subprocess, re, sys
BIN="./build/temporal_boundary_probe"
def run(U, frames, yaw, reg):
    a=[BIN,str(U),str(frames),str(yaw)]
    if reg is not None: a.append(str(reg))
    out=subprocess.run(a,capture_output=True,text=True).stdout
    g=lambda p,d=None: (re.search(p,out).group(1) if re.search(p,out) else d)
    return dict(
      miss=g(r'NOT in the dirty set\s+(\d+)'),
      selfchk=g(r're-derivation != renderer output\s+(\d+)'),
      rowwr=g(r'materializer performs\s+([\d.]+)'),
      dirty=g(r'marks dirty\s+([\d.]+)\s+/update\s+\(([\d.]+)%'),
      dirtyp=g(r'marks dirty\s+[\d.]+\s+/update\s+\(([\d.]+)%'),
      floorp=g(r'ACTUALLY changed \(exact floor\)\s+[\d.]+\s+/update\s+\(([\d.]+)%'),
      p95=g(r'dirty cells  mean [\d.]+   p95 (\d+)'),
      mx=g(r'dirty cells  mean [\d.]+   p95 \d+   max (\d+)'),
      mean=g(r'dirty cells  mean ([\d.]+)'),
      colmat=g(r'kernel does now\s+([\d.]+)'),
      skip=g(r'fully skippable.*?([\d.]+)%'),
      edge=g(r'edge-only.*?([\d.]+)%'),
      full=g(r'full derivation.*?([\d.]+)%'),
      nochg=g(r'NO_CHANGE\s+\d+\s+([\d.]+)%'),
      pshift=g(r'PHASE_SHIFT\s+\d+\s+([\d.]+)%'),
      pramp=g(r'PHASE_RAMP\s+\d+\s+([\d.]+)%'),
      ecross=g(r'EDGE_CROSS\s+\d+\s+([\d.]+)%'),
      slope=g(r'SLOPE_CHANGE\s+\d+\s+([\d.]+)%'),
      vcol=g(r'V_COLUMN_SHIFT\s+\d+\s+([\d.]+)%'),
      fallb=g(r'FALLBACK\s+\d+\s+([\d.]+)%'),
      grow=g(r'GROW events ([\d.]+)'),
      shrink=g(r'SHRINK ([\d.]+)'),
      ops=g(r'ops per update  mean ([\d.]+)'),
      opsp95=g(r'ops per update  mean [\d.]+  p95 (\d+)'),
      subcell=g(r'no column crossed\)\s+([\d.]+)%'),
      uni=g(r'all four boundaries\s+([\d.]+)%'),
      unitop=g(r'TOP edge alone\s+([\d.]+)%'),
      rsbg=g(r'background \(ceiling/floor/horizon\)\s+([\d.]+)%'),
      rsknown=g(r'ALREADY in retained state\s+([\d.]+)%'),
      rsnew=g(r'appeared this update\s+([\d.]+)%'),
      vac=g(r'total vacated cells\s+([\d.]+)'),
      underlay=g(r'contributed underneath last update\s+([\d.]+)%'),
      incr=g(r'`tile_id \+= small delta`\s+([\d.]+)%'),
      attr=g(r'attribute bits \(FLIPX/FLIPY/PAL\) equal\s+([\d.]+)%'),
      dcol=re.search(r'\|dcolumns\|:(.*)', out).group(1).strip() if re.search(r'\|dcolumns\|:',out) else '',
    )
reg = None if len(sys.argv)<2 or sys.argv[1]=='all' else int(sys.argv[1])
label = "ALL REGIMES" if reg is None else f"REGIME {reg}"
rows=[]
for U in range(1,9):
    rows.append((U, run(U, 240, 64, reg)))
print(f"=== CADENCE SWEEP: {label} ===")
print(f"{'U':>2} {'dirty%':>7} {'floor%':>7} {'skip%':>6} {'edge%':>6} {'full%':>6} "
      f"{'mean':>6} {'p95':>5} {'max':>5} {'vcol%':>6} {'slope%':>7} {'ecross%':>7} "
      f"{'pshift%':>7} {'nochg%':>7} {'ops':>6} {'vac':>7} {'uni%':>6} {'miss':>5}")
for U,d in rows:
    print(f"{U:>2} {d['dirtyp']:>7} {d['floorp']:>7} {d['skip']:>6} {d['edge']:>6} {d['full']:>6} "
          f"{d['mean']:>6} {d['p95']:>5} {d['mx']:>5} {d['vcol']:>6} {d['slope']:>7} {d['ecross']:>7} "
          f"{d['pshift']:>7} {d['nochg']:>7} {d['ops']:>6} {d['vac']:>7} {d['uni']:>6} {d['miss']:>5}")
print()
print(f"{'U':>2} {'subcell%':>9} {'|dcol| distribution':<52} {'restore bg%':>11} {'known%':>7} {'new%':>6} {'incr%':>6}")
for U,d in rows:
    print(f"{U:>2} {d['subcell']:>9} {d['dcol'][:52]:<52} {d['rsbg']:>11} {d['rsknown']:>7} {d['rsnew']:>6} {d['incr']:>6}")
