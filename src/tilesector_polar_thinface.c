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

/* Mirror the renderer's RAM run record. The array itself is exported only so
 * this banked post-pass can recover true physical-corner height without adding
 * more code to the nearly-full renderer bank. */
typedef struct TSPThinRun {
    uint8_t sid;
    uint8_t v0;
    uint8_t v1;
    uint8_t x0;
    uint8_t x1;
    uint8_t inv0;
    uint8_t inv1;
    uint8_t inv_mid;
    uint8_t left_real;
    uint8_t right_real;
    uint8_t c0;
    uint8_t c1;
    uint8_t depth_plane;
#if defined(TSPF_E1M1_FRONT_ENVELOPE)
    uint8_t right_connected;
#endif
    int16_t iq;
    int16_t step;
} TSPThinRun;
extern TSPThinRun g_runs[];

/* Horizontal seam retention is intentionally compact: descriptor identity/X
 * is enough to know whether a coarse tile column must be re-materialized.
 * Vertical motion is already handled by the retained top/bottom symmetric-
 * difference path and by coverage reconciliation. */
uint8_t g_tspf_seam_dirty_cols[3];
uint8_t g_tspf_seam_history_valid;
static uint8_t s_prev_x[32];
static uint8_t s_prev_seen[4];
static uint8_t s_cur_seen[4];
static uint8_t s_overlay_touched[(TSP_MAP_CELLS+7u)/8u];
extern const uint8_t g_e1env_center_col_lut[1025];

extern const uint8_t g_tsp_seam_mask_home[20];
extern const uint8_t g_tsp_seam_reflect_home[20];

#define TSP_SEAM_TILE_BASE 412u
#define TSP_SEAM_BASE_COUNT 20u
#define TSP_SEAM_EXTRA_COUNT 9u
#define TSP_SEAM_TILE_COUNT 29u

/* The fixed bank was already within a few bytes of 16 KiB. Keep the rare
 * crowded-mask extension here in bank 254 rather than spending HOME bytes.
 * These are exactly the canonical 3+ masks observed by the projection census. */
static const uint8_t k_extra_seam_mask[TSP_SEAM_EXTRA_COUNT] = {
    0x46u,0x51u,0x23u,0x29u,0x45u,0x25u,0x31u,0x49u,0x89u
};
static const uint8_t k_extra_seam_reflect[TSP_SEAM_EXTRA_COUNT] = {
    0x62u,0x8au,0xc4u,0x94u,0xa2u,0xa4u,0x8cu,0x92u,0x91u
};

uint8_t tsp_polar_extra_seam_mask(uint8_t i) BANKED
{
    return i<TSP_SEAM_EXTRA_COUNT ? k_extra_seam_mask[i] : 0u;
}

static void seam_dirty_col(uint8_t col)
{
    g_tspf_seam_dirty_cols[col>>3] |= (uint8_t)(1u<<(col&7u));
}

/* Capture one physical connected corner before coarse 8-pixel ownership can
 * discard the face on either side. This used to live in HOME; banking it here
 * restores the fixed-ROM safety margin. It is deliberately a correctness rung:
 * once the visual result is proven we can batch these records to remove the
 * per-boundary bank-switch cost. */
void tsp_polar_record_subcolumn_boundary(uint8_t left_i,uint8_t n,int16_t rel) BANKED
{
    uint8_t count,owner,next_owner,ni,code;
    int16_t x;

    if(rel<=-512 || rel>=512) return;
    owner=g_e1env_program[(uint8_t)(2u+(uint8_t)(left_i<<1))];
    ni=(uint8_t)(left_i+1u<n ? left_i+1u : 0u);
    next_owner=g_e1env_program[(uint8_t)(2u+(uint8_t)(ni<<1))];

    /* Do not limit the true-X overlay to corners physically shared by both
     * owners (bit7). A visibility handoff can also be one wall ENDING at its
     * authored endpoint and revealing a different wall/void behind, or a new
     * wall BEGINNING at its endpoint in front of the previous owner. Those
     * one-sided silhouettes are exactly where the remaining long snapped
     * 0/7 tile-border ghosts come from.
     *
     * bit6 = left owner's right boundary is its physical endpoint.
     * bit5 = next owner's left boundary is its physical endpoint.
     * 0xff is NO_WALL, whose high bits must never be interpreted as flags. */
    if(!((owner!=0xffu && (owner&0x40u)) ||
         (next_owner!=0xffu && (next_owner&0x20u))))
        return;

    count=g_tspf_seam_desc_count;
    if(count>=32u) return;

    code=g_e1env_center_col_lut[(uint16_t)(rel+512)];
    x=(int16_t)((uint16_t)(code&31u)<<3) +
      (int16_t)((int8_t)(code>>5)-4);
    if(x<0 || x>=160) return;

    {
        uint8_t vid=g_e1env_program[(uint8_t)(1u+(uint8_t)(ni<<1))];
        uint8_t ux=(uint8_t)x;
        uint8_t col=(uint8_t)(ux>>3);
        uint8_t bi=(uint8_t)(vid>>3);
        uint8_t bm=(uint8_t)(1u<<(vid&7u));

        g_tspf_seam_x[count]=ux;
        g_tspf_seam_vid[count]=vid;
        g_tspf_seam_desc_count=(uint8_t)(count+1u);

        /* Current exact seam coverage is known before any coarse column draws. */
        g_tspf_seam_cur_cols[col>>3] |= (uint8_t)(1u<<(col&7u));

        /* Direct physical-vertex history turns the temporal comparison into
         * O(1) per descriptor. On movement mark both old and new coarse tiles;
         * those tiles alone are forced through the ordinary front-to-back
         * owner/materializer path. */
        if(vid<32u){
            if(g_tspf_seam_history_valid && (s_prev_seen[bi]&bm)){
                uint8_t ox=s_prev_x[vid];
                uint8_t oc=(uint8_t)(ox>>3);
                /* Moving inside one 8px tile needs no coarse re-render: the
                 * post-pass directly replaces the old seam mask with the new
                 * one, which is the cheapest possible delta-X in-paint for
                 * this FULL/single-material benchmark. Only a tile the seam
                 * LEAVES must be re-materialized so its old overlay disappears
                 * and current front-to-back ownership is restored. */
                if(oc!=col)
                    seam_dirty_col(oc);
            }
            /* A newly appearing seam likewise needs no coarse redraw: the
             * current FULL tile is already the right owner/material and the
             * overlay simply installs the physical line. */
            s_prev_x[vid]=ux;
            s_cur_seen[bi]|=bm;
        }
    }
}

/* Finish the horizontal delta after every current seam has recorded itself.
 * The common case has no disappearing vertex: four byte tests, no descriptor
 * cross-product. A vanished seam dirties only its old tile so current
 * front-to-back geometry can reclaim the released strip. */
void tsp_polar_seam_prepare_dirty(void) BANKED
{
    uint8_t bi;

    /* Diagnostic correctness rung: force every CURRENT physical-seam column
     * through the ordinary coarse materializer before the overlay is applied.
     * Horizontal history alone cannot see a seam that stays in the same tile
     * while its vertical extent shrinks; retained cells can therefore preserve
     * last frame's taller line. If the exact census loses its tile-edge ghosts
     * with this enabled, the missing state is specifically previous/current Y
     * extent rather than another projection/ownership error. */
    g_tspf_seam_dirty_cols[0] |= g_tspf_seam_cur_cols[0];
    g_tspf_seam_dirty_cols[1] |= g_tspf_seam_cur_cols[1];
    g_tspf_seam_dirty_cols[2] |= g_tspf_seam_cur_cols[2];

    if(g_tspf_seam_history_valid){
        for(bi=0u;bi<4u;++bi){
            uint8_t gone=(uint8_t)(s_prev_seen[bi] & (uint8_t)~s_cur_seen[bi]);
            if(gone){
                uint8_t b;
                for(b=0u;b<8u;++b){
                    uint8_t bm=(uint8_t)(1u<<b);
                    if(gone&bm)
                        seam_dirty_col((uint8_t)(s_prev_x[(uint8_t)((bi<<3)+b)]>>3));
                }
            }
        }
    }

    for(bi=0u;bi<4u;++bi){
        s_prev_seen[bi]=s_cur_seen[bi];
        s_cur_seen[bi]=0u;
    }
    g_tspf_seam_history_valid=1u;
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
        for(i=0u;i<TSP_SEAM_EXTRA_COUNT;++i){
            if(mask==k_extra_seam_mask[i]){ *attr=0u; return (uint8_t)(TSP_SEAM_BASE_COUNT+i); }
            if(mask==k_extra_seam_reflect[i]){ *attr=TSP_ATTR_FLIPX; return (uint8_t)(TSP_SEAM_BASE_COUNT+i); }
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

/* The coarse run depth field is linear in screen X, but the materializer's
 * endpoint cache used the snapped 8-pixel ownership boundary as though it were
 * the authored vertex. That is harmless for X and very wrong for Y on oblique
 * faces: a corner four pixels inside the tile can differ by ~12 vertical
 * pixels. Recover the half-height at the descriptor's TRUE pixel X from a
 * surviving adjacent run. This is intentionally banked and runs once after
 * all coarse columns, keeping the 68-byte renderer-bank margin intact. */
static int16_t seam_step_subpx(int16_t step,uint8_t px)
{
    if(px==1u) return (int16_t)(step>>3);
    if(px==2u) return (int16_t)(step>>2);
    if(px==3u) return (int16_t)((step>>2)+(step>>3));
    if(px>=4u) return (int16_t)(step>>1);
    return 0;
}

static uint8_t seam_half_from_run(const TSPThinRun *r,uint8_t vid,uint8_t x,uint8_t *dist)
{
    int16_t dxl,dxr,dx,q,dq;
    uint8_t adl,adr,ad,inv;

    if(r->v0!=vid && r->v1!=vid) return 0xffu;

    /* Program v0/v1 are cyclic authored endpoints; after clipping/walking they
     * are not a reliable statement that v0 is the LEFT screen endpoint. The
     * previous version made exactly that assumption, which explains the few
     * large strafe/far-rotate Y misses. A physical vertex must be close to one
     * of this run's two snapped ownership boundaries, so select that boundary
     * geometrically instead. */
    dxl=(int16_t)x-(int16_t)((uint16_t)r->c0<<3);
    dxr=(int16_t)x-(int16_t)((uint16_t)(r->c1+1u)<<3);
    adl=(uint8_t)(dxl<0 ? -dxl : dxl);
    adr=(uint8_t)(dxr<0 ? -dxr : dxr);
    if(adl<=adr){
        dx=dxl; ad=adl; q=r->iq;
    }else{
        dx=dxr; ad=adr;
        /* inv1 is preserved as the exact coarse right-edge inverse depth even
         * when inv_mid carries a canonical connected-corner half-height. */
        q=(int16_t)((uint16_t)r->inv1<<6);
    }

    if(ad>4u) return 0xffu;
    dq=seam_step_subpx(r->step,ad);
    q=(int16_t)(dx<0 ? q-dq : q+dq);
    if(q<0) inv=0u;
    else if(q>=((int16_t)255<<6)) inv=255u;
    else inv=(uint8_t)((q+32)>>6);
    *dist=ad;
    return (uint8_t)(inv>>1);
}

void tsp_polar_refine_seam_heights(uint8_t run_count) BANKED
{
    uint8_t i,j;

    for(i=0u;i<g_tspf_seam_desc_count;++i){
        uint8_t vid=g_tspf_seam_vid[i];
        uint8_t x=g_tspf_seam_x[i];
        uint8_t best=0xffu,bestd=0xffu,matches=0u;
        uint8_t lo=0xffu,hi=0u,old;

        if(vid>=32u) continue;
        old=g_tspf_seam_vertex_half[vid];
        for(j=0u;j<run_count;++j){
            uint8_t d=0xffu;
            uint8_t h=seam_half_from_run(&g_runs[j],vid,x,&d);
            if(h!=0xffu){
                ++matches;
                if(h<lo) lo=h;
                if(h>hi) hi=h;
                if(d<bestd){
                    best=h;
                    bestd=d;
                }
            }
        }

        if(best!=0xffu){
            if(matches>1u && old!=0xffu){
                /* Normally envelope_join_connected() already reconciles the
                 * two faces and its cached canonical Y is the best answer.
                 * There is one nasty exception: a wall plane closer than the
                 * 10-unit reciprocal near limit saturates BEFORE its oblique
                 * ray factor is applied. Then the two mathematically-equal
                 * corner estimates split badly (the rotation offenders were
                 * almost exactly 2:1). On this E1M1 course there is no far
                 * plane clamp, so the saturated near-plane estimate is the
                 * smaller one. Only override when the disagreement is far
                 * beyond ordinary quantization/subpixel error. */
                if((uint8_t)(hi-lo)>12u)
                    g_tspf_seam_vertex_half[vid]=hi;
                continue;
            }
            /* With exactly one surviving face there is no competing
             * canonical corner to protect. Matching the authored vertex AND
             * landing within four pixels of this run boundary is already the
             * locality proof. The old <=12 guard was backwards here: it kept
             * precisely the largest collapsed-face Y errors (for example the
             * 17.5px strafe miss at vertex 20). Trust the true-X evaluation. */
            g_tspf_seam_vertex_half[vid]=best;
        }
    }
}

void tsp_polar_subcolumn_seams_fast(void) BANKED
{
    uint8_t i,row;

    /* Descriptor-major compositor. The first current seam touching a cell
     * REPLACES any previous/coarse border semantics; later current seams in
     * that same cell merge with the mask written by this pass. A 360-bit
     * touched set (45 bytes RAM) is much cheaper than a 360-byte screen mask
     * and avoids rescanning every descriptor once per active coarse column. */
    for(i=0u;i<(uint8_t)((TSP_MAP_CELLS+7u)/8u);++i)
        s_overlay_touched[i]=0u;

    for(i=0u;i<g_tspf_seam_desc_count;++i){
        uint8_t vid,half,x,col,bit,first,last;
        int8_t top_tile;
        uint16_t idx;

        x=g_tspf_seam_x[i];
        col=(uint8_t)(x>>3);
        if(col>=TSP_COLS) continue;
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

        idx=(uint16_t)((uint16_t)first*20u+col);
        for(row=first;;++row,idx+=20u){
            uint8_t tb=(uint8_t)(idx>>3);
            uint8_t tm=(uint8_t)(1u<<(idx&7u));
            uint16_t old=g_map[idx];
            uint16_t id=(uint16_t)(old&TSP_TILE_ID_MASK);
            uint8_t mask,code;
            uint16_t attr=0u,nw;

            if(!(s_overlay_touched[tb]&tm)){
                /* First physical seam this frame: discard the old snapped
                 * 0/7 border or last frame's seam mask entirely. The current
                 * physical seam set is authoritative. */
                if(!((id>=3u && id<7u) ||
                     (id>=TSP_SEAM_TILE_BASE &&
                      id<(TSP_SEAM_TILE_BASE+TSP_SEAM_TILE_COUNT))))
                    goto seam_row_done;
                mask=bit;
                s_overlay_touched[tb]|=tm;
            } else {
                /* This cell was already rewritten by a current descriptor.
                 * Decode that small current mask, then add this seam. */
                uint8_t ci;
                if(id<TSP_SEAM_TILE_BASE ||
                   id>=(TSP_SEAM_TILE_BASE+TSP_SEAM_TILE_COUNT))
                    goto seam_row_done;
                ci=(uint8_t)(id-TSP_SEAM_TILE_BASE);
                if(ci<TSP_SEAM_BASE_COUNT){
                    mask=(old&TSP_ATTR_FLIPX) ?
                         g_tsp_seam_reflect_home[ci] :
                         g_tsp_seam_mask_home[ci];
                } else {
                    ci=(uint8_t)(ci-TSP_SEAM_BASE_COUNT);
                    mask=(old&TSP_ATTR_FLIPX) ?
                         k_extra_seam_reflect[ci] :
                         k_extra_seam_mask[ci];
                }
                mask|=bit;
            }

            code=seam_mask_to_code(mask,&attr);
            if(code==0xffu){
                /* Future uncensused crowd: retain the two outer CURRENT
                 * physical boundaries, never resurrect a coarse tile edge. */
                uint8_t lo=0u,hi=7u;
                while(lo<8u && !(mask&(uint8_t)(1u<<lo))) ++lo;
                while(hi>lo && !(mask&(uint8_t)(1u<<hi))) --hi;
                mask=(uint8_t)((1u<<lo)|(1u<<hi));
                code=seam_mask_to_code(mask,&attr);
                if(code==0xffu) goto seam_row_done;
            }

            nw=(uint16_t)(TSP_SEAM_TILE_BASE+code+attr);
            if(nw!=old){
                g_map[idx]=nw;
                if(g_polar_nt_row_min[row]==0xffu || col<g_polar_nt_row_min[row])
                    g_polar_nt_row_min[row]=col;
                if(col>g_polar_nt_row_max[row])
                    g_polar_nt_row_max[row]=col;
            }

seam_row_done:
            if(row==last) break;
        }
    }

    /* Coarse materialization has consumed this frame's horizontal delta.
     * Clear here in bank 254 rather than spending fixed/HOME bytes. */
    g_tspf_seam_dirty_cols[0]=0u;
    g_tspf_seam_dirty_cols[1]=0u;
    g_tspf_seam_dirty_cols[2]=0u;
}

#endif
