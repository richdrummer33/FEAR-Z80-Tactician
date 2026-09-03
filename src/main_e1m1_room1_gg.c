#include <stdint.h>
#include <string.h>
#include <gbdk/platform.h>
#include "e1m1_room1_world.h"
#include "tilesector_polar.h"

#ifndef E1_PROFILE_HOOKS
#define E1_PROFILE_HOOKS 0
#endif
#if E1_PROFILE_HOOKS
volatile uint8_t g_ts_prof_phase;
#define E1_PHASE(v) do { g_ts_prof_phase=(v); } while(0)
#else
#define E1_PHASE(v) ((void)0)
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

/* Keep these symbols stable for the existing Gearsystem proof harness. */
E1Room1State g_e1m1_state;
uint16_t g_map[TSP_MAP_CELLS];
volatile uint16_t g_e1m1_loop_count;

static uint8_t g_tile[32u];
static uint8_t g_prev_pad;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);

static uint8_t shade_color(uint8_t shade){return shade==0u?C_FAR:(shade==1u?C_MID:C_NEAR);}
static void clear_tile(void){uint8_t i;for(i=0u;i<32u;++i)g_tile[i]=0u;}
static void paint_pixel(uint8_t x,uint8_t y,uint8_t color){uint8_t p,bit=(uint8_t)(0x80u>>x);uint8_t *row=g_tile+(uint16_t)y*4u;for(p=0u;p<4u;++p)if(color&(uint8_t)(1u<<p))row[p]|=bit;}
static void emit_solid(uint16_t id,uint8_t color){uint8_t x,y;clear_tile();for(y=0u;y<8u;++y)for(x=0u;x<8u;++x)paint_pixel(x,y,color);set_bkg_4bpp_data(id,1u,g_tile);}
static void emit_horizon(void){uint8_t x,y;clear_tile();for(y=0u;y<8u;++y)for(x=0u;x<8u;++x)paint_pixel(x,y,y==0u?C_BLACK:C_FLOOR);set_bkg_4bpp_data(TSP_TILE_HORIZON,1u,g_tile);}
static uint8_t side_border(uint8_t border,uint8_t x){return (uint8_t)(((border&1u)&&x==0u)||((border&2u)&&x==7u));}
static void emit_full(uint8_t shade,uint8_t cap,uint8_t border){uint8_t x,y,color=shade_color(shade);clear_tile();for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){uint8_t black=side_border(border,x);if(cap==TSP_CAP_TOP&&y==0u)black=1u;if(cap==TSP_CAP_BOTTOM&&y==7u)black=1u;paint_pixel(x,y,black?C_BLACK:color);}set_bkg_4bpp_data(TSP_TILE_FULL(shade,cap,border),1u,g_tile);}
static void emit_edge(uint8_t shade,uint8_t oi,uint8_t si){uint8_t x,y,color=shade_color(shade);int8_t off=(int8_t)TSP_EDGE_OFF_MIN+(int8_t)oi;clear_tile();for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){int8_t line=(int8_t)(off+k_edge_lut[si][x]);uint8_t cc=(int8_t)y<line?C_OUT:((int8_t)y==line?C_BLACK:color);paint_pixel(x,y,cc);}set_bkg_4bpp_data(TSP_TILE_EDGE(shade,oi,si),1u,g_tile);}
static void init_tiles(void){uint8_t s,c,b,o,m;emit_solid(TSP_TILE_CEILING,C_OUT);emit_solid(TSP_TILE_FLOOR,C_FLOOR);emit_horizon();for(s=0u;s<TSP_SHADE_COUNT;++s)for(c=0u;c<TSP_CAP_COUNT;++c)for(b=0u;b<TSP_BORDER_COUNT;++b)emit_full(s,c,b);for(s=0u;s<TSP_SHADE_COUNT;++s)for(o=0u;o<TSP_EDGE_OFF_COUNT;++o)for(m=0u;m<TSP_EDGE_SLOPE_COUNT;++m)emit_edge(s,o,m);}

static uint8_t read_input(void){
    uint8_t pad=joypad(),pressed=(uint8_t)(pad&(uint8_t)~g_prev_pad),input=0u;
    if(pad&J_UP)input|=E1_INPUT_UP;
    if(pad&J_DOWN)input|=E1_INPUT_DOWN;
    if(pad&J_LEFT)input|=E1_INPUT_LEFT;
    if(pad&J_RIGHT)input|=E1_INPUT_RIGHT;
    if(pad&J_B)input|=E1_INPUT_STRAFE_LEFT;
    if(pad&J_A)input|=E1_INPUT_STRAFE_RIGHT;
    if(pressed&J_START)input|=E1_INPUT_SPEED;
    g_prev_pad=pad;
    return input;
}

static void render_room1(void){
    TSPState p;
    memset(&p,0,sizeof(p));
    p.x_q4=g_e1m1_state.x_q4;
    p.y_q4=g_e1m1_state.y_q4;
    p.z_q4=g_e1m1_state.z_q4;
    p.yaw=g_e1m1_state.yaw;
    p.speed_scale=g_e1m1_state.speed_scale;
    tsp_polar_render(&p,g_map,(TSPColumn *)0);
}

void main(void){
    DISPLAY_OFF;
    __WRITE_VDP_REG(VDP_R2,R2_MAP_0x3800);
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_palettes);
    init_tiles();

    e1_room1_reset(&g_e1m1_state);
    tsp_polar_renderer_reset();
    g_tspf_appearance_mode=0u;
    tsp_polar_nt_init();
    render_room1();
    tsp_polar_nt_upload_dirty();
    g_e1m1_loop_count=0u;
    E1_PHASE(0u);
    DISPLAY_ON;

    for(;;){
        uint8_t input;
        E1_PHASE(1u);
        input=read_input();
        e1_room1_step(&g_e1m1_state,input);
        E1_PHASE(2u);
        render_room1();
        E1_PHASE(3u);
        vsync();
        E1_PHASE(4u);
        tsp_polar_nt_upload_dirty();
        E1_PHASE(5u);
        ++g_e1m1_loop_count;
    }
}
