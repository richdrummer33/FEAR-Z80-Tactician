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

/* Classify the raw nametable word without first materializing a 9-bit tile
 * ID. Attributes start at bit 9, so bit 8 selects the only two substrate
 * ranges accepted by this FULL-only compositor: IDs 3..6 when clear and
 * seam IDs 412..440 (low byte 156..184) when set. The low-byte subtraction
 * is therefore exact over the 0..511 tile-ID domain and avoids repeated
 * 16-bit masks/subtracts in the hottest row-pair path. */
#define TSP_SEAM_SUBSTRATE_WORD(w) \
    (((w)&0x0100u) ? \
     ((uint8_t)((uint8_t)(w)-(uint8_t)TSP_SEAM_TILE_BASE)<TSP_SEAM_TILE_COUNT) : \
     ((uint8_t)((uint8_t)(w)-(uint8_t)TSP_TILE_FULL_BASE)<4u))

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
                uint16_t *mapt=&g_map[idx];
                uint16_t *mapb=&g_map[idxb];
                uint16_t old=*mapt;
                uint16_t oldb=*mapb;
                uint8_t *touch=&s_overlay_touched[idx>>3];
                uint8_t tm=(uint8_t)(1u<<(idx&7u));

                row=first;
                rowb=last;
                for(;;){
                    uint16_t nw;

                    if(!(*touch&tm)){
                        /* First touch still has to prove that BOTH symmetric
                         * coarse cells are wall/seam material. Once claimed,
                         * later descriptors can trust the current-pass seam
                         * word without repeating this class test. */
                        if(!TSP_SEAM_SUBSTRATE_WORD(old) ||
                           !TSP_SEAM_SUBSTRATE_WORD(oldb))
                            goto seam_pair_done;
                        *touch|=tm;
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
                        *mapt=nw;
                        if(g_polar_nt_row_min[row]==0xffu ||
                           col<g_polar_nt_row_min[row])
                            g_polar_nt_row_min[row]=col;
                        if(col>g_polar_nt_row_max[row])
                            g_polar_nt_row_max[row]=col;
                    }
                    if(nw!=oldb){
                        *mapb=nw;
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
                    mapt+=20u;
                    mapb-=20u;
                    old=*mapt;
                    oldb=*mapb;

                    if(tm&0xf0u){
                        tm=(uint8_t)(tm>>4);
                        touch+=3u;
                    } else {
                        tm=(uint8_t)(tm<<4);
                        touch+=2u;
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


/* ------------------------------------------------------------------------- */
/* Exact-X boundary composite experiment.
 *
 * The front envelope has already solved visibility.  This pass preserves that
 * answer at 1px X only in the hardware tile(s) containing a visible ownership
 * handoff.  Ordinary columns stay on the existing coarse/retained fast path.
 *
 * Preparation happens before coarse materialization: it computes the small
 * mixed-owner tile vocabulary for this frame and flags current/previous
 * boundary columns so retained tiles cannot hide stale composites.  Apply runs
 * after coarse materialization and replaces only the mixed cells.
 */
#ifndef TSPF_BOUNDARY_COMPOSITE
#define TSPF_BOUNDARY_COMPOSITE 0
#endif

#if defined(__SDCC) && TSPF_BOUNDARY_COMPOSITE
#include "e1env_depth_edges_bank.h"
#include "e1env_plane_meta.h"

#define TSP_BC_SLOTS 18u
#define TSP_BC_PATCH_MAX 32u
#define TSP_BC_BASE0 412u
#define TSP_BC_BASE1 430u
#define TSP_BC_SAFE_PUBLISHES 3u

extern uint16_t g_map[TSP_MAP_CELLS];
extern uint8_t g_polar_nt_cov_cur[60];
extern uint8_t g_polar_nt_row_min[TSP_ROWS];
extern uint8_t g_polar_nt_row_max[TSP_ROWS];
extern uint8_t g_tspf_boundary_dirty_by_col[TSP_COLS];
extern uint8_t g_tspf_boundary_any_dirty;
extern uint8_t g_e1env_program[];
extern uint16_t g_corner_bearing_q12[32];
extern uint8_t g_corner_bearing_valid[4];
extern const uint8_t g_e1env_center_col_lut[1025];
extern volatile uint8_t g_tspf_appearance_mode;
extern volatile uint8_t g_tspf_boundary_publish_tick;

/* Reuse the old, currently dead seam descriptor arena as three 32-byte event
 * vectors while preparing.  No seam post-pass is linked in this experiment. */
extern uint8_t g_tspf_seam_desc_count;
extern uint8_t g_tspf_seam_x[32];
extern uint8_t g_tspf_seam_vid[32];
extern uint8_t g_tspf_seam_vertex_half[32];

typedef struct TSPBoundaryRun {
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
} TSPBoundaryRun;
extern TSPBoundaryRun g_runs[];

/* VBlank-facing staging ABI.  Pattern IDs 412..447 are split into two banks;
 * the old seam tiles are deliberately not uploaded in this build. */
uint8_t g_tspf_boundary_pattern_data[TSP_BC_SLOTS * 16u];
volatile uint8_t g_tspf_boundary_patterns_pending;
uint16_t g_tspf_boundary_pattern_base;
uint8_t g_tspf_boundary_pattern_count;

/* Tiny diagnostics visible to Gearsystem/debuggers. */
volatile uint8_t g_tspf_boundary_last_patterns;
volatile uint8_t g_tspf_boundary_last_patches;
volatile uint8_t g_tspf_boundary_skip_reason;
volatile uint8_t g_tspf_boundary_last_crowded;
volatile uint8_t g_tspf_boundary_last_local_fallbacks;
#if TSPF_PROFILE_HOOKS
/* Profile-only copy of the physical vertex identity for each exact-X event.
 * The legacy seam vectors are reused for x/left-owner/right-owner. */
uint8_t g_tspf_boundary_vid[32];
#endif

static uint8_t s_pattern_hash[TSP_BC_SLOTS];
static uint8_t s_patch_pos[TSP_BC_PATCH_MAX];
static uint16_t s_patch_word[TSP_BC_PATCH_MAX];
static uint8_t s_patch_count;
static uint8_t s_work[32];
/* Only columns that actually received dynamic words need forced coarse
 * restoration next update. Candidate/crowded columns are allowed to remain on
 * the normal retained coarse path. */
static uint8_t s_exact_cols[3];
static uint8_t s_prev_cols[3];
static uint8_t s_bank_used[2];
static uint8_t s_bank_released[2];
static uint8_t s_bank_release_tick[2];
static uint8_t s_prev_bank=0xffu;
static uint8_t s_target_bank=0xffu;
static uint8_t s_prepared;

/* Exact copy of the p99 line stepping used by the normal top-edge vocabulary.
 * The compositor uses it only for the few mixed-owner tiles. */
static const uint8_t k_bc_edge_step[25][8] = {
    {0,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,1},
    {0,0,1,1,1,1,2,2},
    {0,0,1,1,2,2,3,3},
    {0,1,1,2,2,3,3,4},
    {0,1,1,2,3,4,4,5},
    {0,1,2,3,3,4,5,6},
    {0,1,2,3,4,5,6,7},
    {0,1,2,3,5,6,7,8},
    {0,1,3,4,5,6,8,9},
    {0,1,3,4,6,7,9,10},
    {0,2,3,5,6,8,9,11},
    {0,2,3,5,7,9,10,12},
    {0,2,4,6,7,9,11,13},
    {0,2,4,6,8,10,12,14},
    {0,2,4,6,9,11,13,15},
    {0,2,5,7,9,11,14,16},
    {0,2,5,7,10,12,15,17},
    {0,3,5,8,10,13,15,18},
    {0,3,5,8,11,14,16,19},
    {0,3,6,9,11,14,17,20},
    {0,3,6,9,12,15,18,21},
    {0,3,6,9,13,16,19,22},
    {0,3,7,10,13,16,20,23},
    {0,3,7,10,14,17,21,24}
};

static const uint8_t k_bc_rev4[16] = {
    0x0,0x8,0x4,0xc,0x2,0xa,0x6,0xe,0x1,0x9,0x5,0xd,0x3,0xb,0x7,0xf
};

static uint8_t bc_rev8(uint8_t x)
{
    return (uint8_t)((k_bc_rev4[x&15u]<<4)|k_bc_rev4[x>>4]);
}

static void bc_clear3(uint8_t *p)
{
    p[0]=0u; p[1]=0u; p[2]=0u;
}

static void bc_mark_col(uint8_t bits[3],uint8_t col)
{
    bits[col>>3] |= (uint8_t)(1u<<(col&7u));
}

static uint8_t bc_col_marked(const uint8_t bits[3],uint8_t col)
{
    return (uint8_t)(bits[col>>3]&(uint8_t)(1u<<(col&7u)));
}

static int16_t bc_rel(uint16_t bearing,uint16_t yawq)
{
    uint16_t d=(uint16_t)((bearing-yawq)&4095u);
    return (int16_t)(d>=2048u ? (int16_t)d-4096 : (int16_t)d);
}

static uint8_t bc_choose_bank(void)
{
    uint8_t c,t=g_tspf_boundary_publish_tick;
    for(c=0u;c<2u;++c){
        if(c==s_prev_bank) continue;
        if(!s_bank_used[c]) return c;
        if(s_bank_released[c] &&
           (uint8_t)(t-s_bank_release_tick[c])>=TSP_BC_SAFE_PUBLISHES)
            return c;
    }
    return 0xffu;
}

static uint8_t bc_inv_from_q6(int16_t q)
{
    int16_t v=(int16_t)((q+32)>>6);
    if(v<0) return 0u;
    if(v>255) return 255u;
    return (uint8_t)v;
}

static void bc_fill_top(uint8_t owner,uint8_t col,const TSPState *s,
                        uint8_t x0,uint8_t x1,int8_t top[8])
{
    uint8_t x,sid,axis,il,ir,mag;
    int16_t dq4,tl,tr,d;

    if(owner==0xffu){
        for(x=x0;x<=x1;++x) top[x]=127;
        return;
    }
    sid=(uint8_t)(owner&31u);
    if(sid>=30u){
        for(x=x0;x<=x1;++x) top[x]=127;
        return;
    }
    axis=k_e1env_depth_axis[sid];
    dq4=(int16_t)(k_e1env_plane_c[sid]-(axis?s->y_q4:s->x_q4));
    if(axis==0u) e1env_depth_edges_0(s->yaw,col,col,dq4);
    else         e1env_depth_edges_1(s->yaw,col,col,dq4);

    il=bc_inv_from_q6(g_e1env_depth_iq);
    ir=g_e1env_depth_end_inv;
    tl=(int16_t)(71-(int16_t)(il>>1));
    tr=(int16_t)(71-(int16_t)(ir>>1));
    d=(int16_t)(tr-tl);
    mag=(uint8_t)(d<0 ? -d : d);
    if(mag>24u) mag=24u;
    if(d>=0){
        for(x=x0;x<=x1;++x)
            top[x]=(int8_t)(tl+(int16_t)k_bc_edge_step[mag][x]);
    }else{
        for(x=x0;x<=x1;++x)
            top[x]=(int8_t)(tr+(int16_t)k_bc_edge_step[mag][(uint8_t)(7u-x)]);
    }
}

static uint8_t bc_pattern_index(uint8_t *flip_out)
{
    uint8_t packed[16];
    uint8_t i,j,h=0x5du,flip=0u;
    uint8_t count=g_tspf_boundary_pattern_count;

    /* Geometry-only dynamic tiles use only color 1 (OUT) and color 4 (MID).
     * Keep just those two active bitplanes in WRAM: 16 bytes/tile rather than
     * native 32-byte 4bpp. VBlank expands them immediately before VRAM upload. */
    for(i=0u;i<8u;++i){
        packed[(uint8_t)(i+i)]=s_work[(uint8_t)(i*4u)];
        packed[(uint8_t)(i+i+1u)]=s_work[(uint8_t)(i*4u+2u)];
    }

    /* Canonicalize under free VDP HFLIP. */
    for(i=0u;i<16u;++i){
        uint8_t r=bc_rev8(packed[i]);
        if(r==packed[i]) continue;
        flip=(uint8_t)(r<packed[i]);
        break;
    }
    if(flip)
        for(i=0u;i<16u;++i) packed[i]=bc_rev8(packed[i]);

    for(i=0u;i<16u;++i)
        h=(uint8_t)((h<<1)|(h>>7))^packed[i];

    for(j=0u;j<count;++j){
        uint8_t *p;
        if(s_pattern_hash[j]!=h) continue;
        p=&g_tspf_boundary_pattern_data[(uint16_t)j<<4];
        for(i=0u;i<16u && p[i]==packed[i];++i) {}
        if(i==16u){
            *flip_out=flip;
            return j;
        }
    }
    if(count>=TSP_BC_SLOTS) return 0xffu;
    {
        uint8_t *p=&g_tspf_boundary_pattern_data[(uint16_t)count<<4];
        for(i=0u;i<16u;++i) p[i]=packed[i];
    }
    s_pattern_hash[count]=h;
    g_tspf_boundary_pattern_count=(uint8_t)(count+1u);
    *flip_out=flip;
    return count;
}

static uint8_t bc_add_patch(uint8_t row,uint8_t col,uint16_t word)
{
    uint8_t n=s_patch_count;
    if(n>=TSP_BC_PATCH_MAX) return 0u;
    s_patch_pos[n]=(uint8_t)(row*20u+col);
    s_patch_word[n]=word;
    s_patch_count=(uint8_t)(n+1u);
    return 1u;
}

static void bc_dirty_row(uint8_t row,uint8_t col)
{
    if(g_polar_nt_row_min[row]==0xffu || col<g_polar_nt_row_min[row])
        g_polar_nt_row_min[row]=col;
    if(col>g_polar_nt_row_max[row])
        g_polar_nt_row_max[row]=col;
}

static void bc_own_cell(uint8_t row,uint8_t col)
{
    g_polar_nt_cov_cur[(uint8_t)(col+col+col+(row>>3))] |=
        (uint8_t)(1u<<(row&7u));
}

static uint8_t bc_build_tile(uint8_t first,uint8_t last,uint8_t col,
                             const TSPState *s)
{
    uint8_t owner[8],line_mask=0u,lx,e,row,ly;
    int8_t top[8];

    for(lx=0u;lx<8u;++lx) owner[lx]=g_tspf_seam_vid[first];

    for(e=first;e<=last;++e){
        uint8_t split=(uint8_t)(g_tspf_seam_x[e]&7u);
        uint8_t lo=g_tspf_seam_vid[e], ro=g_tspf_seam_vertex_half[e];
        uint8_t connected=(uint8_t)(lo!=0xffu && ro!=0xffu && (lo&0x80u));
        uint8_t physical=(uint8_t)((lo!=0xffu && (lo&0x40u)) ||
                                   (ro!=0xffu && (ro&0x20u)));
        if(!split) continue;
        for(lx=split;lx<8u;++lx) owner[lx]=ro;
        if(physical && !connected) line_mask|=(uint8_t)(1u<<split);
    }

    /* This first rung deliberately handles wall<->wall handoffs only.  E1M1's
     * enclosed projection sweeps are entirely in that class.  A void handoff
     * needs a special row-9 horizon composite rather than blindly VFLIPing row
     * 8, so leave it on the proven coarse fallback for now. */
    for(lx=0u;lx<8u;++lx)
        if(owner[lx]==0xffu) return 1u;

    lx=0u;
    while(lx<8u){
        uint8_t x1=lx;
        while(x1<7u && owner[(uint8_t)(x1+1u)]==owner[lx]) ++x1;
        bc_fill_top(owner[lx],col,s,lx,x1,top);
        lx=(uint8_t)(x1+1u);
    }

    for(row=0u;row<9u;++row){
        uint8_t all_out=1u,all_wall=1u,flip,index;
        for(ly=0u;ly<8u;++ly){
            uint8_t y=(uint8_t)(row*8u+ly);
            uint8_t sem[8],outm=0u,wallm=0u;
            for(lx=0u;lx<8u;++lx){
                int8_t ty=top[lx];
                uint8_t v;
                if((int16_t)y<(int16_t)ty) v=0u;       /* ceiling */
                else if((int16_t)y==(int16_t)ty) v=2u; /* black top edge */
                else v=1u;                              /* wall */
                sem[lx]=v;
            }
            for(e=first;e<=last;++e){
                uint8_t split=(uint8_t)(g_tspf_seam_x[e]&7u);
                if(split && (line_mask&(uint8_t)(1u<<split))){
                    uint8_t a=sem[(uint8_t)(split-1u)],b=sem[split];
                    /* A non-connected authored endpoint is an EXTERNAL
                     * silhouette, not merely a top-edge discontinuity.  The
                     * old true-X seam rung drew that line through ordinary
                     * wall rows; preserve the same visual rule here at the
                     * exact pixel X.  Do not extend it into ceiling above both
                     * faces, and let the existing horizontal top edge own the
                     * exact edge pixel itself. */
                    if(a==1u || b==1u)
                        sem[split]=2u;
                }
            }
            for(lx=0u;lx<8u;++lx){
                uint8_t bit=(uint8_t)(0x80u>>lx);
                if(sem[lx]==0u) outm|=bit;
                else if(sem[lx]==1u) wallm|=bit;
                if(sem[lx]!=0u) all_out=0u;
                if(sem[lx]!=1u) all_wall=0u;
            }
            s_work[(uint8_t)(ly*4u+0u)]=outm;
            s_work[(uint8_t)(ly*4u+1u)]=0u;
            s_work[(uint8_t)(ly*4u+2u)]=wallm;
            s_work[(uint8_t)(ly*4u+3u)]=0u;
        }

        /* Coarse materialization was forced for this column. Pure ceiling or
         * pure wall cells are already correct and need no dynamic tile. */
        if(all_out || all_wall) continue;

        index=bc_pattern_index(&flip);
        if(index==0xffu) return 0u;
        {
            uint16_t word=(uint16_t)(g_tspf_boundary_pattern_base+index);
            if(flip) word|=TSP_ATTR_FLIPX;
            if(!bc_add_patch(row,col,word)) return 0u;
        }
    }
    return 1u;
}

void tsp_polar_boundary_reset(void) BANKED
{
    uint8_t i;
    g_tspf_boundary_patterns_pending=0u;
    g_tspf_boundary_pattern_count=0u;
    g_tspf_boundary_last_patterns=0u;
    g_tspf_boundary_last_patches=0u;
    g_tspf_boundary_skip_reason=0u;
    g_tspf_boundary_last_crowded=0u;
    g_tspf_boundary_last_local_fallbacks=0u;
    s_prev_bank=0xffu;
    s_target_bank=0xffu;
    s_prepared=0u;
    bc_clear3(s_exact_cols);
    bc_clear3(s_prev_cols);
    s_bank_used[0]=s_bank_used[1]=0u;
    s_bank_released[0]=s_bank_released[1]=0u;
    g_tspf_boundary_any_dirty=0u;
    for(i=0u;i<TSP_COLS;++i) g_tspf_boundary_dirty_by_col[i]=0u;
}

void tsp_polar_boundary_prepare(const TSPState *s) BANKED
{
    uint8_t i,n,count=0u;
    uint16_t yawq;
    uint8_t target;

    s_prepared=0u;
    s_patch_count=0u;
    s_target_bank=0xffu;
    /* Do NOT clear pattern_count while a previous render is still waiting for
     * its safe-VBlank pattern upload. That count belongs to the queued staging
     * buffer until tsp_polar_boundary_upload() consumes it. */
    g_tspf_boundary_last_patterns=0u;
    g_tspf_boundary_last_patches=0u;
    g_tspf_boundary_skip_reason=0u;
    g_tspf_boundary_last_crowded=0u;
    g_tspf_boundary_last_local_fallbacks=0u;
    g_tspf_seam_desc_count=0u;
    bc_clear3(s_exact_cols);

    /* Only PREVIOUS dynamic columns require forced coarse restoration. A new
     * exact-X tile sits on top of the ordinary retained coarse answer: rows
     * where the two owners look identical are deliberately left coarse, while
     * every row that depends on the sub-tile handoff is replaced below. Thus a
     * current-only boundary does not justify throwing away a valid retained
     * substrate. The one-byte OR gate keeps even this previous-column check out
     * of the overwhelmingly common no-boundary path. */
    g_tspf_boundary_any_dirty=0u;
    for(i=0u;i<TSP_COLS;++i){
        g_tspf_boundary_dirty_by_col[i]=bc_col_marked(s_prev_cols,i)?1u:0u;
        if(g_tspf_boundary_dirty_by_col[i]) g_tspf_boundary_any_dirty=1u;
    }

#if defined(TSPF_OPTIMIZED_MAP)
    if(s->z_q4!=TSP_OPT_EYE_Q4){
        g_tspf_boundary_skip_reason=1u;
        return;
    }
#endif
    if(g_tspf_appearance_mode!=0u){
        g_tspf_boundary_skip_reason=2u;
        return;
    }

    n=g_e1env_program[0];
    if(!n || n>31u) return;
    yawq=(uint16_t)s->yaw<<4;

    /* Record the visible sub-tile ownership transitions.  Exact tile-edge
     * transitions need no composite: the ordinary c0/c1 handoff is already at
     * the right raster X. */
    for(i=0u;i<n;++i){
        uint8_t ni=(uint8_t)(i+1u<n?i+1u:0u);
        uint8_t left=g_e1env_program[(uint8_t)(2u+(uint8_t)(i<<1))];
        uint8_t right=g_e1env_program[(uint8_t)(2u+(uint8_t)(ni<<1))];
        uint8_t vid=g_e1env_program[(uint8_t)(1u+(uint8_t)(ni<<1))];
        int16_t rel,x;
        uint8_t code;

        if(left==0xffu || right==0xffu) continue;
        /* Packed owner bytes also carry endpoint/connected flags.  A flag
         * change is NOT an ownership handoff; only a surface-ID change can
         * create a mixed exact-X tile. */
        if((left&31u)==(right&31u)) continue;
        if(vid>=32u) continue;
        if(!(g_corner_bearing_valid[vid>>3]&(uint8_t)(1u<<(vid&7u)))) continue;
        rel=bc_rel(g_corner_bearing_q12[vid],yawq);
        if(rel<=-512 || rel>=512) continue;
        code=g_e1env_center_col_lut[(uint16_t)(rel+512)];
        x=(int16_t)(((uint16_t)(code&31u)<<3)+(int16_t)((int8_t)(code>>5)-4));
        if(x<0 || x>=160 || (((uint8_t)x&7u)==0u)) continue;
        if(count>=32u){ g_tspf_boundary_skip_reason=3u; return; }
        g_tspf_seam_x[count]=(uint8_t)x;
        g_tspf_seam_vid[count]=left;
        g_tspf_seam_vertex_half[count]=right;
#if TSPF_PROFILE_HOOKS
        g_tspf_boundary_vid[count]=vid;
#endif
        ++count;
    }
    g_tspf_seam_desc_count=count;
    if(!count) return;

    /* Insertion sort: at most a few visible handoffs, and doing it here means
     * the tile builder consumes one left->right edge record at a time. */
    for(i=1u;i<count;++i){
        uint8_t x=g_tspf_seam_x[i],l=g_tspf_seam_vid[i],
                r=g_tspf_seam_vertex_half[i],j=i;
        while(j && g_tspf_seam_x[(uint8_t)(j-1u)]>x){
            g_tspf_seam_x[j]=g_tspf_seam_x[(uint8_t)(j-1u)];
            g_tspf_seam_vid[j]=g_tspf_seam_vid[(uint8_t)(j-1u)];
            g_tspf_seam_vertex_half[j]=g_tspf_seam_vertex_half[(uint8_t)(j-1u)];
            --j;
        }
        g_tspf_seam_x[j]=x;
        g_tspf_seam_vid[j]=l;
        g_tspf_seam_vertex_half[j]=r;
    }

    /* Never overwrite a staging buffer that has not reached VRAM yet.
     * Current/previous boundary columns are already marked dirty above, so the
     * normal coarse path safely removes stale composites for this update. */
    if(g_tspf_boundary_patterns_pending){
        g_tspf_boundary_skip_reason=4u;
        return;
    }
    g_tspf_boundary_pattern_count=0u;
    target=bc_choose_bank();
    if(target==0xffu){
        g_tspf_boundary_skip_reason=5u;
        return;
    }
    s_target_bank=target;
    g_tspf_boundary_pattern_base=(uint16_t)(target?TSP_BC_BASE1:TSP_BC_BASE0);

    i=0u;
    while(i<count){
        uint8_t first=i,last=i,col=(uint8_t)(g_tspf_seam_x[i]>>3);
        while((uint8_t)(last+1u)<count &&
              (uint8_t)(g_tspf_seam_x[(uint8_t)(last+1u)]>>3)==col)
            ++last;

        /* First shipping-shaped rung: exact 0..2 ownership transitions per
         * hardware tile.  The census says that class dominates; crowded 3+
         * transition tiles stay on the known-correct coarse fallback instead
         * of consuming a disproportionate share of the 18-slot dynamic bank.
         * Other, ordinary boundary tiles in the same frame remain exact. */
        if((uint8_t)(last-first)<2u){
            uint8_t pattern_before=g_tspf_boundary_pattern_count;
            uint8_t patch_before=s_patch_count;

            /* Capacity pressure is TILE-local, not frame-fatal. Roll this tile
             * back to the coarse substrate and keep exact composites already
             * built for other columns. Counts are the ownership boundary:
             * stale staging bytes/hash entries beyond pattern_count are
             * unreachable and the next allocation overwrites them. */
            if(!bc_build_tile(first,last,col,s)){
                g_tspf_boundary_pattern_count=pattern_before;
                s_patch_count=patch_before;
                ++g_tspf_boundary_last_local_fallbacks;
            }else if(s_patch_count!=patch_before){
                bc_mark_col(s_exact_cols,col);
            }
        }else{
            ++g_tspf_boundary_last_crowded;
        }
        i=(uint8_t)(last+1u);
    }

    if(!s_patch_count || !g_tspf_boundary_pattern_count) return;
    s_prepared=1u;
    g_tspf_boundary_last_patterns=g_tspf_boundary_pattern_count;
    g_tspf_boundary_last_patches=s_patch_count;
    /* Pattern upload is always published before any dirty name-table row, so
     * a name-table reference can never race an uninitialized dynamic tile. */
    g_tspf_boundary_patterns_pending=1u;
}

void tsp_polar_boundary_apply(void) BANKED
{
    uint8_t i;

    /* Profile builds keep the event vectors alive until the loop-boundary
     * sampler reads them. Zero-hook playables keep the old diagnostics
     * quiescent; the next prepare resets the count either way. */
#if !TSPF_PROFILE_HOOKS
    g_tspf_seam_desc_count=0u;
#endif

    if(s_prepared){
        for(i=0u;i<s_patch_count;++i){
            uint8_t pos=s_patch_pos[i];
            uint8_t row=(uint8_t)(pos/20u);
            uint8_t col=(uint8_t)(pos-(uint8_t)(row*20u));
            uint8_t brow=(uint8_t)(17u-row);
            uint16_t word=s_patch_word[i];
            uint16_t idx=(uint16_t)pos;
            uint16_t bidx=(uint16_t)((uint16_t)brow*20u+col);
            uint16_t bword=(uint16_t)(word|TSP_ATTR_FLIPY|TSP_ATTR_PALETTE);

            if(g_map[idx]!=word){
                g_map[idx]=word;
                bc_dirty_row(row,col);
            }
            if(g_map[bidx]!=bword){
                g_map[bidx]=bword;
                bc_dirty_row(brow,col);
            }
            bc_own_cell(row,col);
            bc_own_cell(brow,col);
        }

        if(s_prev_bank!=0xffu && s_prev_bank!=s_target_bank){
            s_bank_released[s_prev_bank]=1u;
            s_bank_release_tick[s_prev_bank]=g_tspf_boundary_publish_tick;
        }
        s_bank_used[s_target_bank]=1u;
        s_bank_released[s_target_bank]=0u;
        s_prev_bank=s_target_bank;
        /* Do not make a crowded/coarse-only candidate pay restoration next
         * frame. Only cells that actually reference this dynamic bank need the
         * forced coarse erase before a new exact overlay can be installed. */
        s_prev_cols[0]=s_exact_cols[0];
        s_prev_cols[1]=s_exact_cols[1];
        s_prev_cols[2]=s_exact_cols[2];
    }else{
        /* The forced coarse raster has removed every previous dynamic cell.
         * Release its pattern bank only now, then forget the old boundary set. */
        if(s_prev_bank!=0xffu){
            s_bank_released[s_prev_bank]=1u;
            s_bank_release_tick[s_prev_bank]=g_tspf_boundary_publish_tick;
        }
        s_prev_bank=0xffu;
        bc_clear3(s_prev_cols);
    }

    s_prepared=0u;
}

void tsp_polar_boundary_upload(void) BANKED
{
    uint8_t slot,y;
    uint8_t tile[32];

    if(!g_tspf_boundary_patterns_pending) return;

    for(slot=0u;slot<g_tspf_boundary_pattern_count;++slot){
        const uint8_t *p=&g_tspf_boundary_pattern_data[(uint16_t)slot<<4];
        for(y=0u;y<8u;++y){
            tile[(uint8_t)(y*4u)]=p[(uint8_t)(y+y)];       /* color bit 0 */
            tile[(uint8_t)(y*4u+1u)]=0u;
            tile[(uint8_t)(y*4u+2u)]=p[(uint8_t)(y+y+1u)];/* color bit 2 */
            tile[(uint8_t)(y*4u+3u)]=0u;
        }
        set_bkg_4bpp_data((uint16_t)(g_tspf_boundary_pattern_base+slot),1u,tile);
    }
    g_tspf_boundary_patterns_pending=0u;
}

#endif /* __SDCC && TSPF_BOUNDARY_COMPOSITE */
