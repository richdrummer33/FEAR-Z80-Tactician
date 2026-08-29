/*
 * Dirty-region killer experiment.
 *
 * Phase 1 is the SAME host-baked on-rails patch stream used by the exploration
 * proof, but the VDP uploader is intentionally stupid: every visible 20x18
 * name-table word is written every frame.
 *
 * Controls:
 *   D-pad: permanently cancels the rail script and takes manual control.
 *   A/B: strafe. They do NOT cancel the rail script; if pressed while the
 *        script is running, we transparently switch from baked patches to the
 *        general Polar renderer and combine script motion + manual strafe.
 *
 * Once arbitrary input has made the pre-baked patch stream invalid, the ROM
 * stays on the general renderer. The full-table uploader remains active, so
 * dirty-region bookkeeping is bypassed for the entire diagnostic session.
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "polar_explore_script.h"
#include "polar_demo_patch_meta.h"

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

extern volatile uint8_t g_tspf_appearance_mode;

static uint8_t g_tile[32u];
static uint8_t g_rail_active;
static uint8_t g_dynamic_renderer;
static PolarExploreCursor g_explore;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_full(void);
void tsp_polar_demo_patch_apply(uint16_t patch);

static uint8_t shade_color(uint8_t shade){return shade==0u?C_FAR:(shade==1u?C_MID:C_NEAR);}
static void clear_tile(void){uint8_t i;for(i=0u;i<32u;++i)g_tile[i]=0u;}
static void paint_pixel(uint8_t x,uint8_t y,uint8_t color){
    uint8_t p,bit=(uint8_t)(0x80u>>x);uint8_t *row=g_tile+(uint16_t)y*4u;
    for(p=0u;p<4u;++p)if(color&(uint8_t)(1u<<p))row[p]|=bit;
}
static void emit_solid(uint16_t id,uint8_t color){
    uint8_t x,y;clear_tile();
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x)paint_pixel(x,y,color);
    set_bkg_4bpp_data(id,1u,g_tile);
}
static void emit_horizon(void){
    uint8_t x,y;clear_tile();
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x)paint_pixel(x,y,y==0u?C_BLACK:C_FLOOR);
    set_bkg_4bpp_data(TSP_TILE_HORIZON,1u,g_tile);
}
static uint8_t side_border(uint8_t border,uint8_t x){
    return (uint8_t)(((border&1u)&&x==0u)||((border&2u)&&x==7u));
}
static void emit_full(uint8_t shade,uint8_t cap,uint8_t border){
    uint8_t x,y,color=shade_color(shade);clear_tile();
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){
        uint8_t black=side_border(border,x);
        if(cap==TSP_CAP_TOP&&y==0u)black=1u;
        if(cap==TSP_CAP_BOTTOM&&y==7u)black=1u;
        paint_pixel(x,y,black?C_BLACK:color);
    }
    set_bkg_4bpp_data(TSP_TILE_FULL(shade,cap,border),1u,g_tile);
}
static void emit_edge(uint8_t shade,uint8_t oi,uint8_t si){
    uint8_t x,y,color=shade_color(shade);
    int8_t off=(int8_t)TSP_EDGE_OFF_MIN+(int8_t)oi;
    clear_tile();
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){
        int8_t line=(int8_t)(off+k_edge_lut[si][x]);
        uint8_t c=(int8_t)y<line?C_OUT:((int8_t)y==line?C_BLACK:color);
        paint_pixel(x,y,c);
    }
    set_bkg_4bpp_data(TSP_TILE_EDGE(shade,oi,si),1u,g_tile);
}
static void init_tiles(void){
    uint8_t s,c,b,o,m;
    emit_solid(TSP_TILE_CEILING,C_OUT);
    emit_solid(TSP_TILE_FLOOR,C_FLOOR);
    emit_horizon();
    for(s=0u;s<TSP_SHADE_COUNT;++s)
        for(c=0u;c<TSP_CAP_COUNT;++c)
            for(b=0u;b<TSP_BORDER_COUNT;++b)
                emit_full(s,c,b);
    for(s=0u;s<TSP_SHADE_COUNT;++s)
        for(o=0u;o<TSP_EDGE_OFF_COUNT;++o)
            for(m=0u;m<TSP_EDGE_SLOPE_COUNT;++m)
                emit_edge(s,o,m);
}

static uint8_t strafe_input(uint8_t pad){
    uint8_t input=0u;
    if(pad&J_B)input|=TSP_INPUT_STRAFE_LEFT;
    if(pad&J_A)input|=TSP_INPUT_STRAFE_RIGHT;
    return input;
}
static uint8_t dpad_input(uint8_t pad){
    uint8_t input=0u;
    if(pad&J_UP)input|=TSP_INPUT_UP;
    if(pad&J_DOWN)input|=TSP_INPUT_DOWN;
    if(pad&J_LEFT)input|=TSP_INPUT_LEFT;
    if(pad&J_RIGHT)input|=TSP_INPUT_RIGHT;
    return input;
}

static void switch_to_dynamic_renderer(void){
    if(g_dynamic_renderer)return;

    /* Patch playback does not maintain the conventional renderer's coverage
     * history. Reinitialize the authoritative map/lifetime state once, then
     * render the CURRENT camera position so manual takeover starts cleanly. */
    tsp_polar_nt_init();
    tsp_polar_renderer_reset();
    tsp_polar_render(&g_state,g_map,(TSPColumn *)0);
    g_dynamic_renderer=1u;
}

void main(void){
    DISPLAY_OFF;
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_palettes);
    init_tiles();

    tsp_reset(&g_state);
    polar_explore_cursor_reset(&g_explore);
    g_tspf_appearance_mode=0u;
    g_rail_active=1u;
    g_dynamic_renderer=0u;

    tsp_polar_nt_init();
    tsp_polar_demo_patch_apply(0u);
    g_patch_index=1u;
    tsp_polar_nt_upload_full();

    DISPLAY_ON;
    for(;;){
        uint8_t pad=joypad();
        uint8_t dpad=dpad_input(pad);
        uint8_t strafe=strafe_input(pad);
        uint8_t scripted=0u;
        uint8_t input;

        /* Any directional key is the explicit "give me the wheel" command.
         * It permanently stops scripted motion. */
        if(dpad){
            g_rail_active=0u;
            switch_to_dynamic_renderer();
        }

        if(g_rail_active)scripted=polar_explore_next(&g_explore);

        /* A/B do not interrupt the rails. They can still strafe. Because that
         * leaves the pre-baked trajectory, switch to the real renderer while
         * continuing to feed it the scripted control sequence. */
        if(strafe) switch_to_dynamic_renderer();

        if(g_dynamic_renderer){
            input=(uint8_t)(scripted|strafe);
            if(!g_rail_active)input=(uint8_t)(input|dpad);
            tsp_step(&g_state,input);
            tsp_polar_render(&g_state,g_map,(TSPColumn *)0);
        }else if(g_patch_index<POLAR_DEMO_PATCH_COUNT){
            /* Untouched on-rails path: exact same pre-baked patch sequence as
             * the original exploration proof. Only the uploader differs. */
            tsp_step(&g_state,scripted);
            tsp_polar_demo_patch_apply(g_patch_index);
            ++g_patch_index;
        }

        vsync();

        /* THE KILLER EXPERIMENT:
         * Never consult dirty min/max. Always rewrite every visible cell. */
        tsp_polar_nt_upload_full();
    }
}
