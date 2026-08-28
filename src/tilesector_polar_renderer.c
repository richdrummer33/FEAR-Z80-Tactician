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

#define TSPF_MAX_ACTIVE 20u
#define TSPF_HORIZON 72
#define TSPF_NEAR_Z_Q4 (10<<4)
#define TSPF_FAR_Z_Q4  (127<<4)

#ifdef __SDCC
void tsp_polar_begin_map_fast(void);
void tsp_polar_surface_column_fast(void);
/* Explicit assembly bridge symbols. No C struct layout and no implicit
 * register-argument ABI: the Z80 materializer reads these exact globals. */
uint8_t g_polar_mat_col;
uint8_t g_polar_mat_shade;
uint8_t g_polar_mat_border;
int16_t g_polar_mat_top_l;
int16_t g_polar_mat_top_r;
int16_t g_polar_mat_bot_l;
int16_t g_polar_mat_bot_r;
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
static const uint8_t k_col_recip_q8[21] = {
    0,255,128,85,64,51,43,36,32,28,26,23,21,20,18,17,16,15,14,13,13
};

typedef struct PolarRun {
    uint8_t key_id;
    uint8_t sid;
    uint8_t v0;
    uint8_t v1;
    int16_t lo_q12;
    int16_t hi_q12;
    uint8_t x0;
    uint8_t x1;
    uint8_t inv0;
    uint8_t inv1;
    uint8_t inv_mid;
    uint8_t left_real;
    uint8_t right_real;
} PolarRun;

static PolarRun g_runs[TSPF_MAX_ACTIVE];
/* Original polar-field design: connected spans share authored corners.
 * Compute each corner bearing at most once per rendered update. 14 vertices
 * need only 28 bytes plus a 16-bit validity mask. */
static uint16_t g_corner_bearing_q12[14];
static uint16_t g_corner_bearing_valid;
#ifndef __SDCC
static uint8_t g_touched_bits[45];
static uint16_t g_touched_list[TSP_MAP_CELLS]; /* host oracle lifetime tracking */
static uint16_t g_touched_count;
static uint8_t g_map_ready;
#endif

static int8_t clamp_s8(int16_t v,int8_t lo,int8_t hi){if(v<lo)return lo;if(v>hi)return hi;return (int8_t)v;}
static uint8_t clamp_u8i(int16_t v,uint8_t hi){if(v<0)return 0;if(v>hi)return hi;return (uint8_t)v;}
static int16_t shr_signed(int16_t v,uint8_t n){return v>=0?(int16_t)(v>>n):(int16_t)-(((-v)>>n));}
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
    out[k_row_base[row]+col]=word;
#else
    uint16_t idx=k_row_base[row]+col;uint8_t *b=&g_touched_bits[idx>>3];uint8_t m=(uint8_t)(1u<<(idx&7u));
    if(!(*b&m)){*b|=m;g_touched_list[g_touched_count++]=(uint16_t)(((uint16_t)row<<8)|col);}
    out[idx]=word;
#endif
}

void tsp_polar_renderer_reset(void) BANKED {
#ifndef __SDCC
    g_map_ready=0u;g_touched_count=0u;memset(g_touched_bits,0,sizeof(g_touched_bits));
#endif
    TSPF_SET_STAGE(0u);
#if TSPF_PROFILE_HOOKS || !defined(__SDCC)
    g_tspf_active_runs=0u;g_tspf_selector_tests=0u;g_tspf_touched_cells=0u;
#endif
}

static uint8_t ao_class(uint8_t vid){return (uint8_t)((k_tspf_ao[vid>>2]>>((vid&3u)*2u))&3u);}
static uint8_t selector_pass(uint8_t sid,uint8_t lx,uint8_t ly){
    int16_t v=(int16_t)k_tspf_sel_a[sid]*(int16_t)lx+(int16_t)k_tspf_sel_b[sid]*(int16_t)ly+k_tspf_sel_c[sid];
    TSPF_SELECTOR_HIT();
    return (uint8_t)(((v>=0)?1u:0u)^k_tspf_sel_inv[sid]);
}

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
static uint16_t bearing_vertex_q12(uint8_t vid,const TSPState *s){
    uint16_t mask=(uint16_t)((uint16_t)1u<<vid);
    if(!(g_corner_bearing_valid&mask)){
        g_corner_bearing_q12[vid]=bearing_q12(
            (int16_t)((int16_t)k_tspf_vx[vid]<<4)-s->x_q4,
            (int16_t)((int16_t)k_tspf_vy[vid]<<4)-s->y_q4);
        g_corner_bearing_valid|=mask;
    }
    return g_corner_bearing_q12[vid];
}
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
    int8_t nx=k_tspf_nx_q5[sid],ny=k_tspf_ny_q5[sid];
    /* Exact cardinal-wall identities. For +/-32 Q5 normals the original
     * multiply/shift expression reduces to one Q4 coordinate subtraction. */
    if(ny==0&&(nx==32||nx==-32)){
        int16_t wall=(int16_t)((int16_t)k_tspf_vx[anchor_vid]<<4);
        return nx>0?(int16_t)(wall-s->x_q4):(int16_t)(s->x_q4-wall);
    }
    if(nx==0&&(ny==32||ny==-32)){
        int16_t wall=(int16_t)((int16_t)k_tspf_vy[anchor_vid]<<4);
        return ny>0?(int16_t)(wall-s->y_q4):(int16_t)(s->y_q4-wall);
    }
    {
        int16_t xi=(int16_t)(s->x_q4>>4),yi=(int16_t)(s->y_q4>>4);uint8_t fx=(uint8_t)(s->x_q4&15),fy=(uint8_t)(s->y_q4&15);
        int16_t dx=(int16_t)k_tspf_vx[anchor_vid]-xi,dy=(int16_t)k_tspf_vy[anchor_vid]-yi;
        int16_t whole=(int16_t)nx*dx+(int16_t)ny*dy;
        int16_t frac=(int16_t)nx*fx+(int16_t)ny*fy;
        return (int16_t)(shr_signed(whole,1)-shr_signed(frac,5));
    }
}
static uint8_t inv_at_invd(uint8_t sid,uint8_t invd,uint16_t world_bearing,int16_t rel){
    uint8_t bi=(uint8_t)(world_bearing>>4);int8_t sn=(int8_t)k_tspf_sin_q7[bi],cs=(int8_t)k_tspf_sin_q7[(uint8_t)(bi+64u)],nx=k_tspf_nx_q5[sid],ny=k_tspf_ny_q5[sid];int16_t dot;uint16_t q,sec;
    /* Exact cardinal-normal shortcuts: (+/-32 * trig) >> 5 == +/-trig.
     * The final magnitude discards normal sign, so no multiply is needed. */
    if(ny==0&&(nx==32||nx==-32)) dot=cs;
    else if(nx==0&&(ny==32||ny==-32)) dot=sn;
    else dot=shr_signed((int16_t)((int16_t)nx*cs+(int16_t)ny*sn),5);
    if(dot<0)dot=(int16_t)-dot;if(dot>127)dot=127;
    q=((uint16_t)invd*(uint16_t)dot+64u)>>7;sec=k_tspf_sec_q7[(uint16_t)(rel<0?-rel:rel)];q=(q*sec+64u)>>7;return (uint8_t)(q>255u?255u:q);
}
static uint8_t shade_for(uint8_t inv,int8_t bias){int8_t s;if(inv>=82u)s=2;else if(inv>=46u)s=1;else s=0;s=(int8_t)(s+bias);if(s<0)s=0;if(s>2)s=2;return (uint8_t)s;}

static uint8_t project_key(uint8_t keyid,const TSPState *s,PolarRun *r){
    uint16_t w=k_tspf_keys[keyid];uint8_t sid=(uint8_t)(w&31u),v0=(uint8_t)((w>>5)&15u),v1=(uint8_t)((w>>9)&15u);uint16_t a0,a1,len,yawq;int16_t st,en,lo,hi;uint8_t x0,x1,invd;
    a0=bearing_vertex_q12(v0,s);
    a1=bearing_vertex_q12(v1,s);
    len=(uint16_t)((a1-a0)&4095u);if(len==0u||len>=2048u)return 0u;yawq=(uint16_t)s->yaw<<4;st=signed_q12((uint16_t)(a0-yawq));en=(int16_t)(st+(int16_t)len);
    while(en<-512){st=(int16_t)(st+4096);en=(int16_t)(en+4096);}while(st>512){st=(int16_t)(st-4096);en=(int16_t)(en-4096);}
    lo=st<-512?-512:st;hi=en>512?512:en;if(hi<=lo)return 0u;x0=angle_x(lo);x1=angle_x(hi);if(x1<x0){uint8_t t=x0;x0=x1;x1=t;}if(x1==x0&&x1<159u)++x1;
    r->key_id=keyid;r->sid=sid;r->v0=v0;r->v1=v1;r->lo_q12=lo;r->hi_q12=hi;r->x0=x0;r->x1=x1;r->left_real=(uint8_t)(lo==st);r->right_real=(uint8_t)(hi==en);
    invd=inv_for_dq4(wall_d_q4(sid,k_tspf_seg_anchor[sid],s));
    r->inv0=inv_at_invd(sid,invd,(uint16_t)(yawq+lo)&4095u,lo);
    r->inv1=inv_at_invd(sid,invd,(uint16_t)(yawq+hi)&4095u,hi);
    r->inv_mid=(uint8_t)(((uint16_t)r->inv0+r->inv1)>>1);return 1u;
}

static uint16_t edge_entry(uint8_t shade,int16_t local_left,int8_t slope,uint8_t bottom){
    uint16_t attr=0;uint8_t mag;int8_t off;if(bottom){local_left=(int16_t)(7-local_left);slope=(int8_t)-slope;attr=(uint16_t)(TSP_ATTR_FLIPY|TSP_ATTR_PALETTE);}if(slope<0){mag=(uint8_t)(-slope);local_left=(int16_t)(local_left-mag);attr|=TSP_ATTR_FLIPX;}else mag=(uint8_t)slope;if(mag>=TSP_EDGE_SLOPE_COUNT)mag=TSP_EDGE_SLOPE_COUNT-1u;off=clamp_s8(local_left,TSP_EDGE_OFF_MIN,(int8_t)(TSP_EDGE_OFF_MIN+TSP_EDGE_OFF_COUNT-1));return (uint16_t)(TSP_TILE_EDGE(shade,(uint8_t)(off-TSP_EDGE_OFF_MIN),mag)|attr);
}
static int8_t row_floor(int16_t y){return y>=0?(int8_t)(y>>3):(int8_t)-(((-y)+7)>>3);}
static void draw_edge(uint16_t *out,uint8_t col,int16_t yl,int16_t yr,uint8_t shade,uint8_t bottom){
    int8_t slope=clamp_s8((int16_t)(yr-yl),-7,7),r0=row_floor(yl<yr?yl:yr),r1=row_floor(yl>yr?yl:yr),r;if(r0<0)r0=0;if(r1>=(int8_t)TSP_ROWS)r1=(int8_t)(TSP_ROWS-1u);
    for(r=r0;r<=r1;++r)put_cell(out,(uint8_t)r,col,edge_entry(shade,(int16_t)(yl-((int16_t)r<<3)),slope,bottom));
}
static void draw_full(uint16_t *out,uint8_t col,int8_t first,int8_t last,uint8_t shade,uint8_t border){
    int8_t r;if(first<0)first=0;if(last>=(int8_t)TSP_ROWS)last=(int8_t)(TSP_ROWS-1u);if(first>last)return;for(r=first;r<=last;++r)put_cell(out,(uint8_t)r,col,TSP_TILE_FULL(shade,TSP_CAP_NONE,border));
}
static void draw_run(uint16_t *out,TSPColumn *cols,const PolarRun *r){
    uint8_t c0=(uint8_t)(r->x0>>3),c1=(uint8_t)(r->x1>>3),n,c,profile=k_tspf_profile[r->sid];int16_t iq,step;if(c0>=TSP_COLS)c0=TSP_COLS-1;if(c1>=TSP_COLS)c1=TSP_COLS-1;if(c1<c0)return;n=(uint8_t)(c1-c0+1u);iq=(int16_t)r->inv0<<6;step=(int16_t)(((int16_t)r->inv1-(int16_t)r->inv0)*(int16_t)k_col_recip_q8[n]);step=shr_signed(step,2);
    for(c=c0;c<=c1;++c){uint8_t invl=(uint8_t)clamp_u8i((iq+32)>>6,255u),invr=(uint8_t)clamp_u8i((iq+step+32)>>6,255u),mid=(uint8_t)(((uint16_t)invl+invr)>>1),hl=(uint8_t)(invl>>1),hr=(uint8_t)(invr>>1);int16_t tl=(int16_t)(TSPF_HORIZON-hl),tr=(int16_t)(TSPF_HORIZON-hr),bl=(int16_t)(TSPF_HORIZON+hl),br=(int16_t)(TSPF_HORIZON+hr);uint8_t border=0,shade,edge_shade;
        if(c==c0&&r->left_real) border|=1u;
        if(c==c1&&r->right_real) border|=2u;
        shade=g_tspf_appearance_mode?shade_for(mid,k_tspf_shade_bias[r->sid]):1u;edge_shade=shade;
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
#ifdef __SDCC
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

static void insert_run(PolarRun *r,uint8_t *count){
    uint8_t i=*count;if(i>=TSPF_MAX_ACTIVE)return;while(i>0u&&g_runs[i-1u].inv_mid>r->inv_mid){g_runs[i]=g_runs[i-1u];--i;}g_runs[i]=*r;*count=(uint8_t)(*count+1u);
}
static void add_key(uint8_t key,const TSPState *s,uint8_t *count){PolarRun r;if(project_key(key,s,&r))insert_run(&r,count);}

void tsp_polar_render(const TSPState *s,uint16_t out_map[TSP_MAP_CELLS],TSPColumn cols[TSP_COLS]) BANKED {
    uint8_t gx,gy,lx,ly,recipe,base_id,cond_count,count=0,i;uint16_t gi,off;const uint8_t *p,*b;
    g_corner_bearing_valid=0u;
    TSPF_SET_STAGE(1u);
#ifdef __SDCC
    tsp_polar_begin_map_fast();
    (void)cols;
#else
    if(!g_map_ready)map_init(out_map);else restore_touched(out_map);
    if(cols)memset(cols,0,sizeof(TSPColumn)*TSP_COLS);
#endif
    TSPF_SET_STAGE(2u);
#if TSPF_PROFILE_HOOKS || !defined(__SDCC)
    g_tspf_selector_tests=0u;
#endif
gx=(uint8_t)((uint16_t)s->x_q4>>6);gy=(uint8_t)((uint16_t)s->y_q4>>6);if(gx>=48u||gy>=24u)goto done;gi=(uint16_t)(((uint16_t)gy<<5)+((uint16_t)gy<<4)+gx);recipe=k_tspf_recipe_grid[gi];if(recipe==0xffu)goto done;lx=(uint8_t)((uint16_t)s->x_q4&63u);ly=(uint8_t)((uint16_t)s->y_q4&63u);
    off=k_tspf_recipe_off[recipe];p=&k_tspf_recipe_stream[off];base_id=*p++;cond_count=*p++;b=&k_tspf_base_stream[k_tspf_base_off[base_id]];i=*b++;for(;i;--i)add_key(*b++,s,&count);
    for(i=0;i<cond_count;++i){uint8_t key=*p++,sel=*p++;if(selector_pass(sel,lx,ly))add_key(key,s,&count);}
#if TSPF_PROFILE_HOOKS || !defined(__SDCC)
    g_tspf_active_runs=count;
#endif
    TSPF_SET_STAGE(3u); /* runs are insertion-sorted far -> near during collection */
    TSPF_SET_STAGE(4u);for(i=0;i<count;++i)draw_run(out_map,cols,&g_runs[i]);
done:
#if !defined(__SDCC)
    g_tspf_touched_cells=g_touched_count;
#elif TSPF_PROFILE_HOOKS
    g_tspf_touched_cells=0u;
#endif
    TSPF_SET_STAGE(0u);
}
