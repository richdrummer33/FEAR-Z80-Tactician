/*
 * Game Gear flicker visual-validation ROM.
 *
 * Replays the real spooky-room baked packet stream to logical frame 96, then
 * holds the same geometry while cycling the production palette states:
 * normal/bright -> dim -> off -> normal/bright.
 *
 * This is deliberately a validation harness, not a gameplay path. It provides
 * stable live-RAM phase hooks for exact screenshot capture while exercising
 * the same GG CRAM writes used by the streamed-room flicker implementation.
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "room_bundle_poc_meta.h"

#define C_BLACK 0u
#define C_CEILING 1u
#define C_FLOOR 2u
#define CAPTURE_SCENE_FRAME 64u
#define HOLD_FRAMES 120u

uint16_t g_map[TSP_MAP_CELLS];
volatile uint16_t g_flicker_capture_status;
volatile uint16_t g_flicker_capture_phase;
volatile uint16_t g_flicker_capture_edges;
volatile uint16_t g_flicker_capture_scene_frame;
volatile uint8_t g_flicker_capture_level;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);

static uint8_t g_tile[32u];

static const palette_color_t k_palettes[32] = {
    RGB(0,0,0),RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),

    RGB(0,0,0),
    RGB(2,2,3),RGB(3,4,6),
    RGB(3,4,6),RGB(4,5,7),RGB(6,7,9),RGB(8,9,11),
    RGB(10,11,13),RGB(11,12,14),RGB(13,14,15),RGB(15,15,15),
    RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13)
};

static const palette_color_t k_lit_dim[16] = {
    RGB(0,0,0),
    RGB(1,1,3),RGB(2,3,4),
    RGB(2,3,5),RGB(3,4,6),RGB(4,5,7),RGB(5,6,8),
    RGB(6,7,9),RGB(8,9,11),RGB(10,11,13),RGB(11,12,14),
    RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13)
};

static const palette_color_t k_lit_off[16] = {
    RGB(0,0,0),
    RGB(1,1,3),RGB(2,2,3),
    RGB(3,4,6),RGB(3,4,6),RGB(4,5,7),RGB(4,5,7),
    RGB(6,7,9),RGB(6,7,9),RGB(8,9,11),RGB(8,9,11),
    RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13)
};

static uint8_t g_last_level=0xffu;

static void clear_tile(void){
    uint8_t i;
    for(i=0u;i<32u;++i)g_tile[i]=0u;
}
static void paint_pixel(uint8_t x,uint8_t y,uint8_t color){
    uint8_t p,bit=(uint8_t)(0x80u>>x);
    uint8_t *row=g_tile+(uint16_t)y*4u;
    for(p=0u;p<4u;++p)
        if(color&(uint8_t)(1u<<p))row[p]|=bit;
}
static void emit_solid(uint16_t id,uint8_t color){
    uint8_t x,y;
    clear_tile();
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x)paint_pixel(x,y,color);
    set_bkg_4bpp_data(id,1u,g_tile);
}
static void emit_horizon(void){
    uint8_t x,y;
    clear_tile();
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x)
        paint_pixel(x,y,y==0u?C_BLACK:C_FLOOR);
    set_bkg_4bpp_data(TSP_TILE_HORIZON,1u,g_tile);
}
static void init_base_tiles(void){
    emit_solid(TSP_TILE_CEILING,C_CEILING);
    emit_solid(TSP_TILE_FLOOR,C_FLOOR);
    emit_horizon();
}

static void set_lit_palette_level(uint8_t level){
    const palette_color_t *p;
    if(level>2u)level=2u;
    if(level==g_last_level)return;
    p=level==0u?k_lit_off:
      (level==1u?k_lit_dim:&k_palettes[16]);
    set_palette(1u,1u,p);
    if(g_last_level!=0xffu)++g_flicker_capture_edges;
    g_last_level=level;
    g_flicker_capture_level=level;
}

static void hold_phase(uint16_t phase,uint8_t level){
    uint16_t i;
    set_lit_palette_level(level);
    /* Ensure the CRAM write has reached a displayed frame before exposing the
     * phase marker that the native runner waits on. */
    vsync();
    g_flicker_capture_phase=phase;
    for(i=0u;i<HOLD_FRAMES;++i)vsync();
}

static uint8_t replay_to_capture_frame(void){
    uint16_t frame;
    uint16_t count=tsp_room_bundle_generated_frames(0u,0u,1u);
    if(count<=CAPTURE_SCENE_FRAME)return 0u;

    tsp_room_bundle_generated_apply_tile(0u,0u,1u,0u);
    tsp_room_bundle_generated_load_canonical();
    tsp_polar_nt_upload_dirty();
    DISPLAY_ON;

    for(frame=1u;frame<=CAPTURE_SCENE_FRAME;++frame){
        tsp_room_bundle_generated_apply_name(0u,0u,1u,frame);
        vsync();
        tsp_room_bundle_generated_apply_tile(0u,0u,1u,frame);
        tsp_polar_nt_upload_dirty();
        g_flicker_capture_scene_frame=frame;
    }
    return 1u;
}

void main(void){
    DISPLAY_OFF;
    __WRITE_VDP_REG(VDP_R2,R2_MAP_0x3800);
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_palettes);
    init_base_tiles();

    g_flicker_capture_status=0u;
    g_flicker_capture_phase=0u;
    g_flicker_capture_edges=0u;
    g_flicker_capture_scene_frame=0u;
    g_flicker_capture_level=2u;
    g_last_level=2u;

    tsp_polar_nt_init();
    if(!replay_to_capture_frame()){
        g_flicker_capture_status=0xEE01u;
        for(;;)vsync();
    }

    /* One normal/in-between state is enough for stills; the second normal
     * closes the video loop so the return from darkness is visible. */
    hold_phase(1u,2u); /* NORMAL / BRIGHT */
    hold_phase(2u,1u); /* DIM */
    hold_phase(3u,0u); /* OFF */
    hold_phase(4u,2u); /* NORMAL restored */

    if(g_flicker_capture_edges<3u){
        g_flicker_capture_status=0xEE02u;
        for(;;)vsync();
    }

    g_flicker_capture_status=0xCAFEu;
    g_flicker_capture_phase=5u;
    for(;;)vsync();
}
