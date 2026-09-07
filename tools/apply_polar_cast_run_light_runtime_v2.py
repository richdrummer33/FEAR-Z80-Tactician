#!/usr/bin/env python3
"""Second-stage cast-light runtime patch.

Runs the first compact Polar cast-plane integration, then replaces its generic
per-tile half-plane classifier with a per-column interval materializer.

For a fixed screen column each receiver is the intersection of three floor
half-planes.  That intersection is a single vertical interval.  Evaluate the
three boundaries once, derive max(top)/min(bottom), then emit at most two
arbitrary-angle edge tiles plus the full lit rows between them.  Only rare
near-plane/corner-degenerate columns fall back to the reference tile loop.
"""
from pathlib import Path
import re
import runpy

runpy.run_path("tools/apply_polar_cast_run_light_runtime.py", run_name="__main__")

p = Path("src/tilesector_polar_cast_light_runtime.c")
s = p.read_text()

new_block = r'''static uint8_t cast_cell_unclaimed(uint8_t row,uint8_t col){
    uint8_t ci=(uint8_t)(col+col+col+(row>>3));
    return (uint8_t)((g_polar_nt_cov_cur[ci]&k_cast_nt_mask8[row&7u])==0u);
}

static void cast_put_cell(uint16_t *out,uint8_t row,uint8_t col,uint16_t word){
    uint16_t idx=k_cast_row_base[row]+col;uint8_t ci=(uint8_t)(col+col+col+(row>>3));
    g_polar_nt_cov_cur[ci]|=k_cast_nt_mask8[row&7u];
    if(out[idx]!=word){
        out[idx]=word;
        if(g_polar_nt_row_min[row]==0xffu||col<g_polar_nt_row_min[row])g_polar_nt_row_min[row]=col;
        if(col>g_polar_nt_row_max[row])g_polar_nt_row_max[row]=col;
    }
}

/* Shade-0 edge patterns are authored as base-floor above / lit-floor below in
 * palette 1. Vertical flip reuses the same pattern for lit-above. */
static uint16_t cast_edge_word(int16_t local_left,int8_t slope,uint8_t lit_below){
    uint16_t attr=TSP_ATTR_PALETTE;uint8_t mag;int8_t off;
    if(!lit_below){local_left=(int16_t)(7-local_left);slope=(int8_t)-slope;attr|=TSP_ATTR_FLIPY;}
    if(slope<0){mag=(uint8_t)(-slope);local_left=(int16_t)(local_left-mag);attr|=TSP_ATTR_FLIPX;}else mag=(uint8_t)slope;
    if(mag>=TSP_EDGE_SLOPE_COUNT)mag=TSP_EDGE_SLOPE_COUNT-1u;
    off=cast_clamp_s8(local_left,TSP_EDGE_OFF_MIN,(int8_t)(TSP_EDGE_OFF_MIN+TSP_EDGE_OFF_COUNT-1));
    return (uint16_t)(TSP_TILE_EDGE(0u,(uint8_t)(off-TSP_EDGE_OFF_MIN),mag)|attr);
}

/* Slow reference is kept only for a column where a plane crosses q=0 or where
 * top and bottom bounds collide inside one 8-pixel column.  Normal columns do
 * not execute this path. */
static uint8_t cast_ref_plane_pass(const CastScreenPlane *p,int16_t ql,int16_t qr,uint8_t row){
    int16_t qm=(int16_t)((ql+qr)>>1),yb;int16_t yc=(int16_t)(((uint16_t)row<<3)+4u);
    if(qm<=0)return p->cam_lit;
    yb=cast_floor_y_from_q(qm,p->floor_z);
    return p->cam_lit?(uint8_t)(yc>=yb):(uint8_t)(yc<yb);
}

static uint8_t cast_ref_plane_cross(const CastScreenPlane *p,int16_t ql,int16_t qr,uint8_t row,int16_t *yl,int16_t *yr){
    int16_t lo=(int16_t)((uint16_t)row<<3),hi=(int16_t)(lo+7);
    if(ql<=0||qr<=0)return 0u;
    *yl=cast_floor_y_from_q(ql,p->floor_z);*yr=cast_floor_y_from_q(qr,p->floor_z);
    return (uint8_t)(((*yl>=lo&&*yl<=hi)||(*yr>=lo&&*yr<=hi)||(*yl<lo&&*yr>hi)||(*yr<lo&&*yl>hi))?1u:0u);
}

static void cast_draw_slow_column(uint16_t *out,uint8_t col,const uint8_t ids[3],const int16_t ql[3],const int16_t qr[3]){
    uint8_t row,k,pass[3],cross[3];int16_t yl[3],yr[3];
    for(row=CAST_FLOOR_FIRST_ROW;row<TSP_ROWS;++row){
        uint8_t all=1u,cross_count=0u,cross_id=0xffu,others=1u;
        if(!cast_cell_unclaimed(row,col))continue;
        for(k=0u;k<3u;++k){
            const CastScreenPlane *p=&g_cast_plane[ids[k]];
            pass[k]=cast_ref_plane_pass(p,ql[k],qr[k],row);if(!pass[k])all=0u;
            cross[k]=cast_ref_plane_cross(p,ql[k],qr[k],row,&yl[k],&yr[k]);
            if(cross[k]){++cross_count;cross_id=k;}
        }
        if(all&&cross_count==0u){cast_put_cell(out,row,col,CAST_LIT_FLOOR_WORD);continue;}
        if(cross_count==1u){
            for(k=0u;k<3u;++k)if(k!=cross_id&&!pass[k])others=0u;
            if(others){
                const CastScreenPlane *p=&g_cast_plane[ids[cross_id]];
                int16_t rowtop=(int16_t)((uint16_t)row<<3);
                int8_t slope=cast_clamp_s8((int16_t)(yr[cross_id]-yl[cross_id]),-7,7);
                cast_put_cell(out,row,col,cast_edge_word((int16_t)(yl[cross_id]-rowtop),slope,p->cam_lit));
                continue;
            }
        }
        if(all)cast_put_cell(out,row,col,CAST_LIT_FLOOR_WORD);
    }
}

static void cast_draw_boundary(uint16_t *out,uint8_t col,int16_t yl,int16_t yr,uint8_t lit_below){
    int16_t lo=yl<yr?yl:yr,hi=yl>yr?yl:yr;int8_t r0,r1,r;int8_t slope;
    if(hi<((int16_t)CAST_FLOOR_FIRST_ROW<<3)||lo>=144)return;
    if(lo<0)lo=0;if(hi>143)hi=143;
    r0=(int8_t)(lo>>3);r1=(int8_t)(hi>>3);
    if(r0<(int8_t)CAST_FLOOR_FIRST_ROW)r0=(int8_t)CAST_FLOOR_FIRST_ROW;
    if(r1>=(int8_t)TSP_ROWS)r1=(int8_t)(TSP_ROWS-1u);
    slope=cast_clamp_s8((int16_t)(yr-yl),-7,7);
    for(r=r0;r<=r1;++r){
        uint8_t row=(uint8_t)r;
        if(cast_cell_unclaimed(row,col))
            cast_put_cell(out,row,col,cast_edge_word((int16_t)(yl-((int16_t)r<<3)),slope,lit_below));
    }
}

static void cast_fill_rows(uint16_t *out,uint8_t col,int8_t first,int8_t last){
    int8_t r;
    if(first<(int8_t)CAST_FLOOR_FIRST_ROW)first=(int8_t)CAST_FLOOR_FIRST_ROW;
    if(last>=(int8_t)TSP_ROWS)last=(int8_t)(TSP_ROWS-1u);
    for(r=first;r<=last;++r){uint8_t row=(uint8_t)r;if(cast_cell_unclaimed(row,col))cast_put_cell(out,row,col,CAST_LIT_FLOOR_WORD);}
}

/* Fast normal path: each plane contributes either a lower bound, an upper
 * bound, no bound, or an empty-column rejection.  Intersect the three once per
 * screen column instead of testing 8 floor rows * 3 planes. */
static void cast_draw_receiver(uint16_t *out,uint8_t p0,uint8_t p1,uint8_t gate){
    uint8_t c,k;uint8_t ids[3];int16_t ql[3],qr[3];
    ids[0]=p0;ids[1]=p1;ids[2]=gate;
    for(k=0u;k<3u;++k)ql[k]=g_cast_plane[ids[k]].q0;
    for(c=0u;c<TSP_COLS;++c){
        int16_t top_l=80,top_r=80,bot_l=144,bot_r=144;
        uint8_t has_top=0u,has_bot=0u,empty=0u,slow=0u;
        for(k=0u;k<3u;++k){
            const CastScreenPlane *p=&g_cast_plane[ids[k]];int16_t yl,yr;
            qr[k]=(int16_t)(ql[k]+p->step);
            if(ql[k]<=0||qr[k]<=0){
                if(ql[k]<=0&&qr[k]<=0){if(!p->cam_lit)empty=1u;}
                else slow=1u;
                continue;
            }
            yl=cast_floor_y_from_q(ql[k],p->floor_z);yr=cast_floor_y_from_q(qr[k],p->floor_z);
            if(p->cam_lit){if(yl>top_l)top_l=yl;if(yr>top_r)top_r=yr;has_top=1u;}
            else {if(yl<bot_l)bot_l=yl;if(yr<bot_r)bot_r=yr;has_bot=1u;}
        }
        if(!empty){
            if((top_l>=bot_l)!=(top_r>=bot_r))slow=1u;
            else if(top_l>=bot_l&&top_r>=bot_r)empty=1u;
            if(!empty&&has_top&&has_bot){
                int16_t ta=top_l<top_r?top_l:top_r,tb=top_l>top_r?top_l:top_r;
                int16_t ba=bot_l<bot_r?bot_l:bot_r,bb=bot_l>bot_r?bot_l:bot_r;
                if((ta>>3)<=(bb>>3)&&(ba>>3)<=(tb>>3)){
                    /* Only force slow if the actual boundary tile-row ranges overlap. */
                    int16_t t0=ta>>3,t1=tb>>3,b0=ba>>3,b1=bb>>3;
                    if(!(t1<b0||b1<t0))slow=1u;
                }
            }
            if(!empty){
                if(slow)cast_draw_slow_column(out,c,ids,ql,qr);
                else {
                    int8_t first=(int8_t)CAST_FLOOR_FIRST_ROW,last=(int8_t)(TSP_ROWS-1u);
                    if(has_top){int16_t t=top_l>top_r?top_l:top_r;cast_draw_boundary(out,c,top_l,top_r,1u);first=(int8_t)((t>>3)+1);}
                    if(has_bot){int16_t b=bot_l<bot_r?bot_l:bot_r;cast_draw_boundary(out,c,bot_l,bot_r,0u);last=(int8_t)((b>>3)-1);}
                    cast_fill_rows(out,c,first,last);
                }
            }
        }
        for(k=0u;k<3u;++k)ql[k]=qr[k];
    }
}

void tsp_polar_cast_light'''

pat = re.compile(r"static uint8_t cast_plane_pass\(.*?\nvoid tsp_polar_cast_light", re.S)
m = pat.search(s)
if not m:
    raise SystemExit("v2 cast materializer anchor not found")
s = s[:m.start()] + new_block + s[m.end():]
p.write_text(s)

print("POLAR_CAST_RUN_V2=PER_COLUMN_INTERVAL")
print("NORMAL_TILE_PLANE_TESTS_PER_COLUMN=0")
print("NORMAL_BOUNDS_PER_RECEIVER_COLUMN=3")
print("NORMAL_EDGE_TILES_PER_COLUMN_MAX=2")
print("DEGENERATE_COLUMN_FALLBACK=REFERENCE_TILE_LOOP")
