#include <stdint.h>
#include <gbdk/platform.h>
#include "e1m1_room1_core.h"

#ifndef E1_PROFILE_HOOKS
#define E1_PROFILE_HOOKS 0
#endif
#if E1_PROFILE_HOOKS
volatile uint8_t g_ts_prof_phase;
#define E1_PHASE(v) do { g_ts_prof_phase=(v); } while(0)
#else
#define E1_PHASE(v) ((void)0)
#endif

#define C_BLACK   0u
#define C_CEIL    1u
#define C_FLOOR   2u
#define C_FAR     3u
#define C_MID     4u
#define C_NEAR    5u

static const palette_color_t k_palette[16] = {
    RGB(0,0,0),
    RGB(1,1,2),
    RGB(2,2,3),
    RGB(3,3,4),
    RGB(5,5,6),
    RGB(8,8,9),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0)
};

/* Public names deliberately match the already-proven 20x18 direct uploader. */
E1Room1State g_e1m1_state;
uint16_t g_map[E1_MAP_CELLS];
uint16_t g_prev_map[E1_MAP_CELLS];
volatile uint16_t g_ts_dirty_words;
volatile uint16_t g_e1m1_loop_count;

static uint8_t g_tile[32u];
static uint8_t g_prev_pad;

void ts_upload_dirty_map_fast(void);

static uint8_t shade_color(uint8_t shade) {
    if(shade==0u)return C_FAR;
    if(shade==1u)return C_MID;
    return C_NEAR;
}

static void clear_tile(void) {
    uint8_t i;
    for(i=0u;i<32u;++i)g_tile[i]=0u;
}

static void paint_pixel(uint8_t x,uint8_t y,uint8_t color) {
    uint8_t plane;
    uint8_t bit=(uint8_t)(0x80u>>x);
    uint8_t *row=g_tile+(uint16_t)y*4u;
    for(plane=0u;plane<4u;++plane)
        if(color&(uint8_t)(1u<<plane))row[plane]|=bit;
}

static void emit_solid(uint16_t tile_id,uint8_t color) {
    uint8_t x,y;
    clear_tile();
    for(y=0u;y<8u;++y)
        for(x=0u;x<8u;++x)
            paint_pixel(x,y,color);
    set_bkg_4bpp_data(tile_id,1u,g_tile);
}

static void emit_horizon(void) {
    uint8_t x,y;
    clear_tile();
    for(y=0u;y<8u;++y)
        for(x=0u;x<8u;++x)
            paint_pixel(x,y,y==0u?C_BLACK:C_FLOOR);
    set_bkg_4bpp_data(E1_TILE_HORIZON,1u,g_tile);
}

static uint8_t side_border(uint8_t border,uint8_t x) {
    if((border&1u)&&x==0u)return 1u;
    if((border&2u)&&x==7u)return 1u;
    return 0u;
}

static void emit_full(uint8_t shade,uint8_t cap,uint8_t border) {
    uint8_t x,y,color=shade_color(shade);
    clear_tile();
    for(y=0u;y<8u;++y) {
        for(x=0u;x<8u;++x) {
            uint8_t black=side_border(border,x);
            if(cap==E1_CAP_TOP&&y==0u)black=1u;
            if(cap==E1_CAP_BOTTOM&&y==7u)black=1u;
            paint_pixel(x,y,black?C_BLACK:color);
        }
    }
    set_bkg_4bpp_data(E1_TILE_FULL(shade,cap,border),1u,g_tile);
}

static void init_tiles(void) {
    uint8_t shade,cap,border;
    emit_solid(E1_TILE_CEILING,C_CEIL);
    emit_solid(E1_TILE_FLOOR,C_FLOOR);
    emit_horizon();
    for(shade=0u;shade<E1_SHADE_COUNT;++shade)
        for(cap=0u;cap<E1_CAP_COUNT;++cap)
            for(border=0u;border<E1_BORDER_COUNT;++border)
                emit_full(shade,cap,border);
}

static void invalidate_map(void) {
    uint16_t i;
    for(i=0u;i<E1_MAP_CELLS;++i)g_prev_map[i]=0xffffu;
}

static uint8_t read_input(void) {
    uint8_t pad=joypad();
    uint8_t pressed=(uint8_t)(pad&(uint8_t)~g_prev_pad);
    uint8_t input=0u;
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

void main(void) {
    DISPLAY_OFF;
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,1u,k_palette);
    init_tiles();
    e1_room1_reset(&g_e1m1_state);
    invalidate_map();
    e1_room1_render(&g_e1m1_state,g_map);
    ts_upload_dirty_map_fast();
    g_e1m1_loop_count=0u;
    E1_PHASE(0u);
    DISPLAY_ON;

    for(;;) {
        uint8_t input;
        E1_PHASE(1u);
        input=read_input();
        e1_room1_step(&g_e1m1_state,input);
        E1_PHASE(2u);
        e1_room1_render(&g_e1m1_state,g_map);
        E1_PHASE(3u);
        vsync();
        E1_PHASE(4u);
        ts_upload_dirty_map_fast();
        E1_PHASE(5u);
        ++g_e1m1_loop_count;
    }
}
