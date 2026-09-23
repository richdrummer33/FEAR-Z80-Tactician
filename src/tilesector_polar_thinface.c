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
/* FULL-only seam composition processes symmetric row pairs. Only the top
 * member (rows 0..8) needs current-frame overlap state; its bottom partner is
 * written at the same instant. */
static uint8_t s_overlay_touched[(TSP_COLS*9u+7u)/8u];

/* Seam-Y refinement is naturally vertex-major: a physical vertex appears at
 * one screen X per frame. Index the current descriptors by vertex once, then
 * visit each run endpoint instead of testing every descriptor against every
 * run (D*R). This keeps the exact same candidate/reconciliation policy while
 * changing the search cost to O(D + R). */
static uint8_t s_refine_x[32];
static uint8_t s_refine_best[32];
static uint8_t s_refine_bestd[32];
static uint8_t s_refine_lo[32];
static uint8_t s_refine_hi[32];
static uint8_t s_refine_matches[32];
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

/* Exact seam-mask encoder. Bits 0..4 are the tile code; bit 7 means
 * horizontal reflection; 0xff means uncensused/unsupported. The first-touch
 * single-line path never reaches this table, so the 256 ROM bytes buy O(1)
 * encoding exactly where masks overlap and the former shift/scan loops were
 * most expensive. */
static const uint8_t k_seam_encode[256]={
    0xffu, 0x00u, 0x01u, 0x04u, 0x02u, 0x05u, 0x0bu, 0xffu, 0x03u, 0x06u, 0x0cu, 0xffu, 0x10u, 0xffu, 0xffu, 0xffu,
    0x83u, 0x07u, 0x0du, 0xffu, 0x11u, 0xffu, 0xffu, 0xffu, 0x13u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0x82u, 0x08u, 0x0eu, 0x16u, 0x12u, 0x19u, 0xffu, 0xffu, 0x91u, 0x17u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0x90u, 0x1au, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0x81u, 0x09u, 0x0fu, 0xffu, 0x8eu, 0x18u, 0x14u, 0xffu, 0x8du, 0x1bu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0x8cu, 0x15u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0x8bu, 0xffu, 0x94u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0x80u, 0x0au, 0x89u, 0xffu, 0x88u, 0xffu, 0xffu, 0xffu, 0x87u, 0x1cu, 0x95u, 0xffu, 0x9au, 0xffu, 0xffu, 0xffu,
    0x86u, 0x9cu, 0x9bu, 0xffu, 0x97u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0x85u, 0xffu, 0x98u, 0xffu, 0x99u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0x84u, 0xffu, 0xffu, 0xffu, 0x96u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu
};

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

    /* Build a compact vertex -> true-X index for only the physical seams that
     * exist this frame. The measured exact sweeps contain no duplicate vertex
     * descriptors; if one ever appears, the last copy is equivalent because a
     * physical vertex has one projected X. */
    /* 0xff is outside the 0..159 pixel aperture, so the X table doubles
     * as the active-vertex map. Clearing 32 linear bytes is cheaper on Z80
     * than rebuilding/test-shifting a four-byte bitset at every run endpoint. */
    for(i=0u;i<32u;++i) s_refine_x[i]=0xffu;
    for(i=0u;i<g_tspf_seam_desc_count;++i){
        uint8_t vid=g_tspf_seam_vid[i];
        if(vid>=32u) continue;
        s_refine_x[vid]=g_tspf_seam_x[i];
        s_refine_best[vid]=0xffu;
        s_refine_bestd[vid]=0xffu;
        s_refine_lo[vid]=0xffu;
        s_refine_hi[vid]=0u;
        s_refine_matches[vid]=0u;
    }

    /* Invert the former descriptor x run cross-product. Each run can offer at
     * most its two endpoint vertices, so the common work is about 2R rather
     * than D*R. seam_half_from_run() and the winner policy are unchanged. */
    for(j=0u;j<run_count;++j){
        TSPThinRun *r=&g_runs[j];
        uint8_t k;
        for(k=0u;k<2u;++k){
            uint8_t vid=(uint8_t)(k ? r->v1 : r->v0);
            uint8_t x,d,h;
            if(vid>=32u) continue;
            x=s_refine_x[vid];
            if(x==0xffu) continue;

            d=0xffu;
            h=seam_half_from_run(r,vid,x,&d);
            if(h==0xffu) continue;

            ++s_refine_matches[vid];
            if(h<s_refine_lo[vid]) s_refine_lo[vid]=h;
            if(h>s_refine_hi[vid]) s_refine_hi[vid]=h;
            if(d<s_refine_bestd[vid]){
                s_refine_best[vid]=h;
                s_refine_bestd[vid]=d;
            }
        }
    }

    for(i=0u;i<g_tspf_seam_desc_count;++i){
        uint8_t vid=g_tspf_seam_vid[i];
        uint8_t best,matches,old;
        if(vid>=32u) continue;

        best=s_refine_best[vid];
        if(best==0xffu) continue;
        matches=s_refine_matches[vid];
        old=g_tspf_seam_vertex_half[vid];

        if(matches>1u && old!=0xffu){
            /* Normally envelope_join_connected() already reconciles the two
             * faces and its cached canonical Y is the best answer. There is
             * one nasty exception: a wall plane closer than the 10-unit
             * reciprocal near limit saturates BEFORE its oblique ray factor
             * is applied. Then the two mathematically-equal corner estimates
             * split badly. Only override when disagreement is extreme. */
            if((uint8_t)(s_refine_hi[vid]-s_refine_lo[vid])>12u)
                g_tspf_seam_vertex_half[vid]=s_refine_hi[vid];
            continue;
        }

        /* With exactly one surviving face there is no competing canonical
         * corner to protect. The same true-X/locality test above is sufficient
         * evidence, so keep the proven sole-face policy unchanged. */
        g_tspf_seam_vertex_half[vid]=best;
    }
}

void tsp_polar_subcolumn_seams_fast(void) BANKED
{
    uint8_t i,row,rowb;

    /* Descriptor-major compositor. The first current seam touching a cell
     * REPLACES any previous/coarse border semantics; later current seams in
     * that same cell merge with the mask written by this pass. A 360-bit
     * touched set (45 bytes RAM) is much cheaper than a 360-byte screen mask
     * and avoids rescanning every descriptor once per active coarse column. */
    for(i=0u;i<(uint8_t)((TSP_COLS*9u+7u)/8u);++i)
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

        {
            /* The overwhelmingly common case is exactly one seam in a cell.
             * Its tile word depends only on x&7, so compute it ONCE per
             * descriptor instead of calling the general mask encoder for every
             * vertical cell. Multi-seam cells still take the exact old merge
             * path below. */
            uint8_t sx=(uint8_t)(x&7u);
            uint8_t single_code;
            uint16_t single_attr,single_nw;

            if(sx>=4u){
                single_code=(uint8_t)(7u-sx);
                single_attr=TSP_ATTR_FLIPX;
            } else {
                single_code=sx;
                single_attr=0u;
            }
            single_nw=(uint16_t)(TSP_SEAM_TILE_BASE+single_code+single_attr);

            idx=(uint16_t)((uint16_t)first*20u+col);
            {
                /* Pair-local material validation happens only on first
                 * touch below. Overlaps are already current-pass seam words,
                 * so they no longer need duplicate ID/range checks. */
                uint16_t idxb=(uint16_t)((uint16_t)last*20u+col);
                uint16_t old=g_map[idx];
                uint16_t oldb=g_map[idxb];
                uint8_t tb=(uint8_t)(idx>>3);
                uint8_t tm=(uint8_t)(1u<<(idx&7u));

                row=first;
                rowb=last;
                for(;;){
                    uint16_t nw;

                    if(!(s_overlay_touched[tb]&tm)){
                        uint16_t id=(uint16_t)(old&TSP_TILE_ID_MASK);
                        uint16_t idb=(uint16_t)(oldb&TSP_TILE_ID_MASK);
                        /* First touch still has to prove that BOTH symmetric
                         * coarse cells are wall/seam material. Once claimed,
                         * later descriptors can trust the current-pass seam
                         * word without repeating this class test. */
                        if(!((id>=3u && id<7u) ||
                             (id>=TSP_SEAM_TILE_BASE &&
                              id<(TSP_SEAM_TILE_BASE+TSP_SEAM_TILE_COUNT))) ||
                           !((idb>=3u && idb<7u) ||
                             (idb>=TSP_SEAM_TILE_BASE &&
                              idb<(TSP_SEAM_TILE_BASE+TSP_SEAM_TILE_COUNT))))
                            goto seam_pair_done;
                        s_overlay_touched[tb]|=tm;
                        nw=single_nw;
                    } else {
                        /* Top-pair touched state is sufficient: the bottom
                         * partner was written simultaneously. Therefore old is
                         * guaranteed to be a seam word from this same pass and
                         * needs no repeated class/range validation. */
                        uint8_t mask,enc,ci;
                        ci=(uint8_t)((old&TSP_TILE_ID_MASK)-TSP_SEAM_TILE_BASE);
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

                        enc=k_seam_encode[mask];
                        if(enc==0xffu){
                            uint8_t lo=0u,hi=7u;
                            while(lo<8u && !(mask&(uint8_t)(1u<<lo))) ++lo;
                            while(hi>lo && !(mask&(uint8_t)(1u<<hi))) --hi;
                            mask=(uint8_t)((1u<<lo)|(1u<<hi));
                            enc=k_seam_encode[mask];
                            if(enc==0xffu) goto seam_pair_done;
                        }
                        nw=(uint16_t)(TSP_SEAM_TILE_BASE+(enc&31u));
                        if(enc&0x80u) nw=(uint16_t)(nw+TSP_ATTR_FLIPX);
                    }

                    if(nw!=old){
                        g_map[idx]=nw;
                        if(g_polar_nt_row_min[row]==0xffu ||
                           col<g_polar_nt_row_min[row])
                            g_polar_nt_row_min[row]=col;
                        if(col>g_polar_nt_row_max[row])
                            g_polar_nt_row_max[row]=col;
                    }
                    if(nw!=oldb){
                        g_map[idxb]=nw;
                        if(g_polar_nt_row_min[rowb]==0xffu ||
                           col<g_polar_nt_row_min[rowb])
                            g_polar_nt_row_min[rowb]=col;
                        if(col>g_polar_nt_row_max[rowb])
                            g_polar_nt_row_max[rowb]=col;
                    }

seam_pair_done:
                    if((uint8_t)(row+1u)>=rowb) break;
                    ++row;
                    --rowb;
                    idx=(uint16_t)(idx+20u);
                    idxb=(uint16_t)(idxb-20u);
                    old=g_map[idx];
                    oldb=g_map[idxb];

                    if(tm&0xf0u){
                        tm=(uint8_t)(tm>>4);
                        tb=(uint8_t)(tb+3u);
                    } else {
                        tm=(uint8_t)(tm<<4);
                        tb=(uint8_t)(tb+2u);
                    }
                }
            }
        }
    }

    /* Coarse materialization has consumed this frame's horizontal delta.
     * Clear here in bank 254 rather than spending fixed/HOME bytes. */
    g_tspf_seam_dirty_cols[0]=0u;
    g_tspf_seam_dirty_cols[1]=0u;
    g_tspf_seam_dirty_cols[2]=0u;
}

#endif
