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

extern const uint8_t g_tsp_seam_mask_home[20];
extern const uint8_t g_tsp_seam_reflect_home[20];

#define TSP_SEAM_TILE_BASE 412u
#define TSP_SEAM_TILE_COUNT 20u

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
    if(m) return 0xffu;

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
    uint8_t i;

    for(i=0u;i<g_tspf_seam_desc_count;++i){
        uint8_t vid=g_tspf_seam_vid[i];
        uint8_t half,x,col,bit,first,last,row;
        int8_t top_tile;

        if(vid>=32u) continue;
        half=g_tspf_seam_vertex_half[vid];
        if(half==0xffu) continue;

        x=g_tspf_seam_x[i];
        if(x>=160u) continue;
        col=(uint8_t)(x>>3);
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

        g_tspf_seam_cur_cols[col>>3] |= (uint8_t)(1u<<(col&7u));

        for(row=first;;++row){
            uint16_t idx=(uint16_t)row*TSP_COLS+col;
            uint16_t old=g_map[idx];
            uint16_t id=(uint16_t)(old&TSP_TILE_ID_MASK);
            uint8_t base_mask;
            uint16_t attr=0u;
            uint8_t code;

            if(id>=3u && id<7u){
                uint8_t border=(uint8_t)(id-3u);
                base_mask=(uint8_t)(((border&1u)?0x01u:0u) |
                                    ((border&2u)?0x80u:0u));
            } else if(id>=TSP_SEAM_TILE_BASE &&
                      id<(TSP_SEAM_TILE_BASE+TSP_SEAM_TILE_COUNT)){
                uint8_t si=(uint8_t)(id-TSP_SEAM_TILE_BASE);
                base_mask=(old&TSP_ATTR_FLIPX) ?
                    g_tsp_seam_reflect_home[si] : g_tsp_seam_mask_home[si];
            } else {
                if(row==last) break;
                continue;
            }

            code=seam_mask_to_code((uint8_t)(base_mask|bit),&attr);
            if(code!=0xffu){
                uint16_t nw=(uint16_t)(TSP_SEAM_TILE_BASE+code+attr);
                if(nw!=old){
                    g_map[idx]=nw;
                    if(g_polar_nt_row_min[row]==0xffu || col<g_polar_nt_row_min[row])
                        g_polar_nt_row_min[row]=col;
                    if(col>g_polar_nt_row_max[row])
                        g_polar_nt_row_max[row]=col;
                }
            }

            if(row==last) break;
        }
    }
}

#endif
