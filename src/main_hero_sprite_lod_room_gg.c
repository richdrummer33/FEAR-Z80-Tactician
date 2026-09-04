/*
 * Lit-room + transparent hierarchical hero LOD runtime proof.
 *
 * Background: mature coarse-lattice baked room bundle, tile cache capped at
 * 416 so VRAM 0x3400..0x37ff is never used by room patterns.
 *
 * Hero sprites (assuming the normal GG/SMS sprite pattern base at 0x2000):
 *   sprite tile 160..175 => VRAM 0x3400..0x35ff, R32 refinement
 *   sprite tile 176..191 => VRAM 0x3600..0x37ff, R36 far core
 *
 * Runtime intent:
 *   - core is loaded once;
 *   - angle changes rewrite sprite attributes only;
 *   - entering R32 loads sixteen refinement patterns once;
 *   - room continues its own name-table/tile-patch playback underneath;
 *   - sprite colour zero is transparent, avoiding hero+room boundary tiles.
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "room_bundle_poc_meta.h"
#include "hero_sprite_lod_pack.h"

#define C_BLACK 0u
#define C_CEILING 1u
#define C_FLOOR 2u
#define ROOM_BUNDLE 0u
#define ROOM_ENTRY 0u
#define ROOM_EXIT 1u
#define AUTO_ANGLE_DIV 6u
#define AUTO_BAND_FRAMES 180u

uint16_t g_map[TSP_MAP_CELLS];
uint8_t g_hero_lod_last_count;
volatile uint16_t g_hero_lod_room_status;
volatile uint16_t g_hero_lod_room_progress;
volatile uint8_t g_hero_lod_angle;
volatile uint8_t g_hero_lod_band;
volatile uint8_t g_hero_lod_refinement_loaded;
volatile uint8_t g_hero_lod_sprite_count;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);

static uint8_t g_tile[32u];

/* Same ambient/light semantic palette used by the room proof.  The sprite
 * palette deliberately uses the ambient 1..7 ramp so the object-only corpus
 * can be replayed directly while room lighting continues independently. */
static const palette_color_t k_bg_palettes[32] = {
    RGB(0,0,0),RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),
    RGB(4,5,7),RGB(8,9,11),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),
    RGB(0,0,0),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),RGB(10,11,13),
    RGB(8,9,11),RGB(10,11,13),
    RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),RGB(4,5,7),RGB(8,9,11),RGB(0,0,0)
};

static const palette_color_t k_sprite_palette[16] = {
    RGB(0,0,0),RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),
    RGB(4,5,7),RGB(8,9,11),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0)
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

static void apply_hero(void){
    hero_lod_apply_view(g_hero_lod_band,g_hero_lod_angle);
    g_hero_lod_sprite_count=hero_lod_view_sprite_count(
        g_hero_lod_band,g_hero_lod_angle);
}

static void enter_mid_band(void){
    if(!g_hero_lod_refinement_loaded){
        /* Sixteen pattern uploads.  The measured room route peaks at roughly
         * twenty-two with this 416-pattern cache, keeping the one-time
         * refinement load + room pattern work below the ~48-pattern target. */
        hero_lod_load_refinement();
        g_hero_lod_refinement_loaded=1u;
    }
    g_hero_lod_band=HERO_LOD_MID_BAND;
    apply_hero();
}

static void enter_far_band(void){
    g_hero_lod_band=HERO_LOD_FAR_BAND;
    apply_hero();
}

void main(void){
    uint16_t room_frame=0u,room_frames;
    uint16_t auto_counter=0u,band_counter=0u;
    uint8_t prev_pad=0u;
    uint8_t room_tick=0u;

    g_hero_lod_last_count=0u;
    g_hero_lod_room_status=0u;
    g_hero_lod_room_progress=0u;
    g_hero_lod_angle=0u;
    g_hero_lod_band=HERO_LOD_FAR_BAND;
    g_hero_lod_refinement_loaded=0u;
    g_hero_lod_sprite_count=0u;

    DISPLAY_OFF;
    __WRITE_VDP_REG(VDP_R2,R2_MAP_0x3800);
    SPRITES_8x8;
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_bg_palettes);
    set_sprite_palette(0u,1u,k_sprite_palette);
    init_base_tiles();
    tsp_polar_nt_init();

    room_frames=tsp_room_bundle_generated_frames(
        ROOM_BUNDLE,ROOM_ENTRY,ROOM_EXIT);
    if(!room_frames){
        g_hero_lod_room_status=0xee01u;
        for(;;)vsync();
    }

    /* Cold room frame and far-core sprite vocabulary are loaded with the
     * display blanked, so neither costs visible-time bandwidth. */
    tsp_room_bundle_generated_apply_tile(
        ROOM_BUNDLE,ROOM_ENTRY,ROOM_EXIT,0u);
    tsp_room_bundle_generated_load_canonical();
    tsp_polar_nt_upload_dirty();
    hero_lod_load_core();
    apply_hero();

    DISPLAY_ON;
    g_hero_lod_room_status=1u;

    for(;;){
        uint8_t pad=joypad();
        uint8_t pressed=(uint8_t)(pad & (uint8_t)~prev_pad);
        uint8_t room_advance=0u;
        prev_pad=pad;

        /* Manual inspection controls. */
        if(pressed&J_LEFT){
            g_hero_lod_angle=(uint8_t)((g_hero_lod_angle+HERO_LOD_ANGLE_COUNT-1u)%HERO_LOD_ANGLE_COUNT);
            apply_hero();
        }
        if(pressed&J_RIGHT){
            g_hero_lod_angle=(uint8_t)((g_hero_lod_angle+1u)%HERO_LOD_ANGLE_COUNT);
            apply_hero();
        }
        if(pressed&J_UP)enter_mid_band();
        if(pressed&J_DOWN)enter_far_band();

        /* No input is needed for CI or browser inspection: orbit the object
         * continuously and periodically exercise the nested distance handoff. */
        if(!(pad&(J_LEFT|J_RIGHT))){
            if(++auto_counter>=AUTO_ANGLE_DIV){
                auto_counter=0u;
                g_hero_lod_angle=(uint8_t)((g_hero_lod_angle+1u)%HERO_LOD_ANGLE_COUNT);
                apply_hero();
            }
        }
        if(++band_counter>=AUTO_BAND_FRAMES){
            band_counter=0u;
            if(g_hero_lod_band==HERO_LOD_FAR_BAND)enter_mid_band();
            else enter_far_band();
        }

        /* Run the independently baked lit room at half rate so the proof
         * visibly exercises room tile/name churn while leaving comfortable
         * time for inspection and input. */
        if(++room_tick>=2u){
            room_tick=0u;
            ++room_frame;
            if(room_frame>=room_frames)room_frame=0u;
            tsp_room_bundle_generated_apply_name(
                ROOM_BUNDLE,ROOM_ENTRY,ROOM_EXIT,room_frame);
            room_advance=1u;
        }

        vsync();

        if(room_advance){
            tsp_room_bundle_generated_apply_tile(
                ROOM_BUNDLE,ROOM_ENTRY,ROOM_EXIT,room_frame);
            tsp_polar_nt_upload_dirty();
        }

        ++g_hero_lod_room_progress;
        g_hero_lod_room_status=(uint16_t)(
            0x100u | ((uint16_t)g_hero_lod_band<<4) |
            (uint16_t)(g_hero_lod_refinement_loaded?1u:0u));
    }
}
