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
#include "tilesector_room_catalog_poc.h"
#include "room_bundle_poc_meta.h"

#define C_BLACK 0u
#define C_CEILING 1u
#define C_FLOOR 2u
#define STREAM_SEED UINT32_C(0xC0FFEE42)
#define SPLIT_STREAM_SEED UINT32_C(0x00000023)
#define STAIR_STREAM_SEED UINT32_C(0x00000010)

uint16_t g_map[TSP_MAP_CELLS];
volatile uint16_t g_room_bundle_stream_status;
volatile uint16_t g_room_bundle_stream_progress;
volatile uint16_t g_room_bundle_stream_signature;
volatile uint8_t g_room_bundle_root_asset;
volatile uint8_t g_room_bundle_child_asset;
volatile uint8_t g_room_bundle_split_left_asset;
volatile uint8_t g_room_bundle_split_right_asset;
volatile uint8_t g_room_bundle_stair_child_asset;
volatile int16_t g_room_bundle_stair_floor_before_q4;
volatile int16_t g_room_bundle_stair_floor_after_q4;

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
    TSPStreamNodeDesc split,left_child,right_child,left_again;
    TSPStreamNodeDesc stair,stair_child;
    TSPRoomCatalogChoice root_choice,child_choice;
    TSPRoomCatalogChoice split_left_choice,split_right_choice;
    TSPRoomCatalogChoice left_child_choice,right_child_choice;
    TSPRoomCatalogChoice stair_choice,stair_child_choice;
    uint8_t root_bundle,child_bundle,left_bundle,right_bundle,stair_child_bundle;

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
    g_room_bundle_split_left_asset=0xffu;
    g_room_bundle_split_right_asset=0xffu;
    g_room_bundle_stair_child_asset=0xffu;
    g_room_bundle_stair_floor_before_q4=0;
    g_room_bundle_stair_floor_after_q4=0;

    tsp_stream_reset(&world,STREAM_SEED,0u);
    root=world.current;
    if(!tsp_room_catalog_choose(STREAM_SEED,&root,0u,&root_choice)){
        g_room_bundle_stream_status=0xEE30u;
        for(;;)vsync();
    }
    root_bundle=root_choice.bundle_id;
    g_room_bundle_root_asset=root_bundle;

    tsp_polar_nt_init();

    /* Cold boot uses canonical frame zero from the selected root route.
     * Normal seam-to-seam transitions begin at frame one. */
    tsp_room_bundle_generated_apply_tile(root_bundle,
                                         root_choice.entry_portal,
                                         root_choice.exit_portal,0u);
    tsp_room_bundle_generated_load_canonical();
    tsp_polar_nt_upload_dirty();
    DISPLAY_ON;

    play_route(root_bundle,root_choice.entry_portal,root_choice.exit_portal,1u,0u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();
    g_room_bundle_stream_status=1u;

    if(!tsp_stream_enter(&world,0u,0u)){
        g_room_bundle_stream_status=0xEE02u;
        for(;;)vsync();
    }
    child=world.current;
    if(!tsp_room_catalog_choose(STREAM_SEED,&child,0u,&child_choice)){
        g_room_bundle_stream_status=0xEE31u;
        for(;;)vsync();
    }
    child_bundle=child_choice.bundle_id;
    g_room_bundle_child_asset=child_bundle;
    if(child_bundle==root_bundle){
        g_room_bundle_stream_status=0xEE04u;
        for(;;)vsync();
    }

    play_route(child_bundle,child_choice.entry_portal,child_choice.exit_portal,1u,1000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();
    g_room_bundle_stream_status=2u;

    play_route(child_bundle,child_choice.exit_portal,child_choice.entry_portal,1u,2000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();
    g_room_bundle_stream_status=3u;

    if(!tsp_stream_back(&world)||!same_node(&world.current,&root)){
        g_room_bundle_stream_status=0xEE03u;
        for(;;)vsync();
    }
    g_room_bundle_stream_status=4u;

    play_route(root_bundle,root_choice.exit_portal,root_choice.entry_portal,1u,3000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();
    g_room_bundle_stream_status=5u;
    g_room_bundle_stream_progress=5000u;

    /*
     * Split proof. Seed 0x20 deterministically resolves its root to T_SPLIT.
     * Authored portal 0 is the parent/entry seam; authored portals 1 and 2
     * correspond to topology exit indices 0 (left) and 1 (right).
     */
    tsp_stream_reset(&world,SPLIT_STREAM_SEED,0u);
    split=world.current;
    if(split.module_kind!=TSP_MODULE_T_SPLIT){
        g_room_bundle_stream_status=0xEE10u;
        for(;;)vsync();
    }
    if(!tsp_room_catalog_choose(SPLIT_STREAM_SEED,&split,0u,&split_left_choice)||
       !tsp_room_catalog_choose(SPLIT_STREAM_SEED,&split,1u,&split_right_choice)||
       split_left_choice.bundle_id!=2u||split_right_choice.bundle_id!=2u){
        g_room_bundle_stream_status=0xEE32u;
        for(;;)vsync();
    }

    /* Enter the split from its parent side and choose left. */
    play_route(2u,split_left_choice.entry_portal,split_left_choice.exit_portal,1u,6000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();

    if(!tsp_stream_enter(&world,0u,0u)){
        g_room_bundle_stream_status=0xEE11u;
        for(;;)vsync();
    }
    left_child=world.current;
    if(!tsp_room_catalog_choose(SPLIT_STREAM_SEED,&left_child,0u,&left_child_choice)){
        g_room_bundle_stream_status=0xEE33u;
        for(;;)vsync();
    }
    left_bundle=left_child_choice.bundle_id;
    g_room_bundle_split_left_asset=left_bundle;

    play_route(left_bundle,left_child_choice.entry_portal,left_child_choice.exit_portal,1u,7000u);
    play_route(left_bundle,left_child_choice.exit_portal,left_child_choice.entry_portal,1u,8000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();

    if(!tsp_stream_back(&world)||!same_node(&world.current,&split)){
        g_room_bundle_stream_status=0xEE12u;
        for(;;)vsync();
    }

    /* We are visually back at portal 1. Cross the junction directly to the
     * sibling portal; both branch mouths are visible during this route. */
    play_route(2u,split_left_choice.exit_portal,split_right_choice.exit_portal,1u,9000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();

    if(!tsp_stream_enter(&world,1u,0u)){
        g_room_bundle_stream_status=0xEE13u;
        for(;;)vsync();
    }
    right_child=world.current;
    if(!tsp_room_catalog_choose(SPLIT_STREAM_SEED,&right_child,0u,&right_child_choice)){
        g_room_bundle_stream_status=0xEE34u;
        for(;;)vsync();
    }
    right_bundle=right_child_choice.bundle_id;
    g_room_bundle_split_right_asset=right_bundle;
    if(right_child.key==left_child.key){
        g_room_bundle_stream_status=0xEE14u;
        for(;;)vsync();
    }

    play_route(right_bundle,right_child_choice.entry_portal,right_child_choice.exit_portal,1u,10000u);
    play_route(right_bundle,right_child_choice.exit_portal,right_child_choice.entry_portal,1u,11000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();

    if(!tsp_stream_back(&world)||!same_node(&world.current,&split)){
        g_room_bundle_stream_status=0xEE15u;
        for(;;)vsync();
    }

    /* Return from right branch to the original parent portal. */
    play_route(2u,split_right_choice.exit_portal,split_right_choice.entry_portal,1u,12000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();

    /* Re-select left after visiting right: deterministic branch identity must
     * reproduce exactly, not merely choose the same visual bundle class. */
    if(!tsp_stream_enter(&world,0u,0u)){
        g_room_bundle_stream_status=0xEE16u;
        for(;;)vsync();
    }
    left_again=world.current;
    if(!same_node(&left_again,&left_child)){
        g_room_bundle_stream_status=0xEE17u;
        for(;;)vsync();
    }
    if(!tsp_stream_back(&world)||!same_node(&world.current,&split)){
        g_room_bundle_stream_status=0xEE18u;
        for(;;)vsync();
    }

    g_room_bundle_stream_status=10u;
    g_room_bundle_stream_progress=15000u;

    /*
     * Vertical proof. Seed 0x10 deterministically resolves to an UP_RIGHT
     * quarter-stair root. Bundle 3 is its authored stepped visual module.
     */
    tsp_stream_reset(&world,STAIR_STREAM_SEED,1u);
    stair=world.current;
    if(stair.module_kind!=TSP_MODULE_STAIR_QUARTER_UP_RIGHT){
        g_room_bundle_stream_status=0xEE20u;
        for(;;)vsync();
    }
    if(!tsp_room_catalog_choose(STAIR_STREAM_SEED,&stair,0u,&stair_choice)||
       stair_choice.bundle_id!=3u){
        g_room_bundle_stream_status=0xEE35u;
        for(;;)vsync();
    }
    g_room_bundle_stair_floor_before_q4=stair.logical_floor_q4;

    play_route(3u,stair_choice.entry_portal,stair_choice.exit_portal,1u,16000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();

    if(!tsp_stream_enter(&world,0u,1u)){
        g_room_bundle_stream_status=0xEE21u;
        for(;;)vsync();
    }
    stair_child=world.current;
    g_room_bundle_stair_floor_after_q4=stair_child.logical_floor_q4;
    if(stair_child.logical_floor_q4!=(int16_t)(stair.logical_floor_q4+TSP_MODULE_STAIR_RISE_Q4)){
        g_room_bundle_stream_status=0xEE22u;
        for(;;)vsync();
    }
    if(stair_child.rotation!=(uint8_t)((stair.rotation+1u)&3u)){
        g_room_bundle_stream_status=0xEE23u;
        for(;;)vsync();
    }

    /* The raised child reuses an ordinary floor-zero local bake. Its logical
     * +4 elevation exists only in the world descriptor: the canonical seam
     * rebases the visual module automatically. */
    if(!tsp_room_catalog_choose(STAIR_STREAM_SEED,&stair_child,0u,&stair_child_choice)){
        g_room_bundle_stream_status=0xEE36u;
        for(;;)vsync();
    }
    stair_child_bundle=stair_child_choice.bundle_id;
    g_room_bundle_stair_child_asset=stair_child_bundle;
    play_route(stair_child_bundle,stair_child_choice.entry_portal,stair_child_choice.exit_portal,1u,17000u);
    play_route(stair_child_bundle,stair_child_choice.exit_portal,stair_child_choice.entry_portal,1u,18000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();

    if(!tsp_stream_back(&world)||!same_node(&world.current,&stair)){
        g_room_bundle_stream_status=0xEE24u;
        for(;;)vsync();
    }

    /* Raised seam -> descend -> original floor-zero canonical seam. */
    play_route(3u,stair_choice.exit_portal,stair_choice.entry_portal,1u,19000u);
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();

    g_room_bundle_stream_status=20u;
    g_room_bundle_stream_progress=22000u;

    /*
     * Final hostile-camera visual tour. The procedural tests above have
     * already exercised bundles 2,3,4,6,7. Explicitly run the remaining
     * visual classes so every one of the eight authored modules executes on
     * the GG, including both directions through the lit L-turn.
     */
    play_route(0u,0u,1u,1u,23000u); /* wide portal-shadow chamber */
    play_route(1u,0u,1u,1u,24000u); /* tight inset/spooky chamber */
    play_route(5u,0u,1u,1u,25000u); /* flat quarter-turn */
    play_route(5u,1u,0u,1u,26000u); /* same L in reverse handedness */
    if(g_room_bundle_stream_status>=0xEE00u)for(;;)vsync();

    g_room_bundle_stream_status=30u;
    g_room_bundle_stream_progress=30000u;

    for(;;)vsync();
}
