#!/usr/bin/env python3
import sys
from pathlib import Path

FRAME_BYTES=720
COLS=20
ROWS=18

def word(buf, off):
    return buf[off] | (buf[off+1]<<8)

def main():
    if len(sys.argv)!=4:
        raise SystemExit("usage: compare_map_dumps.py LABEL control.bin repair.bin")
    label,ca,ra=sys.argv[1:]
    c=Path(ca).read_bytes()
    r=Path(ra).read_bytes()
    if len(c)!=len(r):
        raise SystemExit(f"{label}: dump size mismatch {len(c)} != {len(r)}")
    if len(c)%FRAME_BYTES:
        raise SystemExit(f"{label}: bad dump size {len(c)}")
    frames=len(c)//FRAME_BYTES
    diff_frames=0
    first=None
    total_cells=0
    for f in range(frames):
        base=f*FRAME_BYTES
        frame_diff=False
        for cell in range(COLS*ROWS):
            off=base+cell*2
            cw=word(c,off)
            rw=word(r,off)
            if cw!=rw:
                total_cells+=1
                frame_diff=True
                if first is None:
                    row=cell//COLS
                    col=cell%COLS
                    first=(f,row,col,cw,rw)
        if frame_diff:
            diff_frames+=1
    if first is None:
        print(f"{label}: map dump equivalence PASS frames={frames}")
        return
    f,row,col,cw,rw=first
    # Show all differing cells in the first bad frame, capped for readable CI.
    diffs=[]
    base=f*FRAME_BYTES
    for cell in range(COLS*ROWS):
        off=base+cell*2
        a=word(c,off); b=word(r,off)
        if a!=b:
            diffs.append((cell//COLS,cell%COLS,a,b))
    print(
        f"{label}: FIRST MISMATCH frame={f} row={row} col={col} "
        f"control=0x{cw:04X} repair=0x{rw:04X}; "
        f"bad_frames={diff_frames}/{frames} total_bad_cells={total_cells}"
    )
    for rr,cc,a,b in diffs[:24]:
        print(f"  frame={f} r={rr:02d} c={cc:02d} control=0x{a:04X} repair=0x{b:04X}")
    if len(diffs)>24:
        print(f"  ... {len(diffs)-24} more differing cells in first bad frame")
    raise SystemExit(1)

if __name__=="__main__":
    main()
