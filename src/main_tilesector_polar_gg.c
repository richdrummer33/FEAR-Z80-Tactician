/* Fresh Game Gear entrypoint for the Adaptive Polar Field renderer.
 * Legacy main_tilesector_gg.c remains untouched and is not linked by this target.
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"

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
static TSPColumn g_cols[TSP_COLS];
uint16_t g_map[TSP_MAP_CELLS];
uint16_t g_prev_map[TSP_MAP_CELLS];
static uint8_t g_tile[32u];
static uint8_t g_prev_pad;

volatile uint8_t g_ts_prof_phase;
volatile uint16_t g_ts_loop_count;
volatile uint16_t g_ts_dirty_words;

void ts_upload_dirty_map_fast(void);

static uint8_t shade_color(uint8_t shade){return shade==0u?C_FAR:(shade==1u?C_MID:C_NEAR);}
static void clear_tile(void){uint8_t i;for(i=0;i<32u;++i)g_tile[i]=0u;}
static void paint_pixel(uint8_t x,uint8_t y,uint8_t color){uint8_t p,bit=(uint8_t)(0x80u>>x);uint8_t *row=g_tile+(uint16_t)y*4u;for(p=0;p<4u;++p)if(color&(uint8_t)(1u<<p))row[p]|=bit;}
static void emit_solid(uint16_t id,uint8_t color){uint8_t x,y;clear_tile();for(y=0;y<8u;++y)for(x=0;x<8u;++x)paint_pixel(x,y,color);set_bkg_4bpp_data(id,1u,g_tile);}
static void emit_horizon(void){uint8_t x,y;clear_tile();for(y=0;y<8u;++y)for(x=0;x<8u;++x)paint_pixel(x,y,y==0u?C_BLACK:C_FLOOR);set_bkg_4bpp_data(TSP_TILE_HORIZON,1u,g_tile);}
static uint8_t side_border(uint8_t border,uint8_t x){return (uint8_t)(((border&1u)&&x==0u)||((border&2u)&&x==7u));}
static void emit_full(uint8_t shade,uint8_t cap,uint8_t border){uint8_t x,y,color=shade_color(shade);clear_tile();for(y=0;y<8u;++y)for(x=0;x<8u;++x){uint8_t black=side_border(border,x);if(cap==TSP_CAP_TOP&&y==0u)black=1u;if(cap==TSP_CAP_BOTTOM&&y==7u)black=1u;paint_pixel(x,y,black?C_BLACK:color);}set_bkg_4bpp_data(TSP_TILE_FULL(shade,cap,border),1u,g_tile);}
static void emit_edge(uint8_t shade,uint8_t oi,uint8_t si){uint8_t x,y,color=shade_color(shade);int8_t off=(int8_t)TSP_EDGE_OFF_MIN+(int8_t)oi;clear_tile();for(y=0;y<8u;++y)for(x=0;x<8u;++x){int8_t line=(int8_t)(off+k_edge_lut[si][x]);uint8_t c=(int8_t)y<line?C_OUT:((int8_t)y==line?C_BLACK:color);paint_pixel(x,y,c);}set_bkg_4bpp_data(TSP_TILE_EDGE(shade,oi,si),1u,g_tile);}
static void init_tiles(void){uint8_t s,c,b,o,m;emit_solid(TSP_TILE_CEILING,C_OUT);emit_solid(TSP_TILE_FLOOR,C_FLOOR);emit_horizon();for(s=0;s<TSP_SHADE_COUNT;++s)for(c=0;c<TSP_CAP_COUNT;++c)for(b=0;b<TSP_BORDER_COUNT;++b)emit_full(s,c,b);for(s=0;s<TSP_SHADE_COUNT;++s)for(o=0;o<TSP_EDGE_OFF_COUNT;++o)for(m=0;m<TSP_EDGE_SLOPE_COUNT;++m)emit_edge(s,o,m);}
static void invalidate_map(void){uint16_t i;for(i=0;i<TSP_MAP_CELLS;++i)g_prev_map[i]=0xffffu;}
static uint16_t upload_dirty_map(void){ts_upload_dirty_map_fast();return g_ts_dirty_words;}

static uint8_t read_input(void){
    uint8_t pad=joypad(),pressed=(uint8_t)(pad&(uint8_t)~g_prev_pad),input=0u;
    if(pad&J_UP)input|=TSP_INPUT_UP;if(pad&J_DOWN)input|=TSP_INPUT_DOWN;if(pad&J_LEFT)input|=TSP_INPUT_LEFT;if(pad&J_RIGHT)input|=TSP_INPUT_RIGHT;
    if(pad&J_B)input|=TSP_INPUT_STRAFE_LEFT;if(pad&J_A)input|=TSP_INPUT_STRAFE_RIGHT;
    /* Profiling toggle: START cycles core -> shade -> shade+AO. Holding B+START
     * jumps directly to geometry-only; holding A+START jumps directly to AO. */
    if(pressed&J_START){if(pad&J_B)g_tspf_appearance_mode=0u;else if(pad&J_A)g_tspf_appearance_mode=2u;else {++g_tspf_appearance_mode;if(g_tspf_appearance_mode>2u)g_tspf_appearance_mode=0u;}}
    g_prev_pad=pad;return input;
}

void main(void){
    DISPLAY_OFF;HIDE_SPRITES;SET_BORDER_COLOR(C_BLACK);set_bkg_palette(0u,2u,k_palettes);init_tiles();
    tsp_reset(&g_state);tsp_polar_renderer_reset();g_tspf_appearance_mode=0u;invalidate_map();tsp_polar_render(&g_state,g_map,g_cols);upload_dirty_map();
    g_ts_prof_phase=0u;g_ts_loop_count=0u;g_ts_dirty_words=0u;DISPLAY_ON;
    for(;;){uint8_t input;g_ts_prof_phase=1u;input=read_input();tsp_step(&g_state,input);g_ts_prof_phase=2u;tsp_polar_render(&g_state,g_map,g_cols);g_ts_prof_phase=3u;vsync();g_ts_prof_phase=4u;g_ts_dirty_words=upload_dirty_map();g_ts_prof_phase=5u;++g_ts_loop_count;}
}
