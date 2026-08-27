#!/usr/bin/env python3
"""Stage-21 exact FULL-wall symmetry invariants.

This validates the hardware-facing identity used by the GG fast path:
  bottom_y = 143-top_y
  bottom_row = 17-top_row
  bottom_local = 7-top_local
  bottom_slope = -top_slope
and therefore the bottom Mode-4 name-table word is the exact top edge pattern
with VFLIP + palette attributes, not a second pattern lookup.
"""

TS_EDGE_OFF_MIN=-7
TS_EDGE_OFF_COUNT=16
TS_EDGE_SLOPE_COUNT=8
TS_TILE_EDGE_BASE=39
TS_ATTR_FLIPX=0x0200
TS_ATTR_FLIPY=0x0400
TS_ATTR_PALETTE=0x0800

def clamp(v,lo,hi):
    return max(lo,min(hi,v))

def edge_entry(shade,local_left,slope,bottom):
    attr=0
    if bottom:
        local_left=7-local_left
        slope=-slope
        attr=TS_ATTR_FLIPY|TS_ATTR_PALETTE
    if slope<0:
        mag=-slope
        local_left-=mag
        attr|=TS_ATTR_FLIPX
    else:
        mag=slope
    mag=min(mag,7)
    off=clamp(local_left,-7,8)
    tile=TS_TILE_EDGE_BASE + ((((shade)*TS_EDGE_OFF_COUNT)+(off-TS_EDGE_OFF_MIN))*TS_EDGE_SLOPE_COUNT)+mag
    return tile|attr

def main():
    # Reciprocal->FULL geometry used by Stage 21.
    for inv in range(256):
        top=71-(inv>>1)
        bottom=143-top
        assert top+bottom==143

    checks=0
    for shade in range(3):
        for top_left in range(-64,208):
            for slope in range(-7,8):
                top_right=top_left+slope
                bot_left=143-top_left
                bot_right=143-top_right
                assert bot_right-bot_left==-slope
                for row in range(18):
                    brow=17-row
                    top_local=top_left-(row<<3)
                    bot_local=bot_left-(brow<<3)
                    assert bot_local==7-top_local
                    top_word=edge_entry(shade,top_local,slope,False)
                    bot_word=edge_entry(shade,bot_local,-slope,True)
                    assert bot_word==(top_word|TS_ATTR_FLIPY|TS_ATTR_PALETTE)
                    checks+=1

    print(f"Stage21 FULL symmetry: PASS ({checks} edge-word mirror checks)")

if __name__=="__main__":
    main()
