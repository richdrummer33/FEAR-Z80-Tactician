/*
 * Direct exact-X mixed-cell materializer.
 *
 * Baked front-envelope transitions are recorded BEFORE coarse materialization.
 * This pass constructs the final mixed 8x8 cells and marks those rows so the
 * coarse Z80 kernel never writes them. It is intentionally not a seam overlay.
 */
#if defined(__SDCC)
#if defined(TSPF_DIRECT_MIXED) && TSPF_DIRECT_MIXED
/* Bank 254 is intentionally reserved by the exact-envelope workflow below.
 * Feature-off builds stay autobanked so ordinary small-ROM targets do not
 * inherit an impossible high-bank requirement from this experiment. */
#pragma bank 254
#endif
#include <gbdk/platform.h>
#endif

#include <stdint.h>
#include "tilesector_polar.h"

#ifdef __SDCC

/* Always-exported assembly bridge. Ordinary builds leave it zero. */
uint8_t g_tspf_mixed_skip[TSP_COLS*3u];
/* Previous-frame dynamic cells force their coarse column through the real
 * materializer once. This restores stale dynamic tile IDs even when retained
 * geometry is otherwise bit-identical. */
uint8_t g_tspf_mixed_force_col[TSP_COLS];
uint8_t g_tspf_mixed_force_any;
/* Per authored surface: bit0 suppress snapped LEFT endpoint, bit1 RIGHT.
 * Keying this by surface endpoint rather than coarse X is crucial: crowded
 * cells can also contain a legitimate physical corner exactly on the nearby
 * tile edge, which must not be erased by another sub-column transition. */
uint8_t g_tspf_mixed_border_clear_sid[32];
uint8_t g_tspf_mixed_any;
uint8_t g_tspf_mixed_event_count;
uint8_t g_tspf_mixed_event_overflow;
uint8_t g_tspf_mixed_event_x[32];
uint8_t g_tspf_mixed_event_left[32];
uint8_t g_tspf_mixed_event_right[32];
uint8_t g_tspf_mixed_retire_bank0[TSP_ROWS];
uint8_t g_tspf_mixed_retire_bank1[TSP_ROWS];
/* Current-generation publication fence. The row uploader clears one byte only
 * after the authoritative row has actually reached the VDP. */
uint8_t g_tspf_mixed_publish_rows[TSP_ROWS];
volatile uint8_t g_tspf_mixed_patterns_pending;

#if TSPF_DIRECT_MIXED

#include "e1env_depth_edges_bank.h"
#include "e1env_plane_meta.h"

#define TSP_MIX_SLOTS 18u
#define TSP_MIX_PATCH_MAX 32u
#define TSP_MIX_BASE0 412u
#define TSP_MIX_BASE1 430u

extern uint16_t g_map[TSP_MAP_CELLS];
extern uint8_t g_polar_nt_cov_cur[TSP_COLS*3u];
extern uint8_t g_polar_nt_row_min[TSP_ROWS];
extern uint8_t g_polar_nt_row_max[TSP_ROWS];
extern volatile uint8_t g_tspf_appearance_mode;

uint8_t g_tspf_mixed_pattern_data[TSP_MIX_SLOTS*16u];
uint16_t g_tspf_mixed_pattern_base;
uint8_t g_tspf_mixed_pattern_count;
uint8_t g_tspf_mixed_upload_index;

volatile uint8_t g_tspf_mixed_last_patterns;
volatile uint8_t g_tspf_mixed_last_patches;
volatile uint8_t g_tspf_mixed_skip_reason;
volatile uint8_t g_tspf_mixed_local_fallbacks;
volatile uint8_t g_tspf_mixed_chain_fallbacks;
volatile uint8_t g_tspf_mixed_unsupported_tiles;
#if TSPF_PROFILE_HOOKS
volatile uint8_t g_tspf_mixed_connected_elided;
volatile uint8_t g_tspf_mixed_collinear_collapsed;
volatile uint8_t g_tspf_mixed_silhouette_lines;
#endif

static uint8_t s_pattern_hash[TSP_MIX_SLOTS];
static uint8_t s_patch_pos[TSP_MIX_PATCH_MAX];
static uint16_t s_patch_word[TSP_MIX_PATCH_MAX];
static uint8_t s_patch_count;
static uint8_t s_work[16];
static uint8_t s_owner[8];
static int8_t s_top[8];
static uint8_t s_hit[8];
static uint8_t s_line_start[8];
/* Previous dynamic cells, not merely rows. Retirement re-dirties these exact
 * positions after the old bank is fenced, so an in-render VBlank can never
 * falsely retire a bank after uploading some unrelated part of the row. */
static uint8_t s_prev_pos[TSP_MIX_PATCH_MAX];
static uint8_t s_prev_count;
static uint8_t s_bank_used[2];
static uint8_t s_prev_bank=0xffu;
static uint8_t s_target_bank=0xffu;
static uint8_t s_prepared;
static uint8_t s_hold_previous;

/* Dynamic mixed tiles are not constrained by the static p24 vocabulary.
 * Preserve the measured full steep-edge range (max |dy|=36) here; it costs
 * only ROM bytes, not permanent VRAM pattern IDs. */
static const uint8_t k_step[37][8]={
    {0,0,0,0,0,0,0,0},{0,0,0,0,1,1,1,1},
    {0,0,1,1,1,1,2,2},{0,0,1,1,2,2,3,3},
    {0,1,1,2,2,3,3,4},{0,1,1,2,3,4,4,5},
    {0,1,2,3,3,4,5,6},{0,1,2,3,4,5,6,7},
    {0,1,2,3,5,6,7,8},{0,1,3,4,5,6,8,9},
    {0,1,3,4,6,7,9,10},{0,2,3,5,6,8,9,11},
    {0,2,3,5,7,9,10,12},{0,2,4,6,7,9,11,13},
    {0,2,4,6,8,10,12,14},{0,2,4,6,9,11,13,15},
    {0,2,5,7,9,11,14,16},{0,2,5,7,10,12,15,17},
    {0,3,5,8,10,13,15,18},{0,3,5,8,11,14,16,19},
    {0,3,6,9,11,14,17,20},{0,3,6,9,12,15,18,21},
    {0,3,6,9,13,16,19,22},{0,3,7,10,13,16,20,23},
    {0,3,7,10,14,17,21,24},{0,4,7,11,14,18,21,25},
    {0,4,7,11,15,19,22,26},{0,4,8,12,15,19,23,27},
    {0,4,8,12,16,20,24,28},{0,4,8,12,17,21,25,29},
    {0,4,9,13,17,21,26,30},{0,4,9,13,18,22,27,31},
    {0,5,9,14,18,23,27,32},{0,5,9,14,19,24,28,33},
    {0,5,10,15,19,24,29,34},{0,5,10,15,20,25,30,35},
    {0,5,10,15,21,26,31,36}
};
static const uint8_t k_rev4[16]={
    0x0,0x8,0x4,0xc,0x2,0xa,0x6,0xe,0x1,0x9,0x5,0xd,0x3,0xb,0x7,0xf
};

static uint8_t rev8(uint8_t x){
    return (uint8_t)((k_rev4[x&15u]<<4)|k_rev4[x>>4]);
}
static void clear_skip_bits(void){
    uint8_t i;
    for(i=0u;i<TSP_COLS*3u;++i)g_tspf_mixed_skip[i]=0u;
    g_tspf_mixed_any=0u;
}
static void clear_skip(void){
    uint8_t i;
    clear_skip_bits();
    for(i=0u;i<32u;++i)g_tspf_mixed_border_clear_sid[i]=0u;
}
static uint8_t publication_pending(void){
    uint8_t i;
    if(g_tspf_mixed_patterns_pending)return 1u;
    for(i=0u;i<TSP_ROWS;++i)if(g_tspf_mixed_publish_rows[i])return 1u;
    return 0u;
}
static void prepare_restore_cols(void){
    uint8_t i;
    g_tspf_mixed_force_any=0u;
    for(i=0u;i<TSP_COLS;++i)g_tspf_mixed_force_col[i]=0u;
    for(i=0u;i<s_prev_count;++i){
        uint8_t pos=s_prev_pos[i];
        uint8_t row=(uint8_t)(pos/20u);
        uint8_t col=(uint8_t)(pos-(uint8_t)(row*20u));
        g_tspf_mixed_force_col[col]=1u;
        g_tspf_mixed_force_any=1u;
    }
}
static uint8_t row_is_direct(uint8_t row,uint8_t col){
    return (uint8_t)(g_tspf_mixed_skip[(uint8_t)(col+col+col+(row>>3))] &
                     (uint8_t)(1u<<(row&7u)));
}
static void refine_restore_cols(void){
    uint8_t i;
    /* If every previous dynamic word in a column is itself replaced by a new
     * direct word this frame, there is no stale dynamic ID for coarse to erase.
     * Avoid throwing the rest of that column off the retained fast path. */
    g_tspf_mixed_force_any=0u;
    for(i=0u;i<TSP_COLS;++i)g_tspf_mixed_force_col[i]=0u;
    for(i=0u;i<s_prev_count;++i){
        uint8_t pos=s_prev_pos[i];
        uint8_t row=(uint8_t)(pos/20u);
        uint8_t col=(uint8_t)(pos-(uint8_t)(row*20u));
        if(!row_is_direct(row,col)){
            g_tspf_mixed_force_col[col]=1u;
            g_tspf_mixed_force_any=1u;
        }
    }
}
static void mark_skip(uint8_t row,uint8_t col){
    g_tspf_mixed_skip[(uint8_t)(col+col+col+(row>>3))]|=
        (uint8_t)(1u<<(row&7u));
    g_tspf_mixed_any=1u;
}
static void hold_previous_cells(void){
    uint8_t i;
    clear_skip_bits();
    g_tspf_mixed_force_any=0u;
    for(i=0u;i<TSP_COLS;++i)g_tspf_mixed_force_col[i]=0u;
    for(i=0u;i<s_prev_count;++i){
        uint8_t pos=s_prev_pos[i];
        uint8_t row=(uint8_t)(pos/20u);
        uint8_t col=(uint8_t)(pos-(uint8_t)(row*20u));
        mark_skip(row,col);
        mark_skip((uint8_t)(17u-row),col);
    }
}
static void own_cell(uint8_t row,uint8_t col){
    g_polar_nt_cov_cur[(uint8_t)(col+col+col+(row>>3))]|=
        (uint8_t)(1u<<(row&7u));
}
static void dirty_cell(uint8_t row,uint8_t col){
    if(g_polar_nt_row_min[row]==0xffu || col<g_polar_nt_row_min[row])
        g_polar_nt_row_min[row]=col;
    if(col>g_polar_nt_row_max[row])g_polar_nt_row_max[row]=col;
}
static uint8_t bank_retired(uint8_t bank){
    uint8_t i;
    uint8_t *p=bank?g_tspf_mixed_retire_bank1:g_tspf_mixed_retire_bank0;
    for(i=0u;i<TSP_ROWS;++i)if(p[i])return 0u;
    return 1u;
}
static void retire_previous(void){
    uint8_t i;
    uint8_t *p;
    if(s_prev_bank==0xffu)return;
    p=s_prev_bank?g_tspf_mixed_retire_bank1:g_tspf_mixed_retire_bank0;
    for(i=0u;i<s_prev_count;++i){
        uint8_t pos=s_prev_pos[i];
        uint8_t row=(uint8_t)(pos/20u);
        uint8_t col=(uint8_t)(pos-(uint8_t)(row*20u));
        uint8_t brow=(uint8_t)(17u-row);
        p[row]=1u;p[brow]=1u;
        /* This happens after all cooperative in-render uploads. Force one
         * post-fence publication that covers every former dynamic cell. */
        dirty_cell(row,col);
        dirty_cell(brow,col);
    }
}
static uint8_t choose_bank(void){
    uint8_t b;
    for(b=0u;b<2u;++b){
        if(b==s_prev_bank)continue;
        if(!s_bank_used[b] || bank_retired(b))return b;
    }
    return 0xffu;
}
static uint8_t inv_q6(int16_t q){
    int16_t v=(int16_t)((q+32)>>6);
    if(v<0)return 0u;
    if(v>255)return 255u;
    return (uint8_t)v;
}
static void fill_top(uint8_t owner,uint8_t col,const TSPState *s,
                     uint8_t x0,uint8_t x1){
    uint8_t x,sid,axis,il,ir,mag;
    int16_t dq4,tl,tr,d;
    sid=(uint8_t)(owner&31u);
    axis=k_e1env_depth_axis[sid];
    dq4=(int16_t)(k_e1env_plane_c[sid]-(axis?s->y_q4:s->x_q4));
    if(axis==0u)e1env_depth_edges_0(s->yaw,col,col,dq4);
    else e1env_depth_edges_1(s->yaw,col,col,dq4);
    il=inv_q6(g_e1env_depth_iq);
    ir=g_e1env_depth_end_inv;
    tl=(int16_t)(71-(int16_t)(il>>1));
    tr=(int16_t)(71-(int16_t)(ir>>1));
    d=(int16_t)(tr-tl);
    mag=(uint8_t)(d<0?-d:d);
    if(mag>36u)mag=36u;
    if(d>=0){
        for(x=x0;x<=x1;++x)s_top[x]=(int8_t)(tl+(int16_t)k_step[mag][x]);
    }else{
        for(x=x0;x<=x1;++x)
            s_top[x]=(int8_t)(tr+(int16_t)k_step[mag][(uint8_t)(7u-x)]);
    }
}
static uint8_t pattern_index(uint8_t *flip_out){
    uint8_t i,j,h=0x5du,flip=0u,count=g_tspf_mixed_pattern_count;
    for(i=0u;i<16u;++i){
        uint8_t r=rev8(s_work[i]);
        if(r==s_work[i])continue;
        flip=(uint8_t)(r<s_work[i]);
        break;
    }
    if(flip)for(i=0u;i<16u;++i)s_work[i]=rev8(s_work[i]);
    for(i=0u;i<16u;++i)h=(uint8_t)((h<<1)|(h>>7))^s_work[i];
    for(j=0u;j<count;++j){
        uint8_t *p;
        if(s_pattern_hash[j]!=h)continue;
        p=&g_tspf_mixed_pattern_data[(uint16_t)j<<4];
        for(i=0u;i<16u && p[i]==s_work[i];++i){}
        if(i==16u){*flip_out=flip;return j;}
    }
    if(count>=TSP_MIX_SLOTS)return 0xffu;
    {
        uint8_t *p=&g_tspf_mixed_pattern_data[(uint16_t)count<<4];
        for(i=0u;i<16u;++i)p[i]=s_work[i];
    }
    s_pattern_hash[count]=h;
    g_tspf_mixed_pattern_count=(uint8_t)(count+1u);
    *flip_out=flip;
    return count;
}
static uint8_t add_patch(uint8_t row,uint8_t col,uint16_t word){
    uint8_t n=s_patch_count;
    if(n>=TSP_MIX_PATCH_MAX)return 0u;
    s_patch_pos[n]=(uint8_t)(row*20u+col);
    s_patch_word[n]=word;
    s_patch_count=(uint8_t)(n+1u);
    return 1u;
}

static uint8_t same_owner(uint8_t a,uint8_t b){
    if(a==0xffu || b==0xffu)return (uint8_t)(a==b);
    return (uint8_t)((a&31u)==(b&31u));
}
static uint8_t same_depth_plane(uint8_t a,uint8_t b){
    uint8_t sa,sb;
    if(a==0xffu || b==0xffu)return 0u;
    sa=(uint8_t)(a&31u);sb=(uint8_t)(b&31u);
    return (uint8_t)(k_e1env_depth_axis[sa]==k_e1env_depth_axis[sb] &&
                     k_e1env_plane_c[sa]==k_e1env_plane_c[sb]);
}

/* 1=exact tile built, 2=unsupported wall/void first rung,
 * 3=inconsistent quantized event chain, 0=capacity. */
static uint8_t build_tile(uint8_t first,uint8_t last,uint8_t col,const TSPState *s){
    uint8_t line_mask=0u,lx,e,row,ly;

    /* Multiple visibility transitions can quantize into one 8px tile. Never
     * invent a pixel ownership order if their streamed left->right chain does
     * not join. This is especially important when two sub-pixel spans collapse
     * onto the same integer X and the focus-outward walk supplied equal-X
     * events in the opposite append order. Coarse is the safe local fallback;
     * a later census can justify a dedicated equal-X chain reorder if needed. */
    for(e=first;e<last;++e)
        if(!same_owner(g_tspf_mixed_event_right[e],
                       g_tspf_mixed_event_left[(uint8_t)(e+1u)]))
            return 3u;

    for(lx=0u;lx<8u;++lx)s_owner[lx]=g_tspf_mixed_event_left[first];
    for(e=first;e<=last;++e){
        uint8_t split=(uint8_t)(g_tspf_mixed_event_x[e]&7u);
        uint8_t lo=g_tspf_mixed_event_left[e],ro=g_tspf_mixed_event_right[e];
        uint8_t physical=(uint8_t)((lo!=0xffu && (lo&0x40u)) ||
                                   (ro!=0xffu && (ro&0x20u)));
        /* Bit7 means the left wall's right physical endpoint is shared by the
         * immediately-visible wall on the right. That is an INTERNAL corner
         * of a continuous visible chain, not a silhouette/end-of-chain edge.
         *
         * Keep the exact ownership/top-edge handoff at true X, but omit the
         * black vertical crease. Endpoints of the visible chain have bit7 clear
         * and therefore still receive a silhouette line. */
#if TSPF_MIX_ELIDE_CONNECTED
        uint8_t connected=(uint8_t)(lo!=0xffu && ro!=0xffu &&
                                    (lo&0x80u) && (ro&0x20u));
        uint8_t collinear=(uint8_t)(connected && same_depth_plane(lo,ro));
#else
        uint8_t connected=0u,collinear=0u;
#endif
        if(!split)continue;
        /* A connected collinear segment boundary has no visible geometry at
         * all once its vertical crease is intentionally elided. Keep the left
         * owner across that sub-range: its plane is bit-identical to the right
         * owner's plane, so this also avoids a redundant fill_top/depth-bank
         * evaluation and can make the entire mixed tile disappear. */
        if(!collinear)
            for(lx=split;lx<8u;++lx)s_owner[lx]=ro;
#if TSPF_PROFILE_HOOKS
        else ++g_tspf_mixed_collinear_collapsed;
#endif
        if(physical && !connected){
            line_mask|=(uint8_t)(1u<<split);
#if TSPF_PROFILE_HOOKS
            ++g_tspf_mixed_silhouette_lines;
#endif
        }
#if TSPF_PROFILE_HOOKS
        else if(physical && connected) ++g_tspf_mixed_connected_elided;
#endif
    }

    /* Wall/void needs a horizon-aware asymmetric bottom half. Keep that rare
     * case on the normal renderer until that direct kernel is added. */
    for(lx=0u;lx<8u;++lx)if(s_owner[lx]==0xffu)return 2u;

    lx=0u;
    while(lx<8u){
        uint8_t x1=lx;
        while(x1<7u && (s_owner[(uint8_t)(x1+1u)]&31u)==(s_owner[lx]&31u))++x1;
        fill_top(s_owner[lx],col,s,lx,x1);
        lx=(uint8_t)(x1+1u);
    }

    for(row=0u;row<9u;++row){
        uint8_t outm=0u,wallm=0u,active=0u,all_out=1u,all_wall=1u,flip,index;
        int16_t y0=(int16_t)((uint16_t)row<<3);
        for(ly=0u;ly<8u;++ly)s_hit[ly]=0u;
        if(line_mask)for(ly=0u;ly<8u;++ly)s_line_start[ly]=0u;
        for(lx=0u;lx<8u;++lx){
            uint8_t bit=(uint8_t)(0x80u>>lx);
            int16_t ty=(int16_t)s_top[lx];
            if(ty>y0)outm|=bit;
            else if(ty<y0)wallm|=bit;
            if(ty>=y0 && ty<(int16_t)(y0+8))s_hit[(uint8_t)(ty-y0)]|=bit;
        }
        if(line_mask)for(e=first;e<=last;++e){
            uint8_t split=(uint8_t)(g_tspf_mixed_event_x[e]&7u);
            if(split && (line_mask&(uint8_t)(1u<<split))){
                int16_t a=(int16_t)s_top[(uint8_t)(split-1u)];
                int16_t b=(int16_t)s_top[split];
                /* The vertical crease owns the boundary pixel itself.
                 * Starting one row below min(topA,topB) leaves a one-pixel
                 * pinhole whenever the nearer face's horizontal edge ends at
                 * split-1. Overlap with an equal-height top edge is harmless. */
                int16_t sy=(int16_t)(a<b?a:b);
                uint8_t bit=(uint8_t)(0x80u>>split);
                if(sy<=y0)active|=bit;
                else if(sy<(int16_t)(y0+8))s_line_start[(uint8_t)(sy-y0)]|=bit;
            }
        }
        for(ly=0u;ly<8u;++ly){
            uint8_t o,w,kill;
            active|=s_line_start[ly];
            kill=(uint8_t)~active;
            o=(uint8_t)(outm&kill);
            w=(uint8_t)(wallm&kill);
            s_work[(uint8_t)(ly+ly)]=o;
            s_work[(uint8_t)(ly+ly+1u)]=w;
            if(o!=0xffu)all_out=0u;
            if(w!=0xffu)all_wall=0u;
            wallm|=s_hit[ly];
            if(ly<7u)outm&=(uint8_t)~s_hit[(uint8_t)(ly+1u)];
        }
        if(all_out || all_wall)continue;
        index=pattern_index(&flip);
        if(index==0xffu)return 0u;
        {
            uint16_t word=(uint16_t)(g_tspf_mixed_pattern_base+index);
            if(flip)word|=TSP_ATTR_FLIPX;
            if(!add_patch(row,col,word))return 0u;
        }
    }
    return 1u;
}

void tsp_polar_mixed_reset(void) BANKED{
    uint8_t i;
    g_tspf_mixed_patterns_pending=0u;
    g_tspf_mixed_pattern_count=0u;
    g_tspf_mixed_upload_index=0u;
    g_tspf_mixed_event_count=0u;
    g_tspf_mixed_event_overflow=0u;
    g_tspf_mixed_last_patterns=0u;
    g_tspf_mixed_last_patches=0u;
    g_tspf_mixed_skip_reason=0u;
    g_tspf_mixed_local_fallbacks=0u;
    g_tspf_mixed_chain_fallbacks=0u;
    g_tspf_mixed_unsupported_tiles=0u;
#if TSPF_PROFILE_HOOKS
    g_tspf_mixed_connected_elided=0u;
    g_tspf_mixed_collinear_collapsed=0u;
    g_tspf_mixed_silhouette_lines=0u;
#endif
    s_prev_bank=0xffu;s_target_bank=0xffu;s_prepared=0u;s_prev_count=0u;
    s_hold_previous=0u;
    s_bank_used[0]=s_bank_used[1]=0u;
    clear_skip();
    prepare_restore_cols();
    for(i=0u;i<TSP_ROWS;++i){
        g_tspf_mixed_retire_bank0[i]=0u;
        g_tspf_mixed_retire_bank1[i]=0u;
        g_tspf_mixed_publish_rows[i]=0u;
    }
}

void tsp_polar_mixed_begin_frame(void) BANKED{
    /* Do not fence the visible bank yet: cooperative VBlank publication can
     * happen inside the coarse renderer. Retirement is armed in apply(), after
     * those yields, and every old dynamic position is explicitly re-dirtied. */
    g_tspf_mixed_event_count=0u;
    g_tspf_mixed_event_overflow=0u;
    g_tspf_mixed_last_patterns=0u;
    g_tspf_mixed_last_patches=0u;
    g_tspf_mixed_skip_reason=0u;
    g_tspf_mixed_local_fallbacks=0u;
    g_tspf_mixed_chain_fallbacks=0u;
    g_tspf_mixed_unsupported_tiles=0u;
#if TSPF_PROFILE_HOOKS
    g_tspf_mixed_connected_elided=0u;
    g_tspf_mixed_collinear_collapsed=0u;
    g_tspf_mixed_silhouette_lines=0u;
#endif
    s_patch_count=0u;s_target_bank=0xffu;s_prepared=0u;
    s_hold_previous=publication_pending();
    if(s_hold_previous){
        /* A staged generation is not disposable merely because another CPU
         * update began. Preserve its exact cells (and previous endpoint-border
         * suppression) until the VDP has both its patterns and row references.
         * This prevents a fast logical update from erasing a direct frame
         * before that frame ever becomes visible. */
        hold_previous_cells();
    }else{
        clear_skip();
        /* Publication finished. Now and only now may stale dynamic IDs be
         * forced back through the coarse raster or replaced by a new direct
         * generation. */
        prepare_restore_cols();
    }
}

void tsp_polar_mixed_prepare(const TSPState *s) BANKED{
    uint8_t i,count=g_tspf_mixed_event_count,target;
#if defined(TSPF_OPTIMIZED_MAP)
    if(s->z_q4!=TSP_OPT_EYE_Q4){g_tspf_mixed_skip_reason=1u;return;}
#endif
    if(g_tspf_appearance_mode!=0u){g_tspf_mixed_skip_reason=2u;return;}
    if(g_tspf_mixed_event_overflow){g_tspf_mixed_skip_reason=3u;return;}
    if(s_hold_previous){g_tspf_mixed_skip_reason=6u;return;}
    if(!count)return;
    /* Defensive: publication_pending() should have made this a held frame. */
    if(g_tspf_mixed_patterns_pending){g_tspf_mixed_skip_reason=4u;return;}

    for(i=1u;i<count;++i){
        uint8_t x=g_tspf_mixed_event_x[i],l=g_tspf_mixed_event_left[i];
        uint8_t r=g_tspf_mixed_event_right[i],j=i;
        while(j && g_tspf_mixed_event_x[(uint8_t)(j-1u)]>x){
            g_tspf_mixed_event_x[j]=g_tspf_mixed_event_x[(uint8_t)(j-1u)];
            g_tspf_mixed_event_left[j]=g_tspf_mixed_event_left[(uint8_t)(j-1u)];
            g_tspf_mixed_event_right[j]=g_tspf_mixed_event_right[(uint8_t)(j-1u)];
            --j;
        }
        g_tspf_mixed_event_x[j]=x;g_tspf_mixed_event_left[j]=l;
        g_tspf_mixed_event_right[j]=r;
    }

    target=choose_bank();
    if(target==0xffu){g_tspf_mixed_skip_reason=5u;return;}
    s_target_bank=target;
    g_tspf_mixed_pattern_base=(uint16_t)(target?TSP_MIX_BASE1:TSP_MIX_BASE0);
    g_tspf_mixed_pattern_count=0u;

    i=0u;
    while(i<count){
        uint8_t first=i,last=i,col=(uint8_t)(g_tspf_mixed_event_x[i]>>3);
        uint8_t p0=s_patch_count,n0=g_tspf_mixed_pattern_count,result,k;
        while((uint8_t)(last+1u)<count &&
              (uint8_t)(g_tspf_mixed_event_x[(uint8_t)(last+1u)]>>3)==col)++last;

        result=build_tile(first,last,col,s);
        if(result==1u){
            uint8_t e;
            for(k=p0;k<s_patch_count;++k){
                uint8_t pos=s_patch_pos[k];
                uint8_t row=(uint8_t)(pos/20u);
                uint8_t pc=(uint8_t)(pos-(uint8_t)(row*20u));
                mark_skip(row,pc);
                mark_skip((uint8_t)(17u-row),pc);
            }
            /* Remove only the exact participating endpoint(s). A coarse-X
             * mask is unsafe in crowded cells: a different physical endpoint
             * may legitimately live on that same 8px edge. The packed owner
             * flags already identify which authored endpoint caused this
             * transition, so use that information directly. */
            for(e=first;e<=last;++e){
                uint8_t lo=g_tspf_mixed_event_left[e];
                uint8_t ro=g_tspf_mixed_event_right[e];
                if(lo!=0xffu && (lo&0x40u))
                    g_tspf_mixed_border_clear_sid[lo&31u]|=2u;
                if(ro!=0xffu && (ro&0x20u))
                    g_tspf_mixed_border_clear_sid[ro&31u]|=1u;
            }
        }else{
            /* Transactional per-tile fallback: make speculative patterns and
             * patches unreachable, and leave this tile entirely to coarse. */
            g_tspf_mixed_pattern_count=n0;
            s_patch_count=p0;
            if(result==2u)++g_tspf_mixed_unsupported_tiles;
            else{
                ++g_tspf_mixed_local_fallbacks;
                if(result==3u)++g_tspf_mixed_chain_fallbacks;
            }
        }
        i=(uint8_t)(last+1u);
    }

    refine_restore_cols();

    if(!s_patch_count || !g_tspf_mixed_pattern_count){
        /* A connected handoff can require ONLY removal of the old snapped
         * border while every pixel remains ordinary wall fill. Keep the
         * successful border-clear decision even when no dynamic pattern is
         * needed; mixed-skip itself is already empty in that case. */
        return;
    }
    s_prepared=1u;
    g_tspf_mixed_last_patterns=g_tspf_mixed_pattern_count;
    g_tspf_mixed_last_patches=s_patch_count;
    g_tspf_mixed_upload_index=0u;
    g_tspf_mixed_patterns_pending=1u;
}

void tsp_polar_mixed_apply(void) BANKED{
    uint8_t i;

    if(s_hold_previous){
        /* Keep the CPU map and coverage coherent with the still-being-published
         * direct generation. Coarse materialization skipped these exact cells
         * above; ownership here prevents nt_end_frame from restoring them as
         * stale while the VDP publication fence is outstanding. */
        for(i=0u;i<s_prev_count;++i){
            uint8_t pos=s_prev_pos[i];
            uint8_t row=(uint8_t)(pos/20u);
            uint8_t col=(uint8_t)(pos-(uint8_t)(row*20u));
            own_cell(row,col);
            own_cell((uint8_t)(17u-row),col);
        }
        s_prepared=0u;
        return;
    }

    /* No cooperative VBlank service occurs inside this banked commit. Fence
     * the old bank NOW, then re-dirty every old dynamic position. Therefore a
     * retire bit can clear only after an authoritative post-fence row upload. */
    retire_previous();

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
            /* A publication fence needs an acknowledgement only when the
             * name-table word actually changed. If the VDP already references
             * this exact dynamic ID, pattern publication alone is sufficient.
             * Marking an unchanged row as awaiting publication can deadlock the
             * hold state because there is no dirty interval to trigger an OTIR
             * acknowledgement for that row. */
            if(g_map[idx]!=word){
                g_map[idx]=word;dirty_cell(row,col);
                g_tspf_mixed_publish_rows[row]=1u;
            }
            if(g_map[bidx]!=bword){
                g_map[bidx]=bword;dirty_cell(brow,col);
                g_tspf_mixed_publish_rows[brow]=1u;
            }
            own_cell(row,col);own_cell(brow,col);
            s_prev_pos[i]=pos;
        }
        s_prev_count=s_patch_count;
        s_bank_used[s_target_bank]=1u;
        s_prev_bank=s_target_bank;
    }else{
        s_prev_bank=0xffu;
        s_prev_count=0u;
    }
    s_prepared=0u;
}

void tsp_polar_mixed_boot_published(void) BANKED{
    uint8_t i;
    for(i=0u;i<TSP_ROWS;++i)g_tspf_mixed_publish_rows[i]=0u;
}

void tsp_polar_mixed_upload(void) BANKED{
    uint8_t slot,y,stop;
    uint8_t tile[32];
    if(!g_tspf_mixed_patterns_pending)return;

    /* Bound pattern publication just like the row uploader. Eight 4-bpp
     * patterns are 256 VDP data bytes: close to the already-proven six-row
     * worst case (240 bytes), without letting a rare 18-pattern mixed frame
     * monopolize an entire safe VBlank interval. Name-table rows remain
     * blocked until every pattern in the bank is resident. */
    slot=g_tspf_mixed_upload_index;
    stop=(uint8_t)(slot+8u);
    if(stop<slot || stop>g_tspf_mixed_pattern_count)
        stop=g_tspf_mixed_pattern_count;

    for(;slot<stop;++slot){
        const uint8_t *p=&g_tspf_mixed_pattern_data[(uint16_t)slot<<4];
        for(y=0u;y<8u;++y){
            tile[(uint8_t)(y*4u)]=p[(uint8_t)(y+y)];
            tile[(uint8_t)(y*4u+1u)]=0u;
            tile[(uint8_t)(y*4u+2u)]=p[(uint8_t)(y+y+1u)];
            tile[(uint8_t)(y*4u+3u)]=0u;
        }
        set_bkg_4bpp_data((uint16_t)(g_tspf_mixed_pattern_base+slot),1u,tile);
    }

    g_tspf_mixed_upload_index=slot;
    if(slot>=g_tspf_mixed_pattern_count)
        g_tspf_mixed_patterns_pending=0u;
}

#else

void tsp_polar_mixed_reset(void) BANKED{}
void tsp_polar_mixed_begin_frame(void) BANKED{
    uint8_t i;
    g_tspf_mixed_any=0u;
    g_tspf_mixed_event_count=0u;
    g_tspf_mixed_event_overflow=0u;
    for(i=0u;i<TSP_COLS*3u;++i)g_tspf_mixed_skip[i]=0u;
    g_tspf_mixed_force_any=0u;
    for(i=0u;i<TSP_COLS;++i)g_tspf_mixed_force_col[i]=0u;
    for(i=0u;i<32u;++i)g_tspf_mixed_border_clear_sid[i]=0u;
    for(i=0u;i<TSP_ROWS;++i)g_tspf_mixed_publish_rows[i]=0u;
}
void tsp_polar_mixed_prepare(const TSPState *s) BANKED{(void)s;}
void tsp_polar_mixed_apply(void) BANKED{}
void tsp_polar_mixed_boot_published(void) BANKED{}
void tsp_polar_mixed_upload(void) BANKED{g_tspf_mixed_patterns_pending=0u;}

#endif
#endif
