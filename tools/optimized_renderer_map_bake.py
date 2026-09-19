#!/usr/bin/env python3
"""Bake a renderer-friendly four-room Game Gear test course.

The map is deliberately designed around what the live Polar renderer does well:
long cardinal spans, low candidate counts, stable FULL walls, and hard occlusion
between "feature rooms".  It still exercises every vertical profile, a true
window (LINTEL + RISER on the same XY span), and two discrete floor steps.

No selector fitting is needed: each 4-world-unit recipe cell belongs to one of
four visibility regions, each with a small hand-authored candidate list.
"""
from __future__ import annotations
import argparse, math, re
from pathlib import Path

# 16 vertices is the native packed-key limit (4 bits per endpoint).
V=[
 (16,16),(56,16),(96,16),(136,16),(176,16),
 (176,80),(136,80),(96,80),(56,80),(16,80),
 (56,42),(56,54),(96,42),(96,54),(136,42),(136,54),
]

FULL,LINTEL,RAISED,RISER=0,1,2,3

# sid, name, v0, v1, profile, shade_bias
# Canonical direction is clockwise as seen from the room for outer surfaces.
S=[
 (0,"full_gallery_top",0,1,FULL,0),
 (1,"window_upper_lintel",1,2,LINTEL,0),
 (2,"window_lower_riser",1,2,RISER,0),
 (3,"raised_gallery_top",2,3,RAISED,1),
 (4,"lintel_gallery_top",3,4,LINTEL,-1),
 (5,"long_bottom_full",5,9,FULL,0),
 (6,"left_outer_full",9,0,FULL,0),
 (7,"right_outer_full",4,5,FULL,0),
 (8,"divider1_upper",1,10,FULL,0),
 (9,"divider1_lower",11,8,FULL,0),
 (10,"divider2_upper",2,12,FULL,0),
 (11,"divider2_lower",13,7,FULL,0),
 (12,"divider3_upper",3,14,FULL,0),
 (13,"divider3_lower",15,6,FULL,0),
 (14,"step_up_riser",12,13,RISER,1),
 (15,"step_down_riser",14,15,RISER,-1),
]
assert [q[0] for q in S]==list(range(len(S)))
assert len(S)<=17  # retained boundary store in the current renderer

# Each region lists directed (sid,v0,v1) keys. Shared divider surfaces reverse
# endpoint direction on the far side without changing sid, so retained state
# still identifies the same physical wall and each surface is drawn once/frame.
REGIONS=[
 # Room A: deliberately boring FULL gallery.
 [(0,0,1),(5,5,9),(6,9,0),(8,1,10),(9,11,8)],
 # Room B: centered window, then the step-up doorway.
 [(1,1,2),(2,1,2),(5,5,9),(8,10,1),(9,8,11),
  (10,2,12),(11,13,7),(14,12,13)],
 # Room C: raised-wall bay, elevated floor, step down at exit.
 [(3,2,3),(5,5,9),(10,12,2),(11,7,13),(14,13,12),
  (12,3,14),(13,15,6),(15,14,15)],
 # Room D: lintel/overhang bay.
 [(4,3,4),(5,5,9),(7,4,5),(12,14,3),(13,6,15),(15,15,14)],
]

def key_word(sid,v0,v1):
    return sid | (v0<<5) | (v1<<9)

def normal(v0,v1):
    ax,ay=V[v0]; bx,by=V[v1]
    dx,dy=bx-ax,by-ay
    if dx==0:
        return (32 if dy>0 else -32,0)
    if dy==0:
        return (0,-32 if dx>0 else 32)
    L=math.hypot(dx,dy)
    return (round(32*dy/L),round(-32*dx/L))

def extract_array(text,name):
    # Preserve the exact declaration/body from the known-good baseline LUTs.
    m=re.search(rf"static const\s+([^;=]+?)\s+{re.escape(name)}\s*\[[^\]]+\]\s*=\s*\{{.*?\}};",text,re.S)
    if not m:
        raise SystemExit(f"could not find baseline array {name}")
    # Whole match includes 'static const ...'.
    start=m.start(); end=m.end()
    return text[start:end]

def emit_arr(ctype,name,vals,per=16):
    out=[f"static const {ctype} {name}[{len(vals)}] = {{"]
    for i in range(0,len(vals),per):
        out.append("    "+", ".join(str(x) for x in vals[i:i+per])+",")
    out.append("};")
    return "\n".join(out)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--repo-root",default=".")
    ap.add_argument("--out",required=True)
    args=ap.parse_args()
    root=Path(args.repo_root)
    baseline="\n".join((root/f"src/generated/tilesector_polar_data_part0{i}.inc").read_text() for i in range(5))

    # Deduplicate directed keys and map each region to key IDs.
    keys=[]; key_id={}
    region_ids=[]
    for reg in REGIONS:
        ids=[]
        for trip in reg:
            if trip not in key_id:
                key_id[trip]=len(keys); keys.append(trip)
            ids.append(key_id[trip])
        region_ids.append(ids)

    key_words=[key_word(*k) for k in keys]
    # Geometry arrays are per physical sid, not per directed key.
    vx=[x for x,_ in V]; vy=[y for _,y in V]
    anchors=[]; nx=[]; ny=[]; prof=[]; shade=[]
    for sid,name,v0,v1,p,bias in S:
        anchors.append(v0)
        a,b=normal(v0,v1); nx.append(a); ny.append(b)
        prof.append(p); shade.append(bias)

    # Four base lists and four recipes, no conditional selectors.
    base_off=[]; base_stream=[]
    for ids in region_ids:
        base_off.append(len(base_stream))
        base_stream += [len(ids)] + ids
    recipe_off=[]; recipe_stream=[]
    for rid in range(4):
        recipe_off.append(len(recipe_stream))
        recipe_stream += [rid,0]

    # 48x24 coarse recipe field (4 world units/cell). Geometry outside the
    # playable strip is absent. Region boundaries line up with divider planes.
    grid=[255]*(48*24)
    for gy in range(4,20):           # y 16 .. <80
        for gx in range(4,45):       # x 16 .. <180
            x=gx*4+2
            if x<56: rid=0
            elif x<96: rid=1
            elif x<136: rid=2
            else: rid=3
            grid[gy*48+gx]=rid

    # AO is intentionally quiet in the benchmark layout; appearance mode 2 can
    # still exercise the renderer without adding corner clutter to the design.
    ao=[0,0,0,0]

    sections=[
      "/* GENERATED by tools/optimized_renderer_map_bake.py. */",
      "#define TSPF_KEY_COUNT %du"%len(keys),
      "#define TSPF_SELECTOR_COUNT 1u",
      "#define TSPF_BASE_COUNT 4u",
      "#define TSPF_RECIPE_COUNT 4u",
      emit_arr("uint16_t","k_tspf_keys",key_words,12),
      emit_arr("int8_t","k_tspf_sel_a",[0]),
      emit_arr("int8_t","k_tspf_sel_b",[0]),
      emit_arr("int16_t","k_tspf_sel_c",[0]),
      emit_arr("uint8_t","k_tspf_sel_inv",[0]),
      emit_arr("uint8_t","k_tspf_ao",ao),
      emit_arr("uint16_t","k_tspf_base_off",base_off),
      emit_arr("uint8_t","k_tspf_base_stream",base_stream,20),
      emit_arr("uint16_t","k_tspf_recipe_off",recipe_off),
      emit_arr("uint8_t","k_tspf_recipe_stream",recipe_stream),
      emit_arr("uint8_t","k_tspf_recipe_grid",grid,24),
      emit_arr("uint8_t","k_tspf_vx",vx),
      emit_arr("uint8_t","k_tspf_vy",vy),
      emit_arr("uint8_t","k_tspf_seg_anchor",anchors),
      emit_arr("int8_t","k_tspf_nx_q5",nx),
      emit_arr("int8_t","k_tspf_ny_q5",ny),
      emit_arr("uint8_t","k_tspf_profile",prof),
      emit_arr("int8_t","k_tspf_shade_bias",shade),
    ]
    for name in ("k_tspf_recip8_q16","k_tspf_atan_q12","k_tspf_angle_x_pos",
                 "k_tspf_sec_q7","k_tspf_sin_q7","k_tspf_invz"):
        sections.append(extract_array(baseline,name))

    out=Path(args.out); out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text("\n\n".join(sections)+"\n")

    print(f"OPT_MAP_BAKE_PASS vertices={len(V)} surfaces={len(S)} keys={len(keys)}")
    print("region_active_keys="+",".join(map(str,map(len,region_ids)))+" max="+str(max(map(len,region_ids))))
    print("profiles FULL=%d LINTEL=%d RAISED=%d RISER=%d"%(
        prof.count(FULL),prof.count(LINTEL),prof.count(RAISED),prof.count(RISER)))
    print("window=roomB top span sid1+sid2 centered aperture")
    print("steps=roomC floor +4 world units; step-up sid14; step-down sid15")
    for i,(sid,name,v0,v1,p,bias) in enumerate(S):
        print(f"surface {sid:02d} {name:22s} v{v0}->v{v1} profile={p} shade={bias}")

if __name__=="__main__":
    main()
