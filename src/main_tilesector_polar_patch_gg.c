/*
 * Game Gear proof ROM for the host-compiled Polar transition architecture.
 *
 * It deliberately does NOT link the Polar projection/visibility/raster path.
 * The scripted camera motion is a player-like exploration path, but each final
 * 20x18 name-table change is replayed from a host-baked exact patch.
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "polar_explore_script.h"
#include "polar_demo_patch_meta.h"
#include "polar_demo_tiles_meta.h"

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
volatile uint16_t g_patch_index;
volatile uint8_t g_tspf_appearance_mode;

#if TSPF_PROFILE_HOOKS
volatile uint8_t g_ts_prof_phase;
volatile uint16_t g_ts_loop_count;
volatile uint16_t g_ts_dirty_words;
#define PATCH_PHASE(v) (g_ts_prof_phase=(v))
#define PATCH_LOOP_INC() (++g_ts_loop_count)
#else
#define PATCH_PHASE(v) ((void)0)
#define PATCH_LOOP_INC() ((void)0)
#endif

static uint8_t g_tile[32u];
static PolarExploreCursor g_explore;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);
void tsp_polar_demo_patch_apply(uint16_t patch);
void tsp_polar_demo_tiles_init(void);

static uint16_t upload_dirty_map(void){
    tsp_polar_nt_upload_dirty();
#if TSPF_PROFILE_HOOKS
    return g_ts_dirty_words;
#else
    return 0u;
#endif
}

static uint8_t shade_color(uint8_t shade){return shade==0u?C_FAR:(shade==1u?C_MID:C_NEAR);}
static void clear_tile(void){uint8_t i;for(i=0;i<32u;++i)g_tile[i]=0u;}
static void paint_pixel(uint8_t x,uint8_t y,uint8_t color){
    uint8_t p,bit=(uint8_t)(0x80u>>x);uint8_t *row=g_tile+(uint16_t)y*4u;
    for(p=0;p<4u;++p)if(color&(uint8_t)(1u<<p))row[p]|=bit;
}
static void emit_solid(uint16_t id,uint8_t color){
    uint8_t x,y;clear_tile();
    for(y=0;y<8u;++y)for(x=0;x<8u;++x)paint_pixel(x,y,color);
    set_bkg_4bpp_data(id,1u,g_tile);
}
static void emit_horizon(void){
    uint8_t x,y;clear_tile();
    for(y=0;y<8u;++y)for(x=0;x<8u;++x)paint_pixel(x,y,y==0u?C_BLACK:C_FLOOR);
    set_bkg_4bpp_data(TSP_TILE_HORIZON,1u,g_tile);
}
static uint8_t side_border(uint8_t border,uint8_t x){
    return (uint8_t)(((border&1u)&&x==0u)||((border&2u)&&x==7u));
}
static void emit_full(uint8_t shade,uint8_t cap,uint8_t border){
    uint8_t x,y,color=shade_color(shade);clear_tile();
    for(y=0;y<8u;++y)for(x=0;x<8u;++x){
        uint8_t black=side_border(border,x);
        if(cap==TSP_CAP_TOP&&y==0u)black=1u;
        if(cap==TSP_CAP_BOTTOM&&y==7u)black=1u;
        paint_pixel(x,y,black?C_BLACK:color);
    }
    set_bkg_4bpp_data(TSP_TILE_FULL(shade,cap,border),1u,g_tile);
}
static void emit_edge(uint8_t shade,uint8_t oi,uint8_t si){
    uint8_t x,y,color=shade_color(shade);int8_t off=(int8_t)TSP_EDGE_OFF_MIN+(int8_t)oi;clear_tile();
    for(y=0;y<8u;++y)for(x=0;x<8u;++x){
        int8_t line=(int8_t)(off+k_edge_lut[si][x]);
        uint8_t c=(int8_t)y<line?C_OUT:((int8_t)y==line?C_BLACK:color);
        paint_pixel(x,y,c);
    }
    set_bkg_4bpp_data(TSP_TILE_EDGE(shade,oi,si),1u,g_tile);
}
static void init_tiles(void){
    uint8_t s,c,b,o,m;
    emit_solid(TSP_TILE_CEILING,C_OUT);emit_solid(TSP_TILE_FLOOR,C_FLOOR);emit_horizon();
    for(s=0;s<TSP_SHADE_COUNT;++s)for(c=0;c<TSP_CAP_COUNT;++c)for(b=0;b<TSP_BORDER_COUNT;++b)emit_full(s,c,b);
    for(s=0;s<TSP_SHADE_COUNT;++s)for(o=0;o<TSP_EDGE_OFF_COUNT;++o)for(m=0;m<TSP_EDGE_SLOPE_COUNT;++m)emit_edge(s,o,m);
}

void main(void){
    DISPLAY_OFF;HIDE_SPRITES;SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_palettes);tsp_polar_demo_tiles_init();

    tsp_reset(&g_state);
    polar_explore_cursor_reset(&g_explore);
    g_tspf_appearance_mode=0u;
    tsp_polar_nt_init();

    /* Patch zero converts the static ceiling/horizon/floor base to the exact
     * initial host-oracle view. The init routine already marked the whole
     * visible table dirty, so the first upload remains simple and deterministic. */
    tsp_polar_demo_patch_apply(0u);
    g_patch_index=1u;
    (void)upload_dirty_map();

#if TSPF_PROFILE_HOOKS
    g_ts_prof_phase=0u;g_ts_loop_count=0u;g_ts_dirty_words=0u;
#endif
    DISPLAY_ON;
    for(;;){
        uint8_t input;
        PATCH_PHASE(1u);
        input=polar_explore_next(&g_explore);
        if(g_patch_index<POLAR_DEMO_PATCH_COUNT){
            /* Scripted proof only: real runtime state/event lookup comes after
             * the patch executor itself is measured and proven exact. */
            tsp_step(&g_state,input);
            PATCH_PHASE(2u);
            tsp_polar_demo_patch_apply(g_patch_index);
            ++g_patch_index;
        }else PATCH_PHASE(2u);

        PATCH_PHASE(3u);vsync();
        PATCH_PHASE(4u);
#if TSPF_PROFILE_HOOKS
        g_ts_dirty_words=upload_dirty_map();
#else
        (void)upload_dirty_map();
#endif
        PATCH_PHASE(5u);PATCH_LOOP_INC();
    }
}
