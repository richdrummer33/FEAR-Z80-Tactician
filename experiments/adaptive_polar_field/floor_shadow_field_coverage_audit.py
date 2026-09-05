#!/usr/bin/env python3
"""Audit the REAL incremental bearing-field cost for baked floor shadows.

The first floor-shadow POC correctly noticed that 9/13 shadow vertices are
existing Polar world vertices, but that does not imply the production local
projection pack contains those 9 records in every camera cell where lighting
may need them. The normal pack stores only geometry-relevant corners per cell.

This audit measures three concrete packs over every non-empty Polar camera cell:

  1) new4_only: the four genuinely new shadow vertices (the original POC number),
  2) overlay_reuse: reuse an old vertex only when its production record is
     actually present in that cell; bake missing old vertices + the four new,
  3) standalone13: a fully independent light-bearing field for all 13 shadow
     vertices, useful as a simplicity upper bound.

Format accounting matches the current runtime-shaped bearing field:
  full 48x24 uint16 cell offsets + uint16 per-active-cell record mask +
  one depth/mode byte per stored vertex + four bytes per affine leaf.

No source-side 'reuse' is credited unless lp.relevant() proves that exact old
corner is present in the production cell block.
"""
from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path

import floor_shadow_edge_field_poc as light
import local_projection_field_poc as lp


def unique_shadow_refs():
    out=[]; seen=set()
    for poly,_ in light.POLYS:
        for v in poly:
            key=('old',v.existing) if v.existing>=0 else ('new',v.name)
            if key not in seen:
                seen.add(key);out.append(v)
    return tuple(out)


def old_depth(d,vid,gx,gy,thr,minq):
    dep,err=lp.corner_quant_depth(d,vid,gx,gy,thr,minq)
    return dep,err


def new_depth(v,gx,gy,thr,minq):
    desc=light.build_new_desc(v,gx,gy,thr,minq)
    return desc.depth,desc.worst


def add_record(stats,dep,err):
    stats['records']+=1
    if dep is None:
        stats['fallback_records']+=1
        return
    stats['depth_hist'][dep]+=1
    stats['affine_leaves']+=4**dep
    stats['worst_q12']=max(stats['worst_q12'],float(err))


def finish(stats,active_cells):
    # Every active cell gets a 16-bit light-record mask. A future generator may
    # compress empty overlay cells, but this deliberately stays conservative.
    offsets=lp.GRID_W*lp.GRID_H*2
    masks=active_cells*2
    stats['offset_bytes']=offsets
    stats['mask_bytes']=masks
    stats['mode_bytes']=stats['records']
    stats['leaf_bytes']=stats['affine_leaves']*4
    stats['estimated_bytes']=offsets+masks+stats['mode_bytes']+stats['leaf_bytes']
    stats['depth_hist']=dict(sorted(stats['depth_hist'].items()))
    return stats


def empty_stats():
    return dict(records=0,fallback_records=0,affine_leaves=0,
                depth_hist=Counter(),worst_q12=0.0)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--threshold',type=float,default=4.0)
    ap.add_argument('--min-q4',type=int,default=8)
    ap.add_argument('--out',default='')
    a=ap.parse_args()

    d=lp.load();refs=unique_shadow_refs()
    old_refs=tuple(v for v in refs if v.existing>=0)
    new_refs=tuple(v for v in refs if v.existing<0)
    assert len(refs)==13 and len(old_refs)==9 and len(new_refs)==4

    new4=empty_stats();overlay=empty_stats();standalone=empty_stats()
    active_cells=0
    old_need=0;old_present=0;old_missing=0
    per_old_present=Counter();per_old_missing=Counter()
    overlay_cells_with_no_missing_old=0

    for gy in range(lp.GRID_H):
      for gx in range(lp.GRID_W):
        prod,_=lp.relevant(d,gx,gy)
        if not prod:continue
        active_cells+=1;prod=set(prod);missing_this=0

        # Four genuinely new records are always light-only in this conservative
        # all-polygons-available model.
        for v in new_refs:
            dep,err=new_depth(v,gx,gy,a.threshold,a.min_q4)
            add_record(new4,dep,err)
            add_record(overlay,dep,err)
            add_record(standalone,dep,err)

        for v in old_refs:
            old_need+=1
            dep,err=old_depth(d,v.existing,gx,gy,a.threshold,a.min_q4)
            # Standalone light pack never relies on production coverage.
            add_record(standalone,dep,err)
            if v.existing in prod:
                old_present+=1;per_old_present[v.existing]+=1
            else:
                old_missing+=1;missing_this+=1;per_old_missing[v.existing]+=1
                add_record(overlay,dep,err)
        if not missing_this:overlay_cells_with_no_missing_old+=1

    out={
      'threshold_q12':a.threshold,
      'min_leaf_q4':a.min_q4,
      'active_camera_cells':active_cells,
      'shadow_vertices_total':len(refs),
      'old_shadow_vertices':len(old_refs),
      'new_shadow_vertices':len(new_refs),
      'old_record_needs':old_need,
      'old_records_present_in_production':old_present,
      'old_records_missing_from_production':old_missing,
      'old_reuse_fraction':old_present/max(1,old_need),
      'cells_where_all_9_old_records_already_present':overlay_cells_with_no_missing_old,
      'per_old_present':dict(sorted(per_old_present.items())),
      'per_old_missing':dict(sorted(per_old_missing.items())),
      'new4_only':finish(new4,active_cells),
      'overlay_reuse':finish(overlay,active_cells),
      'standalone13':finish(standalone,active_cells),
    }

    print('=== FLOOR SHADOW BEARING COVERAGE AUDIT ===')
    print(f"active camera cells={active_cells}; old record needs={old_need}")
    print(f"production old-record reuse={old_present}/{old_need} ({100*out['old_reuse_fraction']:.2f}%) missing={old_missing}")
    print(f"cells with all 9 old shadow records already present={overlay_cells_with_no_missing_old}/{active_cells}")
    for name in ('new4_only','overlay_reuse','standalone13'):
        s=out[name]
        print(f"{name}: records={s['records']} leaves={s['affine_leaves']} fallback={s['fallback_records']} bytes~={s['estimated_bytes']} hist={s['depth_hist']}")
    print('NOTE: overlay_reuse is the conservative corrected incremental ROM estimate.')

    if a.out:
        p=Path(a.out);p.parent.mkdir(parents=True,exist_ok=True)
        p.write_text(json.dumps(out,indent=2,sort_keys=True)+'\n')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
