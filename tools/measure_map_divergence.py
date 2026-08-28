#!/usr/bin/env python3
import argparse, csv, statistics
from pathlib import Path

FRAME_BYTES=720
CELLS=360
TILE_MASK=0x01ff
ATTR_MASK=0xfe00

def words(buf,frame):
    base=frame*FRAME_BYTES
    return [buf[base+i] | (buf[base+i+1]<<8) for i in range(0,FRAME_BYTES,2)]

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--csv")
    a=ap.parse_args()
    rb=Path(a.reference).read_bytes()
    cb=Path(a.candidate).read_bytes()
    if len(rb)!=len(cb) or len(rb)%FRAME_BYTES:
        raise SystemExit(f"bad/sized dumps: {len(rb)} vs {len(cb)}")
    frames=len(rb)//FRAME_BYTES
    rows=[]
    for f in range(frames):
        rw=words(rb,f); cw=words(cb,f)
        word=sum(x!=y for x,y in zip(rw,cw))
        tile=sum((x&TILE_MASK)!=(y&TILE_MASK) for x,y in zip(rw,cw))
        attr=sum((x&ATTR_MASK)!=(y&ATTR_MASK) for x,y in zip(rw,cw))
        attr_only=sum((x&TILE_MASK)==(y&TILE_MASK) and (x&ATTR_MASK)!=(y&ATTR_MASK) for x,y in zip(rw,cw))
        rows.append((f,word,tile,attr,attr_only))
    vals=[r[1] for r in rows]
    tilevals=[r[2] for r in rows]
    print(f"frames={frames}")
    print(f"name-word divergence: mean={statistics.mean(vals):.2f}/{CELLS} ({100*statistics.mean(vals)/CELLS:.2f}%) "
          f"median={statistics.median(vals):.1f} max={max(vals)}")
    print(f"tile-id divergence:   mean={statistics.mean(tilevals):.2f}/{CELLS} ({100*statistics.mean(tilevals)/CELLS:.2f}%) "
          f"median={statistics.median(tilevals):.1f} max={max(tilevals)}")
    print(f"frames exact={sum(v==0 for v in vals)}/{frames}")
    if a.csv:
        with Path(a.csv).open("w",newline="") as f:
            w=csv.writer(f); w.writerow(["frame","word_diff","tile_id_diff","attr_diff","attr_only_diff"]); w.writerows(rows)

if __name__=="__main__":
    main()
