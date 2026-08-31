/*
 * Thick porthole / window proof ROM.
 *
 * Local bundle 0: an interior freestanding thick wall divider with a
 * non-traversable rectangular view aperture into another part of the room.
 * Local bundle 1: a thick exterior wall with a recessed porthole looking into
 * an inaccessible exterior court/lightwell.
 *
 * All geometry and multi-depth visibility are resolved by the host bake.
 * Runtime remains the same packet replay used by the streamed-room proof.
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "room_bundle_poc_meta.h"

#define C_BLACK 0u
#define C_CEILING 1u
#define C_FLOOR 2u

uint16_t g_map[TSP_MAP_CELLS];
volatile uint16_t g_porthole_status;
volatile uint16_t g_porthole_progress;
volatile uint16_t g_porthole_signature;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);

static uint8_t g_tile[32u];

/* Same solid-eight wall-light palette contract as the main lighting branch.
 * The porthole rooms themselves are currently baseline-lit; retaining the
 * shared palette makes this ROM a faithful packet-replay environment. */
static const palette_color_t k_palettes[32] = {
    RGB(0,0,0),RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),

    RGB(0,0,0),
    RGB(2,2,3),RGB(3,4,6),
    RGB(3,4,6),RGB(4,5,7),RGB(6,7,9),RGB(8,9,11),
    RGB(10,11,13),RGB(11,12,14),RGB(13,14,15),RGB(15,15,15),
    RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13)
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

static uint8_t play_route(uint8_t bundle,uint16_t progress_base){
    uint16_t frame;
    uint16_t count=tsp_room_bundle_generated_frames(bundle,0u,1u);
    if(!count)return 0u;
    for(frame=1u;frame<count;++frame){
        tsp_room_bundle_generated_apply_name(bundle,0u,1u,frame);
        vsync();
        tsp_room_bundle_generated_apply_tile(bundle,0u,1u,frame);
        tsp_polar_nt_upload_dirty();
        g_porthole_progress=(uint16_t)(progress_base+frame);
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

    g_porthole_status=0u;
    g_porthole_progress=0u;
    g_porthole_signature=0x504Fu; /* "PO" */

    tsp_polar_nt_init();

    /* Cold boot local bundle 0 from the shared canonical hidden seam. */
    tsp_room_bundle_generated_apply_tile(0u,0u,1u,0u);
    tsp_room_bundle_generated_load_canonical();
    tsp_polar_nt_upload_dirty();
    DISPLAY_ON;

    if(!play_route(0u,0u)){
        g_porthole_status=0xEE01u;
        for(;;)vsync();
    }
    g_porthole_status=1u;

    /* Bundle 0 ends on the exact canonical seam, so bundle 1 can begin
     * immediately without a geometry/runtime handoff layer. */
    if(!play_route(1u,1000u)){
        g_porthole_status=0xEE02u;
        for(;;)vsync();
    }

    g_porthole_status=0xCAFEu;
    g_porthole_progress=0xBEEFu;
    g_porthole_signature=0x574Eu; /* "WN" - window proof complete */
    for(;;)vsync();
}
