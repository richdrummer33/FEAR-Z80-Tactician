/* Fresh Game Gear entrypoint for the Adaptive Polar Field renderer.
 * Legacy main_tilesector_gg.c remains untouched and is not linked by this target.
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#if defined(TSPF_E1M1_EDGE_VOCAB) && TSPF_E1M1_EDGE_VOCAB
#include "e1m1_edge_vocab.h"
#endif

#define C_BLACK 0u
#define C_OUT   1u
#define C_FLOOR 2u
#define C_FAR   3u
#define C_MID   4u
#define C_NEAR  5u

static const palette_color_t k_palettes[32] = {
    RGB(0,0,0),RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),
    RGB(0,0,0),RGB(2,2,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0)
};
static const int8_t k_edge_lut[8][8] = {
    {0,0,0,0,0,0,0,0},{0,0,0,0,1,1,1,1},{0,0,1,1,1,1,2,2},{0,0,1,1,2,2,3,3},
    {0,1,1,2,2,3,3,4},{0,1,1,2,3,4,4,5},{0,1,2,3,3,4,5,6},{0,1,2,3,4,5,6,7}
};

TSPState g_state;
uint16_t g_map[TSP_MAP_CELLS];
#if defined(TSPF_OPTIMIZED_MAP)
/* Verification/layout marker and zero-hook loop counter.  The capture harness
 * uses these to decode the custom TSPState without enabling profile hooks. */
volatile uint8_t g_opt_state_layout=1u;
volatile uint16_t g_opt_loop_count;
#endif
static uint8_t g_tile[32u];
static uint8_t g_prev_pad;

#if TSPF_PROFILE_HOOKS
volatile uint8_t g_ts_prof_phase;
volatile uint16_t g_ts_loop_count;
volatile uint16_t g_ts_dirty_words;
#define TSPF_PHASE(v) (g_ts_prof_phase=(v))
#define TSPF_LOOP_INC() (++g_ts_loop_count)
#else
#define TSPF_PHASE(v) ((void)0)
#define TSPF_LOOP_INC() ((void)0)
#endif

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);
void tsp_polar_nt_upload_dirty_budgeted(void);

/* Cooperative VBlank publisher.
 *
 * The VBL ISR itself only raises a byte flag.  Rendering reaches safe yield
 * points (currently every FULL coarse column) and services the flag from normal
 * code, so VDP writes never run inside the interrupt dispatcher.  The GG V
 * interrupt fires at V counter C0h; while VCOUNTER>=C0h the VDP is outside the
 * effective display area and VRAM writes need no active-display wait states.
 *
 * The uploader intentionally drains only six dirty rows per VBlank.  A worst
 * case row is 20 name-table words / 40 VDP bytes, so this leaves substantial
 * margin inside the ~4.3 ms post-effective-area safe interval even when the
 * renderer notices VBlank a little late. */
volatile uint8_t g_ts_vblank_pending;
#if TSPF_PROFILE_HOOKS
volatile uint16_t g_ts_vblank_bursts;
volatile uint16_t g_ts_vblank_missed;
#endif

static void tsp_vblank_mark(void) NONBANKED {
    g_ts_vblank_pending=1u;
}

void tsp_polar_service_vblank(void) NONBANKED {
    if(!g_ts_vblank_pending) return;
    g_ts_vblank_pending=0u;
    if(VCOUNTER<0xC0u){
#if TSPF_PROFILE_HOOKS
        ++g_ts_vblank_missed;
#endif
        return;
    }
    tsp_polar_nt_upload_dirty_budgeted();
#if TSPF_PROFILE_HOOKS
    ++g_ts_vblank_bursts;
#endif
}

static uint8_t shade_color(uint8_t shade){return shade==0u?C_FAR:(shade==1u?C_MID:C_NEAR);}
static void clear_tile(void){uint8_t i;for(i=0;i<32u;++i)g_tile[i]=0u;}
static void paint_pixel(uint8_t x,uint8_t y,uint8_t color){uint8_t p,bit=(uint8_t)(0x80u>>x);uint8_t *row=g_tile+(uint16_t)y*4u;for(p=0;p<4u;++p)if(color&(uint8_t)(1u<<p))row[p]|=bit;}
static void emit_solid(uint16_t id,uint8_t color){uint8_t x,y;clear_tile();for(y=0;y<8u;++y)for(x=0;x<8u;++x)paint_pixel(x,y,color);set_bkg_4bpp_data(id,1u,g_tile);}
static void emit_horizon(void){uint8_t x,y;clear_tile();for(y=0;y<8u;++y)for(x=0;x<8u;++x)paint_pixel(x,y,y==0u?C_BLACK:C_FLOOR);set_bkg_4bpp_data(TSP_TILE_HORIZON,1u,g_tile);}
static uint8_t side_border(uint8_t border,uint8_t x){return (uint8_t)(((border&1u)&&x==0u)||((border&2u)&&x==7u));}
static void emit_full(uint8_t shade,uint8_t cap,uint8_t border){uint8_t x,y,color=shade_color(shade);clear_tile();for(y=0;y<8u;++y)for(x=0;x<8u;++x){uint8_t black=side_border(border,x);if(cap==TSP_CAP_TOP&&y==0u)black=1u;if(cap==TSP_CAP_BOTTOM&&y==7u)black=1u;paint_pixel(x,y,black?C_BLACK:color);}set_bkg_4bpp_data(TSP_TILE_FULL(shade,cap,border),1u,g_tile);}
static void emit_edge_at(uint16_t id,uint8_t shade,uint8_t oi,uint8_t si,uint8_t border){uint8_t x,y,color=shade_color(shade);int8_t off=(int8_t)TSP_EDGE_OFF_MIN+(int8_t)oi;clear_tile();for(y=0;y<8u;++y)for(x=0;x<8u;++x){int8_t line=(int8_t)(off+k_edge_lut[si][x]);uint8_t c=(int8_t)y<line?C_OUT:((int8_t)y==line?C_BLACK:color);if(side_border(border,x))c=C_BLACK;paint_pixel(x,y,c);}set_bkg_4bpp_data(id,1u,g_tile);}
static void emit_edge(uint8_t shade,uint8_t oi,uint8_t si){emit_edge_at(TSP_TILE_EDGE(shade,oi,si),shade,oi,si,0u);}
static void init_tiles(void){uint8_t s,c,b,o,m;emit_solid(TSP_TILE_CEILING,C_OUT);emit_solid(TSP_TILE_FLOOR,C_FLOOR);emit_horizon();for(s=0;s<TSP_SHADE_COUNT;++s)for(c=0;c<TSP_CAP_COUNT;++c)for(b=0;b<TSP_BORDER_COUNT;++b)emit_full(s,c,b);
#if defined(TSPF_E1M1_EDGE_VOCAB) && TSPF_E1M1_EDGE_VOCAB
    /* Repack the 384 semantic EDGE tiles into 240 unique physical patterns,
     * then spend the recovered slots on left/right EDGE+vertical-border
     * combinations. Border=3 (one-column faces) deliberately remains a later
     * thin-face rung. 435 patterns stay below the 0x3800 name-table boundary. */
    for(s=0;s<TSP_SHADE_COUNT;++s)for(o=0;o<TSP_EDGE_OFF_COUNT;++o)for(m=0;m<TSP_EDGE_SLOPE_COUNT;++m){uint8_t sem=(uint8_t)(o*8u+m);uint16_t id=(uint16_t)(TSP_TILE_EDGE_COMPACT_BASE+(uint16_t)s*TSP_TILE_EDGE_COMPACT_SHADE_STRIDE+g_tsp_edge_unique_idx_home[sem]);emit_edge_at(id,s,o,m,0u);}
    for(b=1u;b<3u;++b)for(o=0;o<TSP_EDGE_OFF_COUNT;++o)for(m=0;m<TSP_EDGE_SLOPE_COUNT;++m){uint8_t sem=(uint8_t)(o*8u+m);uint16_t id=(uint16_t)(TSP_TILE_EDGE_BORDER_BASE+(b==1u?g_tsp_edge_border_b1_home[sem]:g_tsp_edge_border_b2_home[sem]));emit_edge_at(id,1u,o,m,b);}
#else
    for(s=0;s<TSP_SHADE_COUNT;++s)for(o=0;o<TSP_EDGE_OFF_COUNT;++o)for(m=0;m<TSP_EDGE_SLOPE_COUNT;++m)emit_edge(s,o,m);
#endif
}
static uint16_t upload_dirty_map(void){
    tsp_polar_nt_upload_dirty();
#if TSPF_PROFILE_HOOKS
    return g_ts_dirty_words;
#else
    return 0u;
#endif
}

#if TSPF_TRACE_INPUT
/* Deterministic scripted input for frame-timeline measurement. A profile that
 * depends on whatever the pad happened to be doing is not comparable between
 * builds, so the trace replaces the pad entirely and the same trace drives every
 * build being compared. It is a build option, not a runtime one: the shipping
 * path below is untouched. */
extern const uint8_t k_tsp_trace[];
extern const uint16_t k_tsp_trace_len;
static uint16_t g_trace_i=0u;
static uint8_t read_input(void){
    uint8_t v=k_tsp_trace[g_trace_i];
    if(++g_trace_i>=k_tsp_trace_len)g_trace_i=0u;
    return v;
}
#else
static uint8_t read_input(void){
    uint8_t pad=joypad(),pressed=(uint8_t)(pad&(uint8_t)~g_prev_pad),input=0u;
    if(pad&J_UP)input|=TSP_INPUT_UP;if(pad&J_DOWN)input|=TSP_INPUT_DOWN;if(pad&J_LEFT)input|=TSP_INPUT_LEFT;if(pad&J_RIGHT)input|=TSP_INPUT_RIGHT;
    if(pad&J_B)input|=TSP_INPUT_STRAFE_LEFT;if(pad&J_A)input|=TSP_INPUT_STRAFE_RIGHT;
    /* Profiling toggle: START cycles core -> shade -> shade+AO. Holding B+START
     * jumps directly to geometry-only; holding A+START jumps directly to AO. */
    if(pressed&J_START){if(pad&J_B)g_tspf_appearance_mode=0u;else if(pad&J_A)g_tspf_appearance_mode=2u;else {++g_tspf_appearance_mode;if(g_tspf_appearance_mode>2u)g_tspf_appearance_mode=0u;}}
    g_prev_pad=pad;return input;
}
#endif

void main(void){
    /* Generated 4-bpp patterns stay below the 512-pattern hardware ceiling. Keep the
     * visible name table in the non-overlapping 0x3800 region. The row uploader
     * targets the matching 0x38xx addresses. */
    DISPLAY_OFF;__WRITE_VDP_REG(VDP_R2,R2_MAP_0x3800);HIDE_SPRITES;SET_BORDER_COLOR(C_BLACK);set_bkg_palette(0u,2u,k_palettes);init_tiles();
    tsp_reset(&g_state);tsp_polar_renderer_reset();g_tspf_appearance_mode=TSPF_DEFAULT_APPEARANCE;tsp_polar_nt_init();tsp_polar_render(&g_state,g_map,(TSPColumn *)0);upload_dirty_map();
    g_ts_vblank_pending=0u;
#if TSPF_PROFILE_HOOKS
    g_ts_vblank_bursts=0u;g_ts_vblank_missed=0u;
#endif
    disable_interrupts();add_VBL(tsp_vblank_mark);enable_interrupts();
#if defined(TSPF_OPTIMIZED_MAP)
    g_opt_loop_count=0u;
#endif
#if TSPF_PROFILE_HOOKS
    g_ts_prof_phase=0u;g_ts_loop_count=0u;g_ts_dirty_words=0u;
#endif
    DISPLAY_ON;
    for(;;){
        uint8_t input;
        /* Do not quantize logical updates behind a blocking vsync().  The
         * column materializer services VBlank while rendering, and these two
         * boundary checks cover frames with no visible FULL columns. */
        TSPF_PHASE(1u);tsp_polar_service_vblank();input=read_input();tsp_step(&g_state,input);
        TSPF_PHASE(2u);tsp_polar_render(&g_state,g_map,(TSPColumn *)0);
        TSPF_PHASE(3u);tsp_polar_service_vblank();
        TSPF_PHASE(4u);
        TSPF_PHASE(5u);TSPF_LOOP_INC();
#if defined(TSPF_OPTIMIZED_MAP)
        ++g_opt_loop_count;
#endif
    }
}
