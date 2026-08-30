/*
 * Player-like exploration with manual takeover.
 *
 * Starts on the same host-baked exploration rails as the video proof.
 * Any movement input permanently cancels rails and hands control to the live
 * Polar renderer. D-pad controls forward/reverse + rotation; A/B strafe.
 *
 * This build uses the normal mature dirty-row uploader. The live renderer is
 * compiled through the repaired C edge materializer so the chemtrail fix is
 * preserved after manual takeover.
 */
#include <stdint.h>
#include <string.h>
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

/*
 * Match the current baked point/penumbra packet exactly while rails are active.
 * After manual takeover the live generic renderer uses palette 0 only.
 */
static const palette_color_t k_baked_palettes[32] = {
    /* palette 0: ambient */
    RGB(0,0,0), RGB(1,1,3), RGB(3,3,4), RGB(4,2,2),
    RGB(7,4,3), RGB(10,6,4), RGB(9,8,7), RGB(0,0,0),
    RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0),
    RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0),

    /* palette 1: lit colors at 1..6; ambient duplicates at 8..13 */
    RGB(0,0,0), RGB(2,2,4), RGB(4,4,5), RGB(7,4,3),
    RGB(10,6,4), RGB(13,8,5), RGB(13,12,10), RGB(0,0,0),
    RGB(1,1,3), RGB(3,3,4), RGB(4,2,2), RGB(7,4,3),
    RGB(10,6,4), RGB(9,8,7), RGB(0,0,0), RGB(0,0,0)
};

/* Generic live-renderer palettes. Palette 1 keeps floor/outside substitution
 * for bottom edges but does NOT brighten wall shades. */
static const palette_color_t k_live_palettes[32] = {
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
static uint8_t g_live_tile_loaded[(TSP_GENERATED_TILE_COUNT+7u)/8u];
volatile uint8_t g_rail_active;
volatile uint8_t g_dynamic_renderer;
volatile uint8_t g_takeover_stage;
static PolarExploreCursor g_explore;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);
void tsp_polar_demo_patch_apply(uint16_t patch);
void tsp_polar_demo_tilepatch_apply(uint16_t patch);

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
static void init_baked_base_tiles(void){
    emit_solid(TSP_TILE_CEILING,C_OUT);
    emit_solid(TSP_TILE_FLOOR,C_FLOOR);
    emit_horizon();
}
static void emit_live_tile_id(uint16_t id){
    if(id==TSP_TILE_CEILING){emit_solid(id,C_OUT);return;}
    if(id==TSP_TILE_FLOOR){emit_solid(id,C_FLOOR);return;}
    if(id==TSP_TILE_HORIZON){emit_horizon();return;}

    if(id>=TSP_TILE_FULL_BASE && id<TSP_TILE_EDGE_BASE){
        uint16_t rel=(uint16_t)(id-TSP_TILE_FULL_BASE);
        uint8_t border=(uint8_t)(rel%TSP_BORDER_COUNT);
        uint8_t q=(uint8_t)(rel/TSP_BORDER_COUNT);
        uint8_t cap=(uint8_t)(q%TSP_CAP_COUNT);
        uint8_t shade=(uint8_t)(q/TSP_CAP_COUNT);
        emit_full(shade,cap,border);
        return;
    }

    if(id>=TSP_TILE_EDGE_BASE && id<TSP_GENERATED_TILE_COUNT){
        uint16_t rel=(uint16_t)(id-TSP_TILE_EDGE_BASE);
        uint8_t si=(uint8_t)(rel%TSP_EDGE_SLOPE_COUNT);
        uint8_t q=(uint8_t)(rel/TSP_EDGE_SLOPE_COUNT);
        uint8_t oi=(uint8_t)(q%TSP_EDGE_OFF_COUNT);
        uint8_t shade=(uint8_t)(q/TSP_EDGE_OFF_COUNT);
        emit_edge(shade,oi,si);
    }
}

static void ensure_live_tiles_for_map(void){
    uint16_t i;
    for(i=0u;i<TSP_MAP_CELLS;++i){
        uint16_t id=(uint16_t)(g_map[i]&TSP_TILE_ID_MASK);
        uint8_t *b;
        uint8_t m;
        if(id>=TSP_GENERATED_TILE_COUNT)continue;
        b=&g_live_tile_loaded[id>>3];
        m=(uint8_t)(1u<<(id&7u));
        if(!(*b&m)){
            emit_live_tile_id(id);
            *b|=m;
        }
    }
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

    g_takeover_stage=1u;
    DISPLAY_OFF;

    /* Baked point-light palette semantics are no longer needed once arbitrary
     * camera motion begins. Restore the generic renderer palettes. */
    g_takeover_stage=2u;
    set_bkg_palette(0u,2u,k_live_palettes);

    g_takeover_stage=3u;
    memset(g_live_tile_loaded,0,sizeof(g_live_tile_loaded));
    tsp_polar_nt_init();

    g_takeover_stage=4u;
    tsp_polar_renderer_reset();

    g_takeover_stage=5u;
    tsp_polar_render(&g_state,g_map,(TSPColumn *)0);

    /* Upload only the generic tile IDs referenced by THIS live frame instead
     * of brute-forcing all 423 possible patterns. */
    g_takeover_stage=6u;
    ensure_live_tiles_for_map();

    g_takeover_stage=7u;
    tsp_polar_nt_upload_dirty();

    g_dynamic_renderer=1u;
    g_takeover_stage=8u;
    DISPLAY_ON;
}

void main(void){
    DISPLAY_OFF;
    __WRITE_VDP_REG(VDP_R2,R2_MAP_0x3800);
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_baked_palettes);
    init_baked_base_tiles();

    tsp_reset(&g_state);
    polar_explore_cursor_reset(&g_explore);
    g_tspf_appearance_mode=0u;
    g_rail_active=1u;
    g_dynamic_renderer=0u;
    g_takeover_stage=0u;

    tsp_polar_nt_init();
    tsp_polar_demo_patch_apply(0u);
    tsp_polar_demo_tilepatch_apply(0u);
    g_patch_index=1u;
    tsp_polar_nt_upload_dirty();

    DISPLAY_ON;
    for(;;){
        uint8_t pad=joypad();
        uint8_t dpad=dpad_input(pad);
        uint8_t strafe=strafe_input(pad);
        uint8_t scripted=0u;
        uint8_t input;
        uint16_t applied=0xffffu;

        /* Any actual movement control is "give me the wheel".
         * Takeover is permanent until reset. */
        if((uint8_t)(dpad|strafe)){
            g_rail_active=0u;
            switch_to_dynamic_renderer();
        }

        if(g_rail_active)scripted=polar_explore_next(&g_explore);

        if(g_dynamic_renderer){
            input=(uint8_t)(dpad|strafe);
            tsp_step(&g_state,input);
            tsp_polar_render(&g_state,g_map,(TSPColumn *)0);
        }else if(g_patch_index<POLAR_DEMO_PATCH_COUNT){
            /* Untouched on-rails path: exact same pre-baked patch sequence as
             * the original exploration proof. The scripted patch state remains exact. */
            tsp_step(&g_state,scripted);
            applied=g_patch_index;
            tsp_polar_demo_patch_apply(applied);
            ++g_patch_index;
        }

        vsync();

        if(!g_dynamic_renderer && applied!=0xffffu)
            tsp_polar_demo_tilepatch_apply(applied);
        if(g_dynamic_renderer)ensure_live_tiles_for_map();
        tsp_polar_nt_upload_dirty();
    }
}
