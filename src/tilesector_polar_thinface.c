/*
 * Thin/sub-column physical seam overlay.
 *
 * Kept out of _HOME deliberately: the fixed 16 KiB window is already occupied
 * by the hot materializer, exact-envelope helpers and edge dictionaries.  This
 * pass runs once after coarse materialization, so a single banked call is far
 * cheaper than overflowing HOME (which silently corrupts the next ROM bank).
 */
#if defined(__SDCC)
#pragma bank 254
#include <gbdk/platform.h>
#endif

#include <stdint.h>
#include "tilesector_polar.h"

#ifndef TSPF_THIN_FACE_SURVIVAL
#define TSPF_THIN_FACE_SURVIVAL 0
#endif

#if defined(__SDCC) && TSPF_THIN_FACE_SURVIVAL

extern uint16_t g_map[TSP_MAP_CELLS];
extern uint8_t g_polar_nt_row_min[TSP_ROWS];
extern uint8_t g_polar_nt_row_max[TSP_ROWS];

extern uint8_t g_tspf_seam_desc_count;
extern uint8_t g_tspf_seam_x[32];
extern uint8_t g_tspf_seam_vid[32];
extern uint8_t g_tspf_seam_vertex_half[32];
extern uint8_t g_tspf_seam_cur_cols[3];
extern uint8_t g_e1env_program[];

/* Horizontal seam retention is intentionally compact: descriptor identity/X
 * is enough to know whether a coarse tile column must be re-materialized.
 * Vertical motion is already handled by the retained top/bottom symmetric-
 * difference path and by coverage reconciliation. */
uint8_t g_tspf_seam_dirty_cols[3];
uint8_t g_tspf_seam_history_valid;
static uint8_t s_prev_count;
static uint8_t s_prev_x[32];
static uint8_t s_prev_vid[32];
static uint8_t s_row_mask[TSP_ROWS];
extern const uint8_t g_e1env_center_col_lut[1025];

extern const uint8_t g_tsp_seam_mask_home[29];
extern const uint8_t g_tsp_seam_reflect_home[29];

#define TSP_SEAM_TILE_BASE 412u
#define TSP_SEAM_TILE_COUNT 29u

/* Capture one physical connected corner before coarse 8-pixel ownership can
 * discard the face on either side. This used to live in HOME; banking it here
 * restores the fixed-ROM safety margin. It is deliberately a correctness rung:
 * once the visual result is proven we can batch these records to remove the
 * per-boundary bank-switch cost. */
void tsp_polar_record_subcolumn_boundary(uint8_t left_i,uint8_t n,int16_t rel) BANKED
{
    uint8_t count,owner,ni,code;
    int16_t x;

    if(rel<=-512 || rel>=512) return;
    owner=g_e1env_program[(uint8_t)(2u+(uint8_t)(left_i<<1))];
    if(!(owner&0x80u)) return;

    count=g_tspf_seam_desc_count;
    if(count>=32u) return;

    code=g_e1env_center_col_lut[(uint16_t)(rel+512)];
    x=(int16_t)((uint16_t)(code&31u)<<3) +
      (int16_t)((int8_t)(code>>5)-4);
    if(x<0 || x>=160) return;

    ni=(uint8_t)(left_i+1u<n ? left_i+1u : 0u);
    g_tspf_seam_x[count]=(uint8_t)x;
    g_tspf_seam_vid[count]=g_e1env_program[(uint8_t)(1u+(uint8_t)(ni<<1))];
    g_tspf_seam_desc_count=(uint8_t)(count+1u);

    /* Make the CURRENT physical seam columns visible before coarse
     * materialization. The retained gate can then distinguish "same tile,
     * seam merely moved/changed inside it" from a tile the seam vacated. */
    {
        uint8_t col=(uint8_t)((uint8_t)x>>3);
        g_tspf_seam_cur_cols[col>>3] |= (uint8_t)(1u<<(col&7u));
    }
}

/* Mark the old/new coarse columns touched by a horizontal physical-boundary
 * change. This is the tile-local form of the swept-delta-X idea: unchanged
 * seam columns stay eligible for retained/Y-only patching; a vacated or newly
 * entered column is forced through the normal front-to-back materializer once.
 * That ordinary pass resolves the current nearest owner, so no background
 * snapshot or reverse painter is required. */
void tsp_polar_seam_prepare_dirty(void) BANKED
{
    uint8_t i,j,found;

    g_tspf_seam_dirty_cols[0]=0u;
    g_tspf_seam_dirty_cols[1]=0u;
    g_tspf_seam_dirty_cols[2]=0u;

    if(!g_tspf_seam_history_valid){
        s_prev_count=0u;
        g_tspf_seam_history_valid=1u;
    }

    for(i=0u;i<s_prev_count;++i){
        found=0u;
        for(j=0u;j<g_tspf_seam_desc_count;++j){
            if(s_prev_vid[i]==g_tspf_seam_vid[j]){
                found=1u;
                if(s_prev_x[i]!=g_tspf_seam_x[j]){
                    uint8_t oc=(uint8_t)(s_prev_x[i]>>3);
                    uint8_t nc=(uint8_t)(g_tspf_seam_x[j]>>3);
                    g_tspf_seam_dirty_cols[oc>>3] |= (uint8_t)(1u<<(oc&7u));
                    g_tspf_seam_dirty_cols[nc>>3] |= (uint8_t)(1u<<(nc&7u));
                }
                break;
            }
        }
        if(!found){
            uint8_t oc=(uint8_t)(s_prev_x[i]>>3);
            g_tspf_seam_dirty_cols[oc>>3] |= (uint8_t)(1u<<(oc&7u));
        }
    }

    for(i=0u;i<g_tspf_seam_desc_count;++i){
        found=0u;
        for(j=0u;j<s_prev_count;++j)
            if(g_tspf_seam_vid[i]==s_prev_vid[j]){ found=1u; break; }
        if(!found){
            uint8_t nc=(uint8_t)(g_tspf_seam_x[i]>>3);
            g_tspf_seam_dirty_cols[nc>>3] |= (uint8_t)(1u<<(nc&7u));
        }
    }

    s_prev_count=g_tspf_seam_desc_count;
    for(i=0u;i<s_prev_count;++i){
        s_prev_x[i]=g_tspf_seam_x[i];
        s_prev_vid[i]=g_tspf_seam_vid[i];
    }
}

/* Same canonical 20-pattern encoding as the former HOME assembly decoder.
 * Return 0xff when a cell somehow contains more than two physical seams. */
static uint8_t seam_mask_to_code(uint8_t mask,uint16_t *attr)
{
    uint8_t x=0u,y,m;

    if(!mask) return 0xffu;
    m=mask;
    while(!(m&1u)){ m>>=1; ++x; }
    m>>=1;
    if(!m){
        if(x>=4u){
            x=(uint8_t)(7u-x);
            *attr=TSP_ATTR_FLIPX;
        } else *attr=0u;
        return x;
    }

    y=(uint8_t)(x+1u);
    while(!(m&1u)){ m>>=1; ++y; }
    m>>=1;
    if(m){
        uint8_t i;
        /* Rare crowded-tile path. The 9 extra physical patterns are exactly
         * the canonical 3+ masks observed by the projection census. Compare
         * against both stored and reflected semantics; no general bit-reverse
         * or 256-entry table is paid on the common one/two-line path. */
        for(i=20u;i<TSP_SEAM_TILE_COUNT;++i){
            if(mask==g_tsp_seam_mask_home[i]){ *attr=0u; return i; }
            if(mask==g_tsp_seam_reflect_home[i]){ *attr=TSP_ATTR_FLIPX; return i; }
        }
        return 0xffu;
    }

    /* Reflect pair if that gives the canonical representative. */
    if(x>(uint8_t)(7u-y)){
        uint8_t ox=x;
        x=(uint8_t)(7u-y);
        y=(uint8_t)(7u-ox);
        *attr=TSP_ATTR_FLIPX;
    } else *attr=0u;

    if(x==0u) return (uint8_t)(y+3u);
    if(x==1u) return (uint8_t)(y+9u);
    if(x==2u) return (uint8_t)(y+13u);
    return 19u; /* only canonical x=3,y=4 remains */
}

static int8_t floor_div8(int8_t v)
{
    if(v>=0) return (int8_t)(v/8);
    return (int8_t)(-(((-v)+7)/8));
}

void tsp_polar_subcolumn_seams_fast(void) BANKED
{
    uint8_t col,i,row;

    /* Compose from CURRENT physical seams, never from the coarse tile's old
     * left/right border bits. That is the correctness half of the delta-X
     * design: the snapped 8-pixel answer is not another line to preserve.
     *
     * Work one coarse column at a time so the complete row-local seam set fits
     * in only 18 scratch bytes. This also naturally handles multiple exact
     * seams in the same tile without a 360-byte screen mask. */
    for(col=0u;col<TSP_COLS;++col){
        uint8_t cm=(uint8_t)(1u<<(col&7u));
        if(!(g_tspf_seam_cur_cols[col>>3]&cm)) continue;

        for(row=0u;row<TSP_ROWS;++row) s_row_mask[row]=0u;

        for(i=0u;i<g_tspf_seam_desc_count;++i){
            uint8_t vid,half,x,bit,first,last;
            int8_t top_tile;

            x=g_tspf_seam_x[i];
            if((x>>3)!=col) continue;
            vid=g_tspf_seam_vid[i];
            if(vid>=32u) continue;
            half=g_tspf_seam_vertex_half[vid];
            if(half==0xffu) continue;

            bit=(uint8_t)(1u<<(x&7u));
            top_tile=floor_div8((int8_t)(71-(int16_t)half));
            {
                int16_t f=(int16_t)top_tile+1;
                int16_t l=16-(int16_t)top_tile;
                if(f<0) f=0;
                if(f>=18 || l<0) continue;
                if(l>=18) l=17;
                if(f>l) continue;
                first=(uint8_t)f;
                last=(uint8_t)l;
            }
            for(row=first;;++row){
                s_row_mask[row]|=bit;
                if(row==last) break;
            }
        }

        for(row=0u;row<TSP_ROWS;++row){
            uint8_t mask=s_row_mask[row],code;
            uint16_t idx,old,id,attr=0u,nw;
            if(!mask) continue;

            idx=(uint16_t)row*TSP_COLS+col;
            old=g_map[idx];
            id=(uint16_t)(old&TSP_TILE_ID_MASK);

            /* Physical seams live only through FULL interior material. A
             * previously composed seam tile is also valid input because an
             * unchanged retained column may not have been re-rasterized. */
            if(!((id>=3u && id<7u) ||
                 (id>=TSP_SEAM_TILE_BASE &&
                  id<(TSP_SEAM_TILE_BASE+TSP_SEAM_TILE_COUNT))))
                continue;

            code=seam_mask_to_code(mask,&attr);
            if(code==0xffu){
                /* Defensive fallback for a future uncensused crowded mask:
                 * preserve the two outer physical boundaries. This deliberately
                 * drops an interior exact seam rather than resurrecting a fake
                 * 8-pixel coarse border. */
                uint8_t lo=0u,hi=7u;
                while(lo<8u && !(mask&(uint8_t)(1u<<lo))) ++lo;
                while(hi>lo && !(mask&(uint8_t)(1u<<hi))) --hi;
                mask=(uint8_t)((1u<<lo)|(1u<<hi));
                code=seam_mask_to_code(mask,&attr);
                if(code==0xffu) continue;
            }

            nw=(uint16_t)(TSP_SEAM_TILE_BASE+code+attr);
            if(nw!=old){
                g_map[idx]=nw;
                if(g_polar_nt_row_min[row]==0xffu || col<g_polar_nt_row_min[row])
                    g_polar_nt_row_min[row]=col;
                if(col>g_polar_nt_row_max[row])
                    g_polar_nt_row_max[row]=col;
            }
        }
    }
}

#endif
