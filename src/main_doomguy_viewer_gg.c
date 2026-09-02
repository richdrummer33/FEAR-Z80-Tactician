/*
 * Doomguy hero-chamber viewer ROM.
 *
 * The streamed-room proof ROM (main_room_bundle_stream_poc_gg.c) is a scripted
 * CI assertion run: a fixed sequence of routes with hard-coded seeds, ending in
 * a halt. It also bakes all thirteen bundles, which needs 490 generated banks
 * against a 224 budget and therefore cannot link at all.
 *
 * This ROM exists to LOOK at the hero chamber on hardware. It bakes bundle 11
 * alone (67 banks) and does nothing but replay that room's two portal routes.
 *
 * Controls:
 *   1  auto playback  -- authored camera runs continuously (default at boot)
 *   2  manual step    -- playback advances only while RIGHT is held; LEFT
 *                        advances one frame per press
 *
 * Why stepping rather than free movement: the Game Gear holds no geometry for
 * this room. It has a baked stream of name-table DELTA patches along the two
 * authored portal routes, so frame N is only reachable by applying frames
 * 0..N in order. Free camera would need the general polar renderer, not this
 * data. Stepping never skips or reorders frames, so it cannot desynchronize
 * the name-table/tile-cache state -- which is what the earlier
 * "any D-pad press takes over playback" hook did, by abandoning a route
 * part-way and leaving the cache off its canonical seam.
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "room_bundle_poc_meta.h"

#define C_BLACK 0u
#define C_CEILING 1u
#define C_FLOOR 2u

/* The subset pack renumbers densely, so the hero chamber is bundle 0 here. */
#define HERO_BUNDLE 0u

uint16_t g_map[TSP_MAP_CELLS];
volatile uint16_t g_hero_view_status;
volatile uint16_t g_hero_view_frame;
volatile uint8_t  g_hero_view_mode;   /* 0 = auto, 1 = manual step */
volatile uint8_t  g_hero_view_phases; /* refresh slices used by last frame */
volatile uint16_t g_hero_view_boot_trace;

#define HERO_UPLOAD_CAP 48u

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
    /* Palette 0, ambient. 1..5 are the original OUT/FLOOR/FAR/MID/NEAR
     * semantics; 6 and 7 are the interstitial surface-brightness bands added
     * for hero-mesh shading, placed at the midpoints of the two largest gaps
     * so the ramp reads FAR < 6 < MID < 7 < NEAR. */
    RGB(0,0,0),RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),
    RGB(4,5,7),RGB(8,9,11),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),
    /* Palette 1, hard-light transform. 1..7 become one whole shade brighter
     * (+2 ramp positions, clamped at NEAR); 8..14 duplicate ambient 1..7 for
     * pixels on the shadow side of a mixed light-boundary tile. Widening the
     * ambient ramp to index 7 keeps the +7 shadow alias inside the palette:
     * 1..7 map to 8..14, leaving index 15 spare. */
    RGB(0,0,0),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),RGB(10,11,13),
    RGB(8,9,11),RGB(10,11,13),
    RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),RGB(4,5,7),RGB(8,9,11),RGB(0,0,0)
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

/*
 * Atomic frame publication.
 *
 * The host pack orders each frame's tile list with preload-safe entries first.
 * "Safe" means the slot is NOT referenced by the currently visible name table,
 * so those patterns may be written during earlier VBlanks while the old frame
 * remains completely intact.  Any slot still referenced by the old frame is
 * kept in the final publication slice.
 *
 * For a fifty-one-upload frame this is the intended two-phase case:
 *   VBlank A: preload three safe patterns, keep old complete frame visible.
 *   VBlank B: upload the remaining forty-eight, apply the new name patch, then
 *             publish dirty name words.  No partial new frame is ever visible.
 *
 * Larger frames generalize to more staging slices, but every slice is still
 * capped at HERO_UPLOAD_CAP and the name table changes exactly once, at commit.
 */
static void show_frame(uint8_t entry,uint8_t exit_portal,uint16_t frame){
    uint16_t safe=0u,n=tsp_room_bundle_generated_tile_meta(
        HERO_BUNDLE,entry,exit_portal,frame,&safe);
    uint16_t preload=n>HERO_UPLOAD_CAP?(uint16_t)(n-HERO_UPLOAD_CAP):0u;
    uint16_t first=0u;
    uint8_t phases=1u;

    if(preload>safe){
        /* The baker's atomic proof gate should make this unreachable. */
        g_hero_view_status=0xEE02u;
        return;
    }

    while(preload){
        uint16_t take=preload>HERO_UPLOAD_CAP?HERO_UPLOAD_CAP:preload;
        vsync();
        tsp_room_bundle_generated_apply_tile_range(
            HERO_BUNDLE,entry,exit_portal,frame,first,take);
        first=(uint16_t)(first+take);
        preload=(uint16_t)(preload-take);
        ++phases;
    }

    /* Final slice: finish tile residency, then atomically expose the new map. */
    vsync();
    if(first<n)
        tsp_room_bundle_generated_apply_tile_range(
            HERO_BUNDLE,entry,exit_portal,frame,first,(uint16_t)(n-first));
    tsp_room_bundle_generated_apply_name(HERO_BUNDLE,entry,exit_portal,frame);
    tsp_polar_nt_upload_dirty();

    g_hero_view_phases=phases;
    g_hero_view_frame=frame;
}

/* Mode is sampled between frames only, never mid-route, so a mode change can
 * never leave a route part-applied. */
static void poll_mode(void){
    uint8_t keys=joypad();
    if(keys&J_START)g_hero_view_mode=0u;   /* 1 on a two-button Game Gear */
    if(keys&J_A)g_hero_view_mode=0u;
    if(keys&J_B)g_hero_view_mode=1u;
}

/* In manual mode, hold RIGHT to run and tap LEFT to advance a single frame.
 * Returns only when the caller may advance. */
static void wait_for_step(void){
    static uint8_t prev_left=0u;
    for(;;){
        uint8_t keys;
        vsync();
        poll_mode();
        if(!g_hero_view_mode)return;
        keys=joypad();
        if(keys&J_RIGHT){prev_left=(uint8_t)((keys&J_LEFT)?1u:0u);return;}
        if((keys&J_LEFT)&&!prev_left){prev_left=1u;return;}
        if(!(keys&J_LEFT))prev_left=0u;
    }
}

static void play_route(uint8_t entry,uint8_t exit_portal,uint16_t first_frame){
    uint16_t frame;
    uint16_t count=tsp_room_bundle_generated_frames(HERO_BUNDLE,entry,exit_portal);
    if(!count){g_hero_view_status=0xEE01u;return;}
    for(frame=first_frame;frame<count;++frame){
        poll_mode();
        if(g_hero_view_mode)wait_for_step();
        show_frame(entry,exit_portal,frame);
    }
}

void main(void){
    g_hero_view_boot_trace=0xD001u;

    DISPLAY_OFF;
    __WRITE_VDP_REG(VDP_R2,R2_MAP_0x3800);
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_palettes);
    g_hero_view_boot_trace=0xD002u;
    init_base_tiles();
    g_hero_view_boot_trace=0xD003u;

    g_hero_view_status=0u;
    g_hero_view_frame=0u;
    g_hero_view_mode=0u;
    g_hero_view_phases=0u;

    tsp_polar_nt_init();

    /* Cold boot uses canonical frame zero; routes then start at frame one. */
    tsp_room_bundle_generated_apply_tile(HERO_BUNDLE,0u,1u,0u);
    tsp_room_bundle_generated_load_canonical();
    tsp_polar_nt_upload_dirty();
    g_hero_view_boot_trace=0xD004u;
    DISPLAY_ON;
    g_hero_view_boot_trace=0xD005u;

    /*
     * Loop the room forward and back forever. Both directions are baked, and
     * each route ends on the canonical seam, so repeating them is exactly the
     * transition the proof ROM already validates -- no new state machine.
     */
    for(;;){
        play_route(0u,1u,1u);
        g_hero_view_status=1u;
        play_route(1u,0u,1u);
        g_hero_view_status=2u;
    }
}
