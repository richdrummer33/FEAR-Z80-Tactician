/*
 * First visible streamed-room proof.
 *
 * No projection, visibility or room geometry exists on the Game Gear side.
 * The ROM replays two independently host-baked room bundles through their
 * shared canonical S-shaped seam:
 *
 *   seam -> Room A -> seam/cache canonicalization
 *        -> Room B -> seam/cache canonicalization
 *
 * The generated dispatcher accepts only bundle-local frame numbers.
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "room_bundle_poc_meta.h"

#define C_BLACK 0u
#define C_CEILING 1u
#define C_FLOOR 2u

uint16_t g_map[TSP_MAP_CELLS];
volatile uint16_t g_room_bundle_stream_status;
volatile uint16_t g_room_bundle_stream_signature;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);

static uint8_t g_tile[32u];

static const palette_color_t k_palette[16] = {
    RGB(0,0,0), RGB(1,1,3), RGB(2,2,3), RGB(3,4,6),
    RGB(6,7,9), RGB(10,11,13), RGB(0,0,0), RGB(0,0,0),
    RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0),
    RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0)
};

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

static void play_bundle(uint8_t bundle,uint16_t first_frame){
    uint16_t frame,count=tsp_room_bundle_generated_frames(bundle);
    for(frame=first_frame;frame<count;++frame){
        tsp_room_bundle_generated_apply_name(bundle,frame);
        vsync();
        tsp_room_bundle_generated_apply_tile(bundle,frame);
        tsp_polar_nt_upload_dirty();
    }
}

void main(void){
    DISPLAY_OFF;
    __WRITE_VDP_REG(VDP_R2,R2_MAP_0x3800);
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,1u,k_palette);
    init_base_tiles();

    g_room_bundle_stream_status=0u;
    g_room_bundle_stream_signature=0xB21Du;

    tsp_polar_nt_init();

    /* Frame zero was baked from a fresh canonical cache. Load its exact
     * dynamic seam patterns, then install the canonical name table while the
     * display is still off. */
    tsp_room_bundle_generated_apply_tile(0u,0u);
    tsp_room_bundle_generated_load_canonical();
    tsp_polar_nt_upload_dirty();

    DISPLAY_ON;

    /* Bundle zero starts at frame one because frame zero is already visible. */
    play_bundle(0u,1u);
    g_room_bundle_stream_status=1u;

    /* Bundle one frame zero is the exact same canonical seam. Replaying its
     * canonical tile loads is harmless and makes predecessor-independence
     * explicit in the runtime trace. */
    play_bundle(1u,0u);
    g_room_bundle_stream_status=2u;

    for(;;)vsync();
}
