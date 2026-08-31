/*
 * Reversible streamed-room proof.
 *
 * The GG runtime knows only deterministic node descriptors plus portal-local
 * baked packet routes. Geometry, visibility and cast lighting remain host-side.
 *
 *   root 0->1
 *   child 0->1
 *   child 1->0
 *   breadcrumb pop/identity check
 *   root 1->0
 *
 * Every route begins and ends at the exact same canonical seam/cache state.
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "tilesector_world_stream_poc.h"
#include "room_bundle_poc_meta.h"

#define C_BLACK 0u
#define C_CEILING 1u
#define C_FLOOR 2u
#define STREAM_SEED UINT32_C(0xC0FFEE42)

uint16_t g_map[TSP_MAP_CELLS];
volatile uint16_t g_room_bundle_stream_status;
volatile uint16_t g_room_bundle_stream_progress;
volatile uint16_t g_room_bundle_stream_signature;
volatile uint8_t g_room_bundle_root_asset;
volatile uint8_t g_room_bundle_child_asset;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);

static uint8_t g_tile[32u];

/*
 * Match the mature baked-light semantic palette exactly.
 * Palette 0 = ambient semantic shades.
 * Palette 1 = one-step brighter lit transform, with indices 8..12 carrying
 * ambient-side pixels for mixed shadow-boundary tiles.
 */
static const palette_color_t k_palettes[32] = {
    RGB(0,0,0),RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),
    RGB(0,0,0),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),RGB(10,11,13),
    RGB(0,0,0),RGB(0,0,0),RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0)
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

static uint8_t same_node(const TSPStreamNodeDesc *a,const TSPStreamNodeDesc *b){
    return (uint8_t)(a->key==b->key &&
                     a->module_kind==b->module_kind &&
                     a->asset_variant==b->asset_variant &&
                     a->rotation==b->rotation &&
                     a->logical_floor_q4==b->logical_floor_q4);
}
static uint8_t bundle_for_node(const TSPStreamNodeDesc *n){
    return (uint8_t)(n->asset_variant&1u);
}

static void play_route(uint8_t bundle,uint8_t entry,uint8_t exit_portal,
                       uint16_t first_frame,uint16_t progress_base){
    uint16_t frame;
    uint16_t count=tsp_room_bundle_generated_frames(bundle,entry,exit_portal);
    if(!count){
        g_room_bundle_stream_status=0xEE01u;
        return;
    }
    for(frame=first_frame;frame<count;++frame){
        tsp_room_bundle_generated_apply_name(bundle,entry,exit_portal,frame);
        vsync();
        tsp_room_bundle_generated_apply_tile(bundle,entry,exit_portal,frame);
        tsp_polar_nt_upload_dirty();
        g_room_bundle_stream_progress=(uint16_t)(progress_base+frame);
    }
}

void main(void){
    TSPStreamWorld world;
    TSPStreamNodeDesc root,child;
    uint8_t root_bundle,child_bundle;

    DISPLAY_OFF;
    __WRITE_VDP_REG(VDP_R2,R2_MAP_0x3800);
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_palettes);
    init_base_tiles();

    g_room_bundle_stream_status=0u;
    g_room_bundle_stream_progress=0u;
    g_room_bundle_stream_signature=0xB21Du;
    g_room_bundle_root_asset=0xffu;
    g_room_bundle_child_asset=0xffu;

    tsp_stream_reset(&world,STREAM_SEED,0u);
    root=world.current;
    root_bundle=bundle_for_node(&root);
    g_room_bundle_root_asset=root_bundle;

    tsp_polar_nt_init();

    /* Cold boot uses canonical frame zero from the selected root's forward
     * route. Normal seam-to-seam transitions begin at frame one. */
    tsp_room_bundle_generated_apply_tile(root_bundle,0u,1u,0u);
    tsp_room_bundle_generated_load_canonical();
    tsp_polar_nt_upload_dirty();
    DISPLAY_ON;

    play_route(root_bundle,0u,1u,1u,0u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();
    g_room_bundle_stream_status=1u;

    if(!tsp_stream_enter(&world,0u,0u)){
        g_room_bundle_stream_status=0xEE02u;
        for(;;)vsync();
    }
    child=world.current;
    child_bundle=bundle_for_node(&child);
    g_room_bundle_child_asset=child_bundle;
    if(child_bundle==root_bundle){
        /* This fixed PoC seed is intentionally chosen to exercise both the
         * tight/inset-lit and wide/portal-shadow bundle classes. */
        g_room_bundle_stream_status=0xEE04u;
        for(;;)vsync();
    }

    play_route(child_bundle,0u,1u,1u,1000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();
    g_room_bundle_stream_status=2u;

    play_route(child_bundle,1u,0u,1u,2000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();
    g_room_bundle_stream_status=3u;

    if(!tsp_stream_back(&world)||!same_node(&world.current,&root)){
        g_room_bundle_stream_status=0xEE03u;
        for(;;)vsync();
    }
    g_room_bundle_stream_status=4u;

    play_route(root_bundle,1u,0u,1u,3000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();
    g_room_bundle_stream_status=5u;
    g_room_bundle_stream_progress=5000u;

    for(;;)vsync();
}
