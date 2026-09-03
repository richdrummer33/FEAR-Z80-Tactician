/*
 * Fresh Adaptive Polar Visibility Field renderer for Game Gear.
 *
 * This file intentionally does NOT transform/patch the legacy TileSector renderer.
 * The old renderer remains the oracle/reference. This path starts from the baked
 * runtime recipe field and emits GG name-table words directly.
 */
#if defined(__SDCC)
#pragma bank 255
#include <gbdk/platform.h>
BANKREF(tilesector_polar_renderer_bank)
#endif

#include <stdint.h>
#include <string.h>
#include "tilesector_polar.h"
#include "generated/tilesector_polar_data.inc"
#ifdef TSPF_E1M1_ROOM1
#include "generated/e1m1_room1_exact_geometry.h"
#include "e1m1_room1_polar_meta.h"
#include "e1m1_room1_polar_pvs.h"
#endif

#ifndef TSPF_LOCAL_PROJECTION
#define TSPF_LOCAL_PROJECTION 0
#endif
#ifndef TSPF_SCREEN_DEPTH_PLANE
#define TSPF_SCREEN_DEPTH_PLANE 0
#endif
#ifndef TSPF_EDGE_CHEMTRAIL_FIX
#define TSPF_EDGE_CHEMTRAIL_FIX 0
#endif
#ifndef TSPF_FORCE_C_MATERIALIZER
#define TSPF_FORCE_C_MATERIALIZER 0
#endif
#ifndef TSPF_HOST_PIXEL_COMPOSITE
#define TSPF_HOST_PIXEL_COMPOSITE 0
#endif
#if !defined(__SDCC) && TSPF_HOST_PIXEL_COMPOSITE
void tsp_host_composite_write(uint8_t row,uint8_t col,uint16_t word);
void tsp_host_composite_surface(uint8_t col,uint8_t clip_x0,uint8_t clip_x1,
                                int16_t tl,int16_t tr,int16_t bl,int16_t br,
                                uint8_t sid,uint8_t shade,uint8_t border,
                                uint8_t ao_left,uint8_t ao_right);
#endif
#if defined(__SDCC) && TSPF_LOCAL_PROJECTION
#include "tilesector_polar_projection_meta.h"
#endif
#if defined(__SDCC) && TSPF_SCREEN_DEPTH_PLANE
#include "tilesector_polar_depthplane_lut.h"
#endif

#ifdef TSPF_E1M1_ROOM1
#define TSPF_MAX_ACTIVE E1PF_SEGMENT_COUNT
#define TSPF_WORLD_VERTEX_COUNT E1PF_VERTEX_COUNT
#else
#define TSPF_MAX_ACTIVE 20u
#define TSPF_WORLD_VERTEX_COUNT 14u
#endif
#define TSPF_HORIZON 72
#define TSPF_NEAR_Z_Q4 (10<<4)
#define TSPF_FAR_Z_Q4  (127<<4)

#ifdef __SDCC
void tsp_polar_nt_begin_frame(void);
void tsp_polar_nt_end_frame(void);
void tsp_polar_surface_column_fast(void);
void tsp_polar_run_geometry_fast(void);
#ifdef TSPF_E1M1_ROOM1
void tsp_polar_run_zspan_fast(void);
#endif
#if TSPF_LOCAL_PROJECTION
void tsp_polar_projection_eval_fast(void);
#endif
extern uint8_t g_polar_nt_cov_cur[60];
extern uint8_t g_polar_nt_row_min[18];
extern uint8_t g_polar_nt_row_max[18];
/* Explicit assembly bridge symbols. No C struct layout and no implicit
 * register-argument ABI: the Z80 materializer reads these exact globals. */
uint8_t g_polar_mat_col;
uint8_t g_polar_mat_shade;
uint8_t g_polar_mat_border;
int16_t g_polar_mat_top_l;
int16_t g_polar_mat_top_r;
int16_t g_polar_mat_bot_l;
int16_t g_polar_mat_bot_r;

/* One-call geometry-run bridge. C computes only run bounds and the Q6 depth
 * increment; Z80 walks every coarse column and materializes it directly. */
uint8_t g_polar_run_c0;
uint8_t g_polar_run_c1;
uint8_t g_polar_run_profile;
uint8_t g_polar_run_left_real;
uint8_t g_polar_run_right_real;
int16_t g_polar_run_iq;
int16_t g_polar_run_step;
/* Present in every GG build because the shared materializer exports the E1M1
 * z-span entry point even when the standard Polar world does not call it. */
int16_t g_polar_run_z0_q4_rel;
int16_t g_polar_run_z1_q4_rel;
#endif

volatile uint8_t g_tspf_appearance_mode;
#if TSPF_PROFILE_HOOKS || !defined(__SDCC)
volatile uint8_t g_tspf_stage;
/* Compatibility marker for the existing Gearsystem stage profiler. */
volatile uint8_t g_ts_render_stage;
volatile uint8_t g_tspf_active_runs;
volatile uint8_t g_tspf_selector_tests;
volatile uint16_t g_tspf_touched_cells;
#define TSPF_SET_STAGE(v) do { g_tspf_stage=(v); g_ts_render_stage=(v); } while(0)
#define TSPF_SELECTOR_HIT() (++g_tspf_selector_tests)
#else
#define TSPF_SET_STAGE(v) ((void)0)
#define TSPF_SELECTOR_HIT() ((void)0)
#endif

static const uint16_t k_row_base[TSP_ROWS] = {
    0u,20u,40u,60u,80u,100u,120u,140u,160u,
    180u,200u,220u,240u,260u,280u,300u,320u,340u
};
#ifdef __SDCC
static const uint8_t k_nt_mask8[8] = {1u,2u,4u,8u,16u,32u,64u,128u};
#endif
static const uint8_t k_col_recip_q8[21] = {
    0,255,128,85,64,51,43,36,32,28,26,23,21,20,18,17,16,15,14,13,13
};

typedef struct PolarRun {
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
    int16_t iq;
    int16_t step;
} PolarRun;

static PolarRun g_runs[TSPF_MAX_ACTIVE];
static uint8_t g_run_order[TSPF_MAX_ACTIVE];
#if defined(__SDCC) && TSPF_SCREEN_DEPTH_PLANE
static int8_t g_depth_nf_q7[TSPF_DEPTH_NORMAL_CLASS_COUNT];
static int8_t g_depth_stepfac_q4[TSPF_DEPTH_NORMAL_CLASS_COUNT];
static uint8_t g_depth_yaw_cache=0xffu;
#endif
/* Connected spans share authored corners.  E1M1 raises the authored-corner
 * count from fourteen to forty-four, so validity is a compact byte bitset
 * rather than a hard-coded sixteen-bit mask. */
uint16_t g_corner_bearing_q12[TSPF_WORLD_VERTEX_COUNT];
static uint8_t g_corner_bearing_valid[(TSPF_WORLD_VERTEX_COUNT+7u)/8u];
#if defined(__SDCC) && TSPF_LOCAL_PROJECTION
/* Cell-local ROM projection field. The selected cell block is copied once on
 * cell transition; ordinary render updates consume only WRAM thereafter.
 * Pointer/depth/lx/ly globals are an explicit fixed-ASM bridge. */
static uint8_t g_proj_cell[TSPF_PROJ_MAX_CELL_BYTES];
const uint8_t *g_proj_corner_ptr[14];
uint8_t g_proj_corner_depth[14];
uint8_t g_proj_lx;
uint8_t g_proj_ly;
static uint16_t g_proj_fallback_mask;
static uint16_t g_proj_cached_gi=0xffffu;
#endif
static const uint16_t k_corner_mask[14] = {
    0x0001u,0x0002u,0x0004u,0x0008u,0x0010u,0x0020u,0x0040u,
    0x0080u,0x0100u,0x0200u,0x0400u,0x0800u,0x1000u,0x2000u
};
#ifndef __SDCC
static uint8_t g_touched_bits[45];
static uint16_t g_touched_list[TSP_MAP_CELLS]; /* host oracle lifetime tracking */
static uint16_t g_touched_count;
static uint8_t g_map_ready;
#endif

static int8_t clamp_s8(int16_t v,int8_t lo,int8_t hi){if(v<lo)return lo;if(v>hi)return hi;return (int8_t)v;}
static uint8_t clamp_u8i(int16_t v,uint8_t hi){if(v<0)return 0;if(v>hi)return hi;return (uint8_t)v;}
static int16_t shr_signed(int16_t v,uint8_t n){return v>=0?(int16_t)(v>>n):(int16_t)-(((-v)>>n));}

static uint8_t world_vx(uint8_t vid){
#ifdef TSPF_E1M1_ROOM1
    return k_e1x_vertices[vid].x;
#else
    return k_tspf_vx[vid];
#endif
}
static uint8_t world_vy(uint8_t vid){
#ifdef TSPF_E1M1_ROOM1
    return k_e1x_vertices[vid].y;
#else
    return k_tspf_vy[vid];
#endif
}
static uint8_t world_seg_anchor(uint8_t sid){
#ifdef TSPF_E1M1_ROOM1
    return k_e1pf_seg_anchor[sid];
#else
    return k_tspf_seg_anchor[sid];
#endif
}
static int8_t world_seg_nx(uint8_t sid){
#ifdef TSPF_E1M1_ROOM1
    return k_e1pf_nx_q5[sid];
#else
    return k_tspf_nx_q5[sid];
#endif
}
static int8_t world_seg_ny(uint8_t sid){
#ifdef TSPF_E1M1_ROOM1
    return k_e1pf_ny_q5[sid];
#else
    return k_tspf_ny_q5[sid];
#endif
}
static int8_t world_shade_bias(uint8_t sid){
#ifdef TSPF_E1M1_ROOM1
    return k_e1pf_shade_bias[sid];
#else
    return k_tspf_shade_bias[sid];
#endif
}
/* Perspective vertical translation for a camera whose nominal eye is z=16.
 * Current projection maps a 32-unit wall to invz screen pixels, so moving the
 * eye by dz world units moves a surface by dz*invz/32 pixels. Stair heights
 * are integral world units; keeping dz small avoids any 32-bit helper on Z80. */
static int16_t camera_z_shift(uint8_t inv,const TSPState *s){
    int16_t dz=(int16_t)((s->z_q4-TSP_EYE_HEIGHT_Q4)>>4);
    return shr_signed((int16_t)(dz*(int16_t)inv),5u);
}
static int16_t signed_q12(uint16_t v){v&=4095u;return v>=2048u?(int16_t)v-4096:(int16_t)v;}

#ifndef __SDCC
static uint16_t base_word(uint8_t row){
    if(row<9u)return TSP_TILE_CEILING;
    if(row==9u)return TSP_TILE_HORIZON;
    return TSP_TILE_FLOOR;
}
static void map_init(uint16_t *out){
    uint8_t r,c;
    for(r=0;r<TSP_ROWS;++r)for(c=0;c<TSP_COLS;++c)out[k_row_base[r]+c]=base_word(r);
    memset(g_touched_bits,0,sizeof(g_touched_bits));g_touched_count=0;g_map_ready=1u;
}
static void restore_touched(uint16_t *out){
    uint16_t i;
    for(i=0;i<g_touched_count;++i){
        uint16_t rc=g_touched_list[i];uint8_t r=(uint8_t)(rc>>8),c=(uint8_t)rc;uint16_t idx=k_row_base[r]+c;
        out[idx]=base_word(r);g_touched_bits[idx>>3]&=(uint8_t)~(1u<<(idx&7u));
    }
    g_touched_count=0u;
}
#endif
static void put_cell(uint16_t *out,uint8_t row,uint8_t col,uint16_t word){
#ifdef __SDCC
    /* AO/C fallback only. Fast geometry/shade assembly marks whole spans and
     * dirties changed words directly. Keep the fallback lifetime-correct too. */
    uint16_t idx=k_row_base[row]+col;
    uint8_t cov_i=(uint8_t)(col+col+col+(row>>3));
    g_polar_nt_cov_cur[cov_i]|=k_nt_mask8[row&7u];
    if(out[idx]!=word){
        out[idx]=word;
        if(g_polar_nt_row_min[row]==0xffu || col<g_polar_nt_row_min[row]) g_polar_nt_row_min[row]=col;
        if(col>g_polar_nt_row_max[row]) g_polar_nt_row_max[row]=col;
    }
#else
    uint16_t idx=k_row_base[row]+col;uint8_t *b=&g_touched_bits[idx>>3];uint8_t m=(uint8_t)(1u<<(idx&7u));
    if(!(*b&m)){*b|=m;g_touched_list[g_touched_count++]=(uint16_t)(((uint16_t)row<<8)|col);}
    out[idx]=word;
#endif
}

void tsp_polar_renderer_reset(void) BANKED {
#if defined(__SDCC) && TSPF_LOCAL_PROJECTION
    g_proj_cached_gi=0xffffu;
#endif
#if defined(__SDCC) && TSPF_SCREEN_DEPTH_PLANE
    g_depth_yaw_cache=0xffu;
#endif
#ifndef __SDCC
    g_map_ready=0u;g_touched_count=0u;memset(g_touched_bits,0,sizeof(g_touched_bits));
#endif
    TSPF_SET_STAGE(0u);
#if TSPF_PROFILE_HOOKS || !defined(__SDCC)
    g_tspf_active_runs=0u;g_tspf_selector_tests=0u;g_tspf_touched_cells=0u;
#endif
}

static uint8_t ao_class(uint8_t vid){
#ifdef TSPF_E1M1_ROOM1
    (void)vid;return 0u; /* Room-1 AO is authored/baked later, never guessed at runtime. */
#else
    return (uint8_t)((k_tspf_ao[vid>>2]>>((vid&3u)*2u))&3u);
#endif
}
static uint8_t selector_pass(uint8_t sid,uint8_t lx,uint8_t ly){
    int16_t v=(int16_t)k_tspf_sel_a[sid]*(int16_t)lx+(int16_t)k_tspf_sel_b[sid]*(int16_t)ly+k_tspf_sel_c[sid];
    TSPF_SELECTOR_HIT();
    return (uint8_t)(((v>=0)?1u:0u)^k_tspf_sel_inv[sid]);
}

static uint16_t bearing_q12(int16_t dxq4,int16_t dyq4);

#if defined(__SDCC) && TSPF_LOCAL_PROJECTION
static void projection_load_cell(uint8_t gx,uint8_t gy,uint16_t gi){
    uint16_t local;
    const uint8_t *p;
    uint16_t mask;
    uint8_t v,d;
#if TSPF_PROJ_ROWS_PER_BANK == 4u && TSPF_PROJ_BANK_COUNT == 6u
    local=(uint16_t)((uint16_t)(gy&3u)*48u+gx);
    switch(gy>>2){
        case 0u:tsp_polar_proj_load_bank0(local,g_proj_cell);break;
        case 1u:tsp_polar_proj_load_bank1(local,g_proj_cell);break;
        case 2u:tsp_polar_proj_load_bank2(local,g_proj_cell);break;
        case 3u:tsp_polar_proj_load_bank3(local,g_proj_cell);break;
        case 4u:tsp_polar_proj_load_bank4(local,g_proj_cell);break;
        default:tsp_polar_proj_load_bank5(local,g_proj_cell);break;
    }
#elif TSPF_PROJ_ROWS_PER_BANK == 1u && TSPF_PROJ_BANK_COUNT == 24u
    /* Diagnostic layout: one world-grid row per ROM source.  This lets us
     * measure much tighter global error thresholds without pretending the
     * resulting ROM footprint is a production recommendation. */
    local=gx;
    switch(gy){
        case 0u:tsp_polar_proj_load_bank0(local,g_proj_cell);break;
        case 1u:tsp_polar_proj_load_bank1(local,g_proj_cell);break;
        case 2u:tsp_polar_proj_load_bank2(local,g_proj_cell);break;
        case 3u:tsp_polar_proj_load_bank3(local,g_proj_cell);break;
        case 4u:tsp_polar_proj_load_bank4(local,g_proj_cell);break;
        case 5u:tsp_polar_proj_load_bank5(local,g_proj_cell);break;
        case 6u:tsp_polar_proj_load_bank6(local,g_proj_cell);break;
        case 7u:tsp_polar_proj_load_bank7(local,g_proj_cell);break;
        case 8u:tsp_polar_proj_load_bank8(local,g_proj_cell);break;
        case 9u:tsp_polar_proj_load_bank9(local,g_proj_cell);break;
        case 10u:tsp_polar_proj_load_bank10(local,g_proj_cell);break;
        case 11u:tsp_polar_proj_load_bank11(local,g_proj_cell);break;
        case 12u:tsp_polar_proj_load_bank12(local,g_proj_cell);break;
        case 13u:tsp_polar_proj_load_bank13(local,g_proj_cell);break;
        case 14u:tsp_polar_proj_load_bank14(local,g_proj_cell);break;
        case 15u:tsp_polar_proj_load_bank15(local,g_proj_cell);break;
        case 16u:tsp_polar_proj_load_bank16(local,g_proj_cell);break;
        case 17u:tsp_polar_proj_load_bank17(local,g_proj_cell);break;
        case 18u:tsp_polar_proj_load_bank18(local,g_proj_cell);break;
        case 19u:tsp_polar_proj_load_bank19(local,g_proj_cell);break;
        case 20u:tsp_polar_proj_load_bank20(local,g_proj_cell);break;
        case 21u:tsp_polar_proj_load_bank21(local,g_proj_cell);break;
        case 22u:tsp_polar_proj_load_bank22(local,g_proj_cell);break;
        default:tsp_polar_proj_load_bank23(local,g_proj_cell);break;
    }
#else
#error Unsupported Polar projection bank partition
#endif
    p=g_proj_cell;
    mask=(uint16_t)p[0]|((uint16_t)p[1]<<8);p+=2;
    g_proj_fallback_mask=0u;
    for(v=0u;v<14u;++v){
        g_proj_corner_depth[v]=0xffu;
        g_proj_corner_ptr[v]=(const uint8_t *)0;
        if(mask&k_corner_mask[v]){
            d=*p++;
            if(d==0xffu)g_proj_fallback_mask|=k_corner_mask[v];
            else{
                g_proj_corner_depth[v]=d;
                g_proj_corner_ptr[v]=p;
                p+=(uint16_t)(4u<<((uint8_t)(d+d)));
            }
        }
    }
    g_proj_cached_gi=gi;
}

static void projection_eval_fallback(const TSPState *s){
    uint8_t v;
    uint16_t m=g_proj_fallback_mask;
    for(v=0u;v<14u&&m;++v){
        uint16_t bit=k_corner_mask[v];
        if(m&bit){
            g_corner_bearing_q12[v]=bearing_q12(
                (int16_t)((int16_t)k_tspf_vx[v]<<4)-s->x_q4,
                (int16_t)((int16_t)k_tspf_vy[v]<<4)-s->y_q4);
            m=(uint16_t)(m&~bit);
        }
    }
}
#endif


/* Exact modulo-8-bit equivalent of ((uint32_t)n*recip_q16 + 128)>>8,
 * decomposed into two 8x8->16 products so SDCC never pulls __mullong. */
static uint8_t ratio_q8_exact(uint8_t n,uint8_t d){
    uint16_t rec,p_lo,p_hi,q;
    if(!d)return 0u;
    rec=k_tspf_recip8_q16[d];
    p_lo=(uint16_t)n*(uint8_t)rec;
    p_hi=(uint16_t)n*(uint8_t)(rec>>8);
    q=(uint16_t)(p_hi+((p_lo+128u)>>8));
    return (uint8_t)q;
}

static uint16_t bearing_q12(int16_t dxq4,int16_t dyq4){
    uint8_t sx,sy,ax8,ay8,ratio;uint16_t ax,ay,a;
    if(dxq4==0&&dyq4==0)return 0u;
    sx=(uint8_t)(dxq4<0);sy=(uint8_t)(dyq4<0);ax=(uint16_t)(dxq4<0?-dxq4:dxq4);ay=(uint16_t)(dyq4<0?-dyq4:dyq4);
    while(ax>255u||ay>255u){ax=(uint16_t)((ax+1u)>>1);ay=(uint16_t)((ay+1u)>>1);}
    ax8=(uint8_t)ax;ay8=(uint8_t)ay;
    if(ax8>=ay8){ratio=ratio_q8_exact(ay8,ax8);a=k_tspf_atan_q12[ratio];}
    else {ratio=ratio_q8_exact(ax8,ay8);a=(uint16_t)(1024u-k_tspf_atan_q12[ratio]);}
    if(sx) a=(uint16_t)(2048u-a);
    if(sy) a=(uint16_t)(0u-a);
    return (uint16_t)(a&4095u);
}
#if !defined(__SDCC) || !TSPF_LOCAL_PROJECTION
static uint16_t bearing_vertex_q12(uint8_t vid,const TSPState *s){
    uint8_t bi=(uint8_t)(vid>>3),mask=(uint8_t)(1u<<(vid&7u));
    if(!(g_corner_bearing_valid[bi]&mask)){
        g_corner_bearing_q12[vid]=bearing_q12(
            (int16_t)((int16_t)world_vx(vid)<<4)-s->x_q4,
            (int16_t)((int16_t)world_vy(vid)<<4)-s->y_q4);
        g_corner_bearing_valid[bi]|=mask;
    }
    return g_corner_bearing_q12[vid];
}
#endif
static uint8_t angle_x(int16_t rel){
    uint16_t a=(uint16_t)(rel<0?-rel:rel);uint16_t x;if(a>512u)a=512u;
    x=rel<0?(uint16_t)(160u-k_tspf_angle_x_pos[a]):k_tspf_angle_x_pos[a];
    return (uint8_t)(x>159u?159u:x);
}
static uint8_t inv_for_dq4(int16_t dq4){
    uint16_t a=(uint16_t)(dq4<0?-dq4:dq4);uint8_t z,f,x0,x1;int16_t d;
    if(a<=TSPF_NEAR_Z_Q4) return 255u;
    if(a>=TSPF_FAR_Z_Q4) return k_tspf_invz[127];
    z=(uint8_t)(a>>4);f=(uint8_t)(a&15u);x0=k_tspf_invz[z];x1=k_tspf_invz[(uint8_t)(z+1u)];d=(int16_t)x1-(int16_t)x0;
    return (uint8_t)((int16_t)x0+shr_signed((int16_t)(d*(int16_t)f+(d>=0?8:-8)),4));
}
static int16_t wall_d_q4(uint8_t sid,uint8_t anchor_vid,const TSPState *s){
    int8_t nx=world_seg_nx(sid),ny=world_seg_ny(sid);
    /* Exact cardinal-wall identities. For +/-32 Q5 normals the original
     * multiply/shift expression reduces to one Q4 coordinate subtraction. */
    if(ny==0&&(nx==32||nx==-32)){
        int16_t wall=(int16_t)((int16_t)world_vx(anchor_vid)<<4);
        return nx>0?(int16_t)(wall-s->x_q4):(int16_t)(s->x_q4-wall);
    }
    if(nx==0&&(ny==32||ny==-32)){
        int16_t wall=(int16_t)((int16_t)world_vy(anchor_vid)<<4);
        return ny>0?(int16_t)(wall-s->y_q4):(int16_t)(s->y_q4-wall);
    }
    {
        int16_t xi=(int16_t)(s->x_q4>>4),yi=(int16_t)(s->y_q4>>4);uint8_t fx=(uint8_t)(s->x_q4&15),fy=(uint8_t)(s->y_q4&15);
        int16_t dx=(int16_t)world_vx(anchor_vid)-xi,dy=(int16_t)world_vy(anchor_vid)-yi;
        int16_t whole=(int16_t)nx*dx+(int16_t)ny*dy;
        int16_t frac=(int16_t)nx*fx+(int16_t)ny*fy;
        return (int16_t)(shr_signed(whole,1)-shr_signed(frac,5));
    }
}
static uint8_t inv_at_invd(uint8_t sid,uint8_t invd,uint16_t world_bearing,int16_t rel){
    uint8_t bi=(uint8_t)(world_bearing>>4);int8_t sn=(int8_t)k_tspf_sin_q7[bi],cs=(int8_t)k_tspf_sin_q7[(uint8_t)(bi+64u)],nx=world_seg_nx(sid),ny=world_seg_ny(sid);int16_t dot;uint16_t q,sec;
    /* Exact cardinal-normal shortcuts: (+/-32 * trig) >> 5 == +/-trig.
     * The final magnitude discards normal sign, so no multiply is needed. */
    if(ny==0&&(nx==32||nx==-32)) dot=cs;
    else if(nx==0&&(ny==32||ny==-32)) dot=sn;
    else dot=shr_signed((int16_t)((int16_t)nx*cs+(int16_t)ny*sn),5);
    if(dot<0)dot=(int16_t)-dot;if(dot>127)dot=127;
    q=((uint16_t)invd*(uint16_t)dot+64u)>>7;sec=k_tspf_sec_q7[(uint16_t)(rel<0?-rel:rel)];q=(q*sec+64u)>>7;return (uint8_t)(q>255u?255u:q);
}
static uint8_t shade_for(uint8_t inv,int8_t bias){int8_t s;if(inv>=82u)s=2;else if(inv>=46u)s=1;else s=0;s=(int8_t)(s+bias);if(s<0)s=0;if(s>2)s=2;return (uint8_t)s;}

#if defined(__SDCC) && TSPF_SCREEN_DEPTH_PLANE
/* Straight-wall inverse camera depth is linear in screen x.  Use a compact
 * normal-class/yaw table to hand the run walker its final Q6 start+step,
 * instead of evaluating two endpoint ray/normal/secant expressions and then
 * reconstructing a step from them.  If the signed wall plane crosses zero
 * inside the visible run, keep the old endpoint path as the exact fallback. */
static uint8_t screen_depth_plane(uint8_t sid,uint8_t invd,uint8_t c0,uint8_t c1,PolarRun *r){
    uint8_t cls=k_tspf_depth_normal_class[sid];
    int8_t nf=g_depth_nf_q7[cls],sf=g_depth_stepfac_q4[cls];
    int16_t iq=shr_signed((int16_t)((int16_t)invd*(int16_t)nf),1);
    int16_t step=shr_signed((int16_t)((int16_t)invd*(int16_t)sf),4);
    int16_t endq=iq,midq;
    uint8_t i,n=(uint8_t)(c1-c0+1u);

    if(c0<10u){for(i=c0;i<10u;++i)iq=(int16_t)(iq-step);}
    else {for(i=10u;i<c0;++i)iq=(int16_t)(iq+step);}

    endq=iq;
    for(i=0u;i<n;++i)endq=(int16_t)(endq+step);
    if((iq<0&&endq>0)||(iq>0&&endq<0))return 0u;
    if(iq<0||endq<0){iq=(int16_t)-iq;endq=(int16_t)-endq;step=(int16_t)-step;}

    midq=(int16_t)((iq+endq)>>1);
    r->c0=c0;r->c1=c1;r->iq=iq;r->step=step;r->depth_plane=1u;
    r->inv_mid=clamp_u8i((int16_t)((midq+32)>>6),255u);
    return 1u;
}
#endif

static uint8_t project_key(uint8_t keyid,const TSPState *s,PolarRun *r){
#ifdef TSPF_E1M1_ROOM1
    uint8_t sid=keyid,v0=k_e1pf_seg_v0[keyid],v1=k_e1pf_seg_v1[keyid];
#else
    uint16_t w=k_tspf_keys[keyid];uint8_t sid=(uint8_t)(w&31u),v0=(uint8_t)((w>>5)&15u),v1=(uint8_t)((w>>9)&15u);
#endif
    uint16_t a0,a1,len,yawq;int16_t st,en,lo,hi;uint8_t x0,x1,invd;
#if defined(__SDCC) && TSPF_LOCAL_PROJECTION
    a0=g_corner_bearing_q12[v0];
    a1=g_corner_bearing_q12[v1];
#else
    a0=bearing_vertex_q12(v0,s);
    a1=bearing_vertex_q12(v1,s);
#endif
#ifdef TSPF_E1M1_ROOM1
    {
        int16_t d=signed_q12((uint16_t)(a1-a0));
        if(!d)return 0u;
        if(d<0){uint16_t ta=a0;a0=a1;a1=ta;{uint8_t tv=v0;v0=v1;v1=tv;}d=(int16_t)-d;}
        len=(uint16_t)d;
    }
#else
    len=(uint16_t)((a1-a0)&4095u);if(len==0u||len>=2048u)return 0u;
#endif
    yawq=(uint16_t)s->yaw<<4;st=signed_q12((uint16_t)(a0-yawq));en=(int16_t)(st+(int16_t)len);
    while(en<-512){st=(int16_t)(st+4096);en=(int16_t)(en+4096);}while(st>512){st=(int16_t)(st-4096);en=(int16_t)(en-4096);}
    lo=st<-512?-512:st;hi=en>512?512:en;if(hi<=lo)return 0u;x0=angle_x(lo);x1=angle_x(hi);if(x1<x0){uint8_t t=x0;x0=x1;x1=t;}if(x1==x0&&x1<159u)++x1;
    r->sid=sid;r->v0=v0;r->v1=v1;r->x0=x0;r->x1=x1;r->left_real=(uint8_t)(lo==st);r->right_real=(uint8_t)(hi==en);
    r->depth_plane=0u;
    invd=inv_for_dq4(wall_d_q4(sid,world_seg_anchor(sid),s));
#if defined(__SDCC) && TSPF_SCREEN_DEPTH_PLANE
    if(g_tspf_appearance_mode<2u){
        uint8_t c0=(uint8_t)(x0>>3),c1=(uint8_t)(x1>>3);
        if(c0>=TSP_COLS)c0=TSP_COLS-1u;if(c1>=TSP_COLS)c1=TSP_COLS-1u;
        if(c1>=c0&&screen_depth_plane(sid,invd,c0,c1,r))return 1u;
    }
#endif
    r->inv0=inv_at_invd(sid,invd,(uint16_t)(yawq+lo)&4095u,lo);
    r->inv1=inv_at_invd(sid,invd,(uint16_t)(yawq+hi)&4095u,hi);
    r->inv_mid=(uint8_t)(((uint16_t)r->inv0+r->inv1)>>1);return 1u;
}

static uint16_t edge_entry(uint8_t shade,int16_t local_left,int8_t slope,uint8_t bottom){
    uint16_t attr=0;uint8_t mag;int8_t off;if(bottom){local_left=(int16_t)(7-local_left);slope=(int8_t)-slope;attr=(uint16_t)(TSP_ATTR_FLIPY|TSP_ATTR_PALETTE);}if(slope<0){mag=(uint8_t)(-slope);local_left=(int16_t)(local_left-mag);attr|=TSP_ATTR_FLIPX;}else mag=(uint8_t)slope;if(mag>=TSP_EDGE_SLOPE_COUNT)mag=TSP_EDGE_SLOPE_COUNT-1u;off=clamp_s8(local_left,TSP_EDGE_OFF_MIN,(int8_t)(TSP_EDGE_OFF_MIN+TSP_EDGE_OFF_COUNT-1));return (uint16_t)(TSP_TILE_EDGE(shade,(uint8_t)(off-TSP_EDGE_OFF_MIN),mag)|attr);
}
static int8_t row_floor(int16_t y){return y>=0?(int8_t)(y>>3):(int8_t)-(((-y)+7)>>3);}
static void draw_edge(uint16_t *out,uint8_t col,int16_t yl,int16_t yr,uint8_t shade,uint8_t bottom){
    int8_t slope=clamp_s8((int16_t)(yr-yl),-7,7),r0=row_floor(yl<yr?yl:yr),r1=row_floor(yl>yr?yl:yr),r;
    if(r0<0)r0=0;
    if(r1>=(int8_t)TSP_ROWS)r1=(int8_t)(TSP_ROWS-1u);
#if TSPF_EDGE_CHEMTRAIL_FIX
    /*
     * A projected edge can cross one screen-tile row boundary inside this
     * 8-pixel-wide column.  The old path emitted an independent partial edge
     * tile into BOTH rows.  That is mathematically literal but visually ugly
     * on a tile renderer: the secondary row becomes a moving 8x8 sliver whose
     * generic fill/outside half reads as the detached "edge chemtrail".
     *
     * Represent the crossing with ONE adaptive edge tile, chosen by the edge
     * midpoint.  The row on the solid-wall side is completed with a normal
     * wall tile; the row on the outside side is left untouched so the already
     * rendered farther/background surface survives.
     *
     * This is deliberately a tile-space ownership rule, not temporal state.
     */
    if(r0<r1){
        int16_t mid=(int16_t)((yl+yr)>>1);
        int8_t owner=row_floor(mid);
        if(owner<r0)owner=r0;
        if(owner>r1)owner=r1;

        put_cell(out,(uint8_t)owner,col,
                 edge_entry(shade,(int16_t)(yl-((int16_t)owner<<3)),slope,bottom));

        for(r=r0;r<=r1;++r){
            if(r==owner)continue;
            if((!bottom && r>owner) || (bottom && r<owner))
                put_cell(out,(uint8_t)r,col,TSP_TILE_FULL(shade,TSP_CAP_NONE,0u));
            /* Outside-side row intentionally preserves the background. */
        }
        return;
    }
#endif
    for(r=r0;r<=r1;++r)
        put_cell(out,(uint8_t)r,col,
                 edge_entry(shade,(int16_t)(yl-((int16_t)r<<3)),slope,bottom));
}
static void draw_full(uint16_t *out,uint8_t col,int8_t first,int8_t last,uint8_t shade,uint8_t border){
    int8_t r;if(first<0)first=0;if(last>=(int8_t)TSP_ROWS)last=(int8_t)(TSP_ROWS-1u);if(first>last)return;for(r=first;r<=last;++r)put_cell(out,(uint8_t)r,col,TSP_TILE_FULL(shade,TSP_CAP_NONE,border));
}
static void draw_run(uint16_t *out,TSPColumn *cols,const PolarRun *r,const TSPState *s){
    uint8_t c0=(uint8_t)(r->x0>>3),c1=(uint8_t)(r->x1>>3),n,c;
#ifdef TSPF_E1M1_ROOM1
    uint8_t profile=0xffu;
#else
    uint8_t profile=k_tspf_profile[r->sid];
#endifint16_t iq,step;if(c0>=TSP_COLS)c0=TSP_COLS-1;if(c1>=TSP_COLS)c1=TSP_COLS-1;if(c1<c0)return;n=(uint8_t)(c1-c0+1u);
#if defined(__SDCC) && TSPF_SCREEN_DEPTH_PLANE
    if(g_tspf_appearance_mode<2u&&r->depth_plane){c0=r->c0;c1=r->c1;n=(uint8_t)(c1-c0+1u);iq=r->iq;step=r->step;}
    else
#endif
    {iq=(int16_t)r->inv0<<6;step=(int16_t)(((int16_t)r->inv1-(int16_t)r->inv0)*(int16_t)k_col_recip_q8[n]);step=shr_signed(step,2);}
#if defined(__SDCC) && !TSPF_FORCE_C_MATERIALIZER
    /* The assembly column materializer now branches on profile for FULL
     * symmetry. Keep the run profile live for BOTH geometry-only and shaded
     * fast paths; previously only mode 0 initialized it because nothing else
     * consumed this bridge field. */
    if(g_tspf_appearance_mode<2u) g_polar_run_profile=profile;
#ifdef TSPF_E1M1_ROOM1
    if(g_tspf_appearance_mode==0u){
        g_polar_run_profile=0xffu;
        g_polar_run_c0=c0;g_polar_run_c1=c1;
        g_polar_run_left_real=r->left_real;g_polar_run_right_real=r->right_real;
        g_polar_run_iq=iq;g_polar_run_step=step;
        g_polar_run_z0_q4_rel=(int16_t)(((int16_t)k_e1pf_z0[r->sid]<<4)-s->z_q4);
        g_polar_run_z1_q4_rel=(int16_t)(((int16_t)k_e1pf_z1[r->sid]<<4)-s->z_q4);
        tsp_polar_run_zspan_fast();
        return;
    }
#else
    if(g_tspf_appearance_mode==0u && s->z_q4==TSP_EYE_HEIGHT_Q4){
        /* Preserve the zero-cost mature assembly run kernel on ordinary flat
         * floor states. Elevated states need asymmetric top/bottom projection,
         * so they fall through to the endpoint-aware column path below. */
        g_polar_run_c0=c0;g_polar_run_c1=c1;
        g_polar_run_left_real=r->left_real;g_polar_run_right_real=r->right_real;
        g_polar_run_iq=iq;g_polar_run_step=step;
        tsp_polar_run_geometry_fast();
        return;
    }
#endif
#endif
    for(c=c0;c<=c1;++c){uint8_t invl=(uint8_t)clamp_u8i((iq+32)>>6,255u),invr=(uint8_t)clamp_u8i((iq+step+32)>>6,255u),mid=(uint8_t)(((uint16_t)invl+invr)>>1),hl=(uint8_t)(invl>>1),hr=(uint8_t)(invr>>1);int16_t tl=(int16_t)(TSPF_HORIZON-hl),tr=(int16_t)(TSPF_HORIZON-hr),bl=(int16_t)(TSPF_HORIZON+hl),br=(int16_t)(TSPF_HORIZON+hr);uint8_t border=0,shade,edge_shade;
        /* POLAR_STAGE21_FULL_SYMMETRY_A: match the mature GG FULL convention.
         * The 144-line viewport is centred on y=71.5, so exact mirror geometry
         * is top=71-half and bottom=72+half. Other profiles stay unchanged. */
#ifdef TSPF_E1M1_ROOM1
        {
            int16_t dz0=(int16_t)(((int16_t)k_e1pf_z0[r->sid]<<4)-s->z_q4);
            int16_t dz1=(int16_t)(((int16_t)k_e1pf_z1[r->sid]<<4)-s->z_q4);
            int16_t iz0=shr_signed(dz0,4),iz1=shr_signed(dz1,4);
            bl=(int16_t)(TSPF_HORIZON-shr_signed((int16_t)(iz0*(int16_t)invl),5));
            br=(int16_t)(TSPF_HORIZON-shr_signed((int16_t)(iz0*(int16_t)invr),5));
            tl=(int16_t)(TSPF_HORIZON-shr_signed((int16_t)(iz1*(int16_t)invl),5));
            tr=(int16_t)(TSPF_HORIZON-shr_signed((int16_t)(iz1*(int16_t)invr),5));
        }
#else
        if(profile==TSP_PROFILE_FULL){tl--;tr--;}
#endif
        if(c==c0&&r->left_real) border|=1u;
        if(c==c1&&r->right_real) border|=2u;
        shade=g_tspf_appearance_mode?shade_for(mid,world_shade_bias(r->sid)):1u;edge_shade=shade;
        if(g_tspf_appearance_mode>=2u&&border){uint8_t cls=0;if((border&1u)&&r->left_real)cls=ao_class(r->v0);if((border&2u)&&r->right_real){uint8_t q=ao_class(r->v1);if(q>cls)cls=q;}if(cls&&edge_shade) --edge_shade;}
        /* Profile is constant for the whole run. Compute both left/right
         * endpoints together instead of two generic helper calls per column. */
        if(profile==TSP_PROFILE_LINTEL){
            bl=(int16_t)(TSPF_HORIZON-(hl>>1));br=(int16_t)(TSPF_HORIZON-(hr>>1));
        }else if(profile==TSP_PROFILE_RAISED){
            bl=(int16_t)(TSPF_HORIZON+hl-(hl>>2));br=(int16_t)(TSPF_HORIZON+hr-(hr>>2));
        }else if(profile==TSP_PROFILE_RISER){
            tl=(int16_t)(TSPF_HORIZON+hl-(hl>>2));tr=(int16_t)(TSPF_HORIZON+hr-(hr>>2));
        }
        /* Camera elevation translates BOTH projected endpoints. Do this after
         * profile shaping: FULL/LINTEL/RAISED/RISER are absolute world-height
         * bands, and a raised eye moves every one of those bands consistently. */
        {
            int16_t zl=camera_z_shift(invl,s),zr=camera_z_shift(invr,s);
            tl=(int16_t)(tl+zl);bl=(int16_t)(bl+zl);
            tr=(int16_t)(tr+zr);br=(int16_t)(br+zr);
        }
#if !defined(__SDCC) && TSPF_HOST_PIXEL_COMPOSITE
        {
            uint8_t coarse0=(uint8_t)(c<<3),coarse1=(uint8_t)(coarse0+7u);
            uint8_t clip0=r->x0>coarse0?r->x0:coarse0;
            uint8_t clip1=r->x1<coarse1?r->x1:coarse1;
            if(clip0<=clip1){
                uint8_t ao_left=(uint8_t)(((border&1u)&&r->left_real)?ao_class(r->v0):0u);
                uint8_t ao_right=(uint8_t)(((border&2u)&&r->right_real)?ao_class(r->v1):0u);
                tsp_host_composite_surface(c,clip0,clip1,tl,tr,bl,br,r->sid,shade,border,
                                           ao_left,ao_right);
            }
        }
#endif
#if defined(__SDCC) && !TSPF_FORCE_C_MATERIALIZER
        if(g_tspf_appearance_mode<2u){
            /* Fast-path geometry/shade materialization. The baked polar
             * renderer supplies final projected endpoints; this kernel only
             * turns them into the existing GG edge/full tile vocabulary. */
            g_polar_mat_col=c;
            g_polar_mat_shade=shade;
            g_polar_mat_border=border;
            g_polar_mat_top_l=tl;g_polar_mat_top_r=tr;
            g_polar_mat_bot_l=bl;g_polar_mat_bot_r=br;
            tsp_polar_surface_column_fast();
        }else
#endif
        {
            /* AO stays on the known C path until the fast kernel is proven
             * geometry-identical, then it gets a separate edge-shade byte. */
            draw_edge(out,c,tl,tr,edge_shade,0u);
            draw_edge(out,c,bl,br,edge_shade,1u);
            draw_full(out,c,(int8_t)(row_floor(tl>tr?tl:tr)+1),(int8_t)(row_floor(bl<br?bl:br)-1),shade,border);
        }
        /* Preserve this NULL-guarded shape: SDCC 4.5 allocates draw_run much
         * better with it present, while the GG caller passes NULL. */
        if(cols&&mid>cols[c].invz){cols[c].invz=mid;cols[c].wall_id=r->sid;cols[c].shade=shade;cols[c].top=clamp_u8i(tl,143u);cols[c].bottom=clamp_u8i(bl,143u);cols[c].top_step=clamp_s8((int16_t)(tr-tl),-7,7);cols[c].bottom_step=clamp_s8((int16_t)(br-bl),-7,7);}
        iq=(int16_t)(iq+step);
    }
}

static void insert_run(uint8_t idx,uint8_t *count){
    uint8_t i=*count;if(i>=TSPF_MAX_ACTIVE)return;
#if defined(__SDCC) && !TSPF_FORCE_C_MATERIALIZER
    /* Geometry/shade fast path writes complete name-table cells. Traverse
     * near->far so the first owner of a cell is already the final answer;
     * the assembly coverage mask then rejects hidden farther writes.
     * AO remains on the reference-style far->near C materializer for now. */
    if(g_tspf_appearance_mode<2u){
        while(i>0u&&g_runs[g_run_order[i-1u]].inv_mid<=g_runs[idx].inv_mid){
            g_run_order[i]=g_run_order[i-1u];--i;
        }
    }else
#endif
    {
        while(i>0u&&g_runs[g_run_order[i-1u]].inv_mid>g_runs[idx].inv_mid){
            g_run_order[i]=g_run_order[i-1u];--i;
        }
    }
    g_run_order[i]=idx;*count=(uint8_t)(*count+1u);
}
static void add_key(uint8_t key,const TSPState *s,uint8_t *count){
    uint8_t idx=*count;
    if(idx<TSPF_MAX_ACTIVE&&project_key(key,s,&g_runs[idx]))insert_run(idx,count);
}

void tsp_polar_render(const TSPState *s,uint16_t out_map[TSP_MAP_CELLS],TSPColumn cols[TSP_COLS]) BANKED {
    uint8_t gx,gy,lx,ly,recipe,base_id,cond_count,count=0,i;uint16_t gi,off;const uint8_t *p,*b;
    memset(g_corner_bearing_valid,0,sizeof(g_corner_bearing_valid));
    TSPF_SET_STAGE(1u);
#ifdef __SDCC
    tsp_polar_nt_begin_frame();
    (void)cols;
#else
    if(!g_map_ready)map_init(out_map);else restore_touched(out_map);
    if(cols)memset(cols,0,sizeof(TSPColumn)*TSP_COLS);
#endif
    TSPF_SET_STAGE(2u);
#if TSPF_PROFILE_HOOKS || !defined(__SDCC)
    g_tspf_selector_tests=0u;
#endif
#ifdef TSPF_E1M1_ROOM1
    {
        uint8_t mask[8],bi,bit;
        int16_t wx=(int16_t)(s->x_q4>>4),wy=(int16_t)(s->y_q4>>4);
        if(wx<E1X_WORLD_MIN_X||wy<E1X_WORLD_MIN_Y||wx>=E1X_WORLD_MAX_X||wy>=E1X_WORLD_MAX_Y)goto done;
        gx=(uint8_t)((wx-E1X_WORLD_MIN_X)>>3);
        gy=(uint8_t)((wy-E1X_WORLD_MIN_Y)>>3);
        e1pf_load_pvs(gx,gy,(uint8_t)((s->yaw+8u)>>4)&15u,mask);
        for(bi=0u;bi<8u;++bi){
            uint8_t m=mask[bi];
            for(bit=0u;bit<8u&&m;++bit){
                if(m&(uint8_t)(1u<<bit)){
                    uint8_t sid=(uint8_t)((bi<<3)+bit);
                    if(sid<E1PF_SEGMENT_COUNT)add_key(sid,s,&count);
                }
            }
        }
    }
#else
    gx=(uint8_t)((uint16_t)s->x_q4>>6);gy=(uint8_t)((uint16_t)s->y_q4>>6);if(gx>=48u||gy>=24u)goto done;gi=(uint16_t)(((uint16_t)gy<<5)+((uint16_t)gy<<4)+gx);recipe=k_tspf_recipe_grid[gi];if(recipe==0xffu)goto done;lx=(uint8_t)((uint16_t)s->x_q4&63u);ly=(uint8_t)((uint16_t)s->y_q4&63u);
#if defined(__SDCC) && TSPF_SCREEN_DEPTH_PLANE
    if(g_tspf_appearance_mode<2u&&s->yaw!=g_depth_yaw_cache){
        tsp_polar_depthplane_load(s->yaw,g_depth_nf_q7,g_depth_stepfac_q4);
        g_depth_yaw_cache=s->yaw;
    }
#endif
#if defined(__SDCC) && TSPF_LOCAL_PROJECTION
    if(gi!=g_proj_cached_gi)projection_load_cell(gx,gy,gi);
    g_proj_lx=lx;g_proj_ly=ly;
    tsp_polar_projection_eval_fast();
    if(g_proj_fallback_mask)projection_eval_fallback(s);
#endif
#ifndef TSPF_E1M1_ROOM1
    off=k_tspf_recipe_off[recipe];p=&k_tspf_recipe_stream[off];base_id=*p++;cond_count=*p++;b=&k_tspf_base_stream[k_tspf_base_off[base_id]];i=*b++;for(;i;--i)add_key(*b++,s,&count);
    for(i=0;i<cond_count;++i){uint8_t key=*p++,sel=*p++;if(selector_pass(sel,lx,ly))add_key(key,s,&count);}
#endif
#if TSPF_PROFILE_HOOKS || !defined(__SDCC)
    g_tspf_active_runs=count;
#endif
    TSPF_SET_STAGE(3u); /* one-byte indices are insertion-sorted far -> near */
    TSPF_SET_STAGE(4u);for(i=0;i<count;++i)draw_run(out_map,cols,&g_runs[g_run_order[i]],s);
done:
#ifdef __SDCC
    /* Restore only geometry-owned cells that disappeared this frame. */
    tsp_polar_nt_end_frame();
#endif
#if !defined(__SDCC)
    g_tspf_touched_cells=g_touched_count;
#elif TSPF_PROFILE_HOOKS
    g_tspf_touched_cells=0u;
#endif
    TSPF_SET_STAGE(0u);
}
