#!/usr/bin/env python3
"""Cycle-exact PC attribution of the dispatcher after run-edge invariant hoist."""
from __future__ import annotations
import pathlib, subprocess, sys
import z80_progjoin_bench as pj
import z80_progjoin_tune_bench as tune
import z80_progjoin_edge_hoist_audit as eh

ROOT=pathlib.Path(__file__).resolve().parents[1]
BASE_BUILD=tune.BASE_BUILD; TUNED_BUILD=tune.TUNED_BUILD
ORIG_ASSEMBLE=pj.assemble; ORIG_Z80=pj.Z80
LABELS={}; PROF={}


def kernel(C, stepmap, desc, thresh, blocks, bodies):
    s=eh.hoisted_kernel(C,stepmap,desc,thresh,blocks,bodies)
    anchors=[
      (f"        ld hl,(0x{pj.V_IQ:04x})            ; a = iq + 32", "ph_phase"),
      (f"        ld hl,(0x{eh.V_THPTR:04x})           ; hoisted THRESH + slot*8", "ph_thresh"),
      ("        ld b,0                       ; rank = #thresholds <= u, sorted so", "ph_rank"),
      (f"        ld hl,(0x{pj.V_ACC:04x})           ; base = (a >> 7) & mask", "ph_base"),
      (f"        ld hl,(0x{eh.V_DESCPTR:04x})         ; hoisted slot+family slab", "ph_desc"),
      (f"        ld a,(0x{pj.V_BASE:04x})           ; entry = base*16 + rank*2", "ph_entry"),
      (f"        ld e,(hl)\n        inc hl\n        ld d,(hl)\n        ld hl,0x{bodies:04x}", "ph_body"),
      ("        ld a,(hl)                      ; selected count is body byte 0", "ph_join"),
    ]
    for text,label in anchors:
        if text not in s: raise RuntimeError(f"anchor changed: {label}")
        s=s.replace(text,f"{label}:\n{text}",1)
    return s

STAGES=[
 ("want/min","edge_loop","ph_phase"),
 ("phase/u","ph_phase","ph_thresh"),
 ("threshold ptr","ph_thresh","ph_rank"),
 ("rank scan","ph_rank","ph_base"),
 ("base extract","ph_base","ph_desc"),
 ("descriptor","ph_desc","ph_entry"),
 ("block entry","ph_entry","ph_body"),
 ("body pointer","ph_body","ph_join"),
 ("count/join","ph_join","wp_loop"),
]

def assem(src,base=0):
    global LABELS
    c,l=ORIG_ASSEMBLE(src,base); LABELS=l; return c,l
class Z(ORIG_Z80):
    def _step(self):
        pc=self.pc;t=self.t;ORIG_Z80._step(self);d=self.t-t
        for nm,a,b in STAGES:
            if LABELS[a] <= pc < LABELS[b]: PROF[nm]=PROF.get(nm,0)+d; break

def merge(src,dst):
    for k,v in src.items():
      if k in ("region","blob"):
        dd=dst.setdefault(k,{})
        for kk,vv in v.items(): dd[kk]=(max(dd.get(kk,0),vv) if k=="blob" else dd.get(kk,0)+vv)
      elif k in ("code","C"): dst[k]=v
      elif k=="tables": dst[k]=max(dst.get(k,0),v)
      else: dst[k]=dst.get(k,0)+v

def main():
    nwin=int(sys.argv[1]) if len(sys.argv)>1 else 63; wpose=int(sys.argv[2]) if len(sys.argv)>2 else 40
    bake=ROOT/"build"/"edge_progjoin_bake"; oracle=ROOT/"build"/"coverage_pose_oracle.txt"; acc={}; splits=[0]
    def do(start,npose):
      if npose<1:return
      r=subprocess.run([str(bake),str(oracle),"6",str(npose),str(BASE_BUILD),str(start)],capture_output=True,text=True)
      if r.returncode: print(r.stdout,r.stderr); raise SystemExit("bake failed")
      try:
        tune.transform_tables(BASE_BUILD,TUNED_BUILD)
        pj.BUILD=TUNED_BUILD;pj.kernel_source=kernel;pj.assemble=assem;pj.Z80=Z
        tmp={};pj.run_window(tmp,quiet=True)
      except pj.Overflow:
        if npose==1: raise
        h=npose//2;splits[0]+=1;do(start,h);do(start+h,npose-h);return
      finally:
        pj.BUILD=BASE_BUILD;pj.kernel_source=tune.BASE_KERNEL;pj.assemble=ORIG_ASSEMBLE;pj.Z80=ORIG_Z80
      merge(tmp,acc)
    for w in range(nwin):do(w*wpose,wpose)
    chunks=acc["chunks"]; broad=acc["region"]["dispatch"]; tot=sum(PROF.values())
    print("=== PROGJOIN POST-HOIST DISPATCH PROFILE ===")
    print(f"run-edges {acc['edges']:,}  chunks {chunks:,}  oracle faults {acc['wrong_edges']}")
    for nm,_,_ in STAGES:
      v=PROF.get(nm,0);print(f"  {nm:16} {v/chunks:8.1f} T/chunk  {100*v/max(tot,1):5.1f}%")
    print(f"  {'TOTAL':16} {tot/chunks:8.1f} T/chunk")
    print(f"  broad crosscheck  {broad/chunks:8.1f} T/chunk")
    return 0 if tot==broad and acc['wrong_edges']==0 and acc['wrong_cells']==0 and acc['stray']==0 else 1
if __name__=='__main__':raise SystemExit(main())
