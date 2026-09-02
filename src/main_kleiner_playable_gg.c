/*
 * Interactive Kleiner's Lab reconstruction ROM.
 *
 * This is not route playback. The host bake contains 592 independently
 * addressable camera states: thirty-seven traced legal positions across the
 * actual lab floor plan times sixteen yaw angles. Each state owns a complete name table and a compact
 * contiguous pattern block.
 *
 * Runtime alternates two disjoint VRAM pools. While pool A is visible, every
 * pattern for the requested state is uploaded into pool B over as many VBlanks
 * as necessary. Only after that complete destination image exists do we publish
 * its name table. The next move reverses A/B. Random movement order therefore
 * cannot desynchronize a persistent tile cache.
 *
 * Controls:
 *   D-pad UP/DOWN  - forward/back
 *   D-pad LEFT/RIGHT - turn left/right (22.5 degrees per state)
 *   button 1 / A   - strafe left
 *   button 2 / B   - strafe right
 *   START          - return to the initial inspection position
 */
#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar.h"
#include "kleiner_playable_meta.h"

#define C_BLACK 0u
#define C_CEILING 1u
#define C_FLOOR 2u
#define SCENE_UPLOAD_CAP 48u

uint16_t g_map[TSP_MAP_CELLS];

volatile uint16_t g_kleiner_play_state;
volatile uint16_t g_kleiner_play_actions;
volatile uint16_t g_kleiner_play_status;
volatile uint8_t g_kleiner_play_ix;
volatile uint8_t g_kleiner_play_iy;
volatile uint8_t g_kleiner_play_yaw;
volatile uint8_t g_kleiner_play_pool;
volatile uint8_t g_kleiner_play_phases;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);

static uint8_t g_tile[32u];

static const palette_color_t k_palettes[32] = {
    /* Game Gear channels are 4-bit: keep every component in 0..15. */
    RGB(0,0,0), RGB(4,5,5), RGB(6,6,5), RGB(5,4,3),
    RGB(8,7,5), RGB(13,12,9), RGB(6,5,4), RGB(10,9,7),
    RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0),
    RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(15,4,1),
    /* Palette 1: lit transform + mixed-tile ambient aliases. */
    RGB(0,0,0), RGB(8,9,9), RGB(9,8,6), RGB(10,9,7),
    RGB(12,11,8), RGB(15,14,10), RGB(11,10,7), RGB(14,13,9),
    RGB(4,5,5), RGB(6,6,5), RGB(5,4,3), RGB(8,7,5),
    RGB(13,12,9), RGB(6,5,4), RGB(10,9,7), RGB(15,6,2)
};

static const int8_t k_move_dx[8]={ 1, 1, 0,-1,-1,-1, 0, 1};
static const int8_t k_move_dy[8]={ 0, 1, 1, 1, 0,-1,-1,-1};

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

static uint16_t state_for(uint8_t ix,uint8_t iy,uint8_t yaw){
    uint8_t p=kleiner_play_position_ordinal(ix,iy);
    if(p==0xffu)return 0xffffu;
    return (uint16_t)p*KLEINER_PLAY_YAWS+(uint16_t)(yaw&(KLEINER_PLAY_YAWS-1u));
}

/* Load one complete random-access pose into the invisible pattern pool, then
 * publish the corresponding full name table on its own VBlank. */
static void present_state(uint16_t state,uint8_t boot){
    uint16_t n=kleiner_play_state_patterns(state),first=0u;
    uint8_t next_pool=boot?0u:(uint8_t)(g_kleiner_play_pool^1u);
    uint8_t phases=1u;

    if(n>KLEINER_PLAY_POOL_SIZE){
        g_kleiner_play_status=0xEE10u;
        return;
    }

    while(first<n){
        uint16_t remain=(uint16_t)(n-first);
        uint16_t take=remain>SCENE_UPLOAD_CAP?SCENE_UPLOAD_CAP:remain;
        if(!boot)vsync();
        kleiner_play_upload_state(state,next_pool,first,take);
        first=(uint16_t)(first+take);
        ++phases;
    }

    /* Name-table publication gets its own blank. That keeps the tile budget
     * honest rather than pretending forty-eight pattern uploads plus a full
     * 20x18 map rewrite somehow occupy the same VBlank for free. */
    if(!boot)vsync();
    kleiner_play_apply_name(state,next_pool);
    tsp_polar_nt_upload_dirty();

    g_kleiner_play_pool=next_pool;
    g_kleiner_play_state=state;
    g_kleiner_play_phases=phases;
}

/* Quantize sixteen look directions to the nearest of eight grid movement
 * directions. This affects movement only; the rendered view still uses all
 * sixteen 22.5-degree yaw states. */
static uint8_t move_dir8(uint8_t yaw){
    return (uint8_t)(((yaw+1u)>>1)&7u);
}

static int8_t clamp_step(int8_t v){
    return v<0?-1:(v>0?1:0);
}

/* Match movement connectivity to the traced walls. Coordinates are tenths of
 * a GG world unit so x=61.6 / y=-3.2 / x=104.8 remain exact on Z80. */
static uint8_t vertical_wall_blocks(int16_t x0,int16_t y0,
                                    int16_t x1,int16_t y1,
                                    int16_t wall_x,
                                    int16_t span_lo,int16_t span_hi,
                                    int16_t open_lo,int16_t open_hi,
                                    uint8_t has_opening){
    int32_t cross_y;
    int16_t dx=(int16_t)(x1-x0);
    if(!((x0<wall_x&&x1>wall_x)||(x1<wall_x&&x0>wall_x)))return 0u;
    cross_y=(int32_t)y0+
            ((int32_t)(y1-y0)*(int32_t)(wall_x-x0))/(int32_t)dx;
    if(cross_y<span_lo||cross_y>span_hi)return 0u;
    if(has_opening&&cross_y>=open_lo&&cross_y<=open_hi)return 0u;
    return 1u;
}

static uint8_t horizontal_wall_blocks(int16_t x0,int16_t y0,
                                      int16_t x1,int16_t y1,
                                      int16_t wall_y,
                                      int16_t span_lo,int16_t span_hi,
                                      int16_t open_lo,int16_t open_hi,
                                      uint8_t has_opening){
    int32_t cross_x;
    int16_t dy=(int16_t)(y1-y0);
    if(!((y0<wall_y&&y1>wall_y)||(y1<wall_y&&y0>wall_y)))return 0u;
    cross_x=(int32_t)x0+
            ((int32_t)(x1-x0)*(int32_t)(wall_y-y0))/(int32_t)dy;
    if(cross_x<span_lo||cross_x>span_hi)return 0u;
    if(has_opening&&cross_x>=open_lo&&cross_x<=open_hi)return 0u;
    return 1u;
}

static uint8_t kleiner_step_allowed(uint8_t ix0,uint8_t iy0,
                                    uint8_t ix1,uint8_t iy1){
    int16_t x0=(int16_t)(KLEINER_PLAY_ORIGIN_X*10+
                         (int16_t)ix0*KLEINER_PLAY_STEP*10);
    int16_t y0=(int16_t)(KLEINER_PLAY_ORIGIN_Y*10+
                         (int16_t)iy0*KLEINER_PLAY_STEP*10);
    int16_t x1=(int16_t)(KLEINER_PLAY_ORIGIN_X*10+
                         (int16_t)ix1*KLEINER_PLAY_STEP*10);
    int16_t y1=(int16_t)(KLEINER_PLAY_ORIGIN_Y*10+
                         (int16_t)iy1*KLEINER_PLAY_STEP*10);

    if(vertical_wall_blocks(x0,y0,x1,y1,616,-160,480,36,204,1u))return 0u;
    if(vertical_wall_blocks(x0,y0,x1,y1,1048,-160,-32,0,0,0u))return 0u;
    if(horizontal_wall_blocks(x0,y0,x1,y1,-32,1048,1320,1140,1244,1u))return 0u;
    return 1u;
}

static uint8_t apply_controls(uint8_t keys){
    uint8_t old_ix=g_kleiner_play_ix,old_iy=g_kleiner_play_iy,old_yaw=g_kleiner_play_yaw;
    uint8_t dir;
    int8_t mx=0,my=0;
    int16_t nx,ny;
    uint16_t state;

    if(keys&J_START){
        g_kleiner_play_ix=4u;
        g_kleiner_play_iy=1u;
        g_kleiner_play_yaw=7u;
    }else{
        if((keys&J_LEFT)&&!(keys&J_RIGHT))
            g_kleiner_play_yaw=(uint8_t)((g_kleiner_play_yaw+KLEINER_PLAY_YAWS-1u)&
                                      (KLEINER_PLAY_YAWS-1u));
        else if((keys&J_RIGHT)&&!(keys&J_LEFT))
            g_kleiner_play_yaw=(uint8_t)((g_kleiner_play_yaw+1u)&
                                      (KLEINER_PLAY_YAWS-1u));

        dir=move_dir8(g_kleiner_play_yaw);
        if((keys&J_UP)&&!(keys&J_DOWN)){
            mx+=k_move_dx[dir];my+=k_move_dy[dir];
        }else if((keys&J_DOWN)&&!(keys&J_UP)){
            mx-=k_move_dx[dir];my-=k_move_dy[dir];
        }

        /* With yaw zero facing +X, screen-left is world -Y and screen-right
         * is +Y. Rotate the eight-way movement direction by +/-90 degrees. */
        if((keys&J_A)&&!(keys&J_B)){
            uint8_t sd=(uint8_t)((dir+6u)&7u);
            mx+=k_move_dx[sd];my+=k_move_dy[sd];
        }else if((keys&J_B)&&!(keys&J_A)){
            uint8_t sd=(uint8_t)((dir+2u)&7u);
            mx+=k_move_dx[sd];my+=k_move_dy[sd];
        }

        mx=clamp_step(mx);my=clamp_step(my);
        nx=(int16_t)g_kleiner_play_ix+mx;
        ny=(int16_t)g_kleiner_play_iy+my;
        if(mx||my){
            if(nx>=0&&nx<KLEINER_PLAY_GRID_W&&ny>=0&&ny<KLEINER_PLAY_GRID_H&&
               kleiner_play_position_ordinal((uint8_t)nx,(uint8_t)ny)!=0xffu&&
               kleiner_step_allowed(g_kleiner_play_ix,g_kleiner_play_iy,
                                    (uint8_t)nx,(uint8_t)ny)){
                g_kleiner_play_ix=(uint8_t)nx;
                g_kleiner_play_iy=(uint8_t)ny;
            }
        }
    }

    if(old_ix==g_kleiner_play_ix&&old_iy==g_kleiner_play_iy&&old_yaw==g_kleiner_play_yaw)
        return 0u;

    state=state_for(g_kleiner_play_ix,g_kleiner_play_iy,g_kleiner_play_yaw);
    if(state==0xffffu){
        g_kleiner_play_ix=old_ix;g_kleiner_play_iy=old_iy;g_kleiner_play_yaw=old_yaw;
        g_kleiner_play_status=0xEE11u;
        return 0u;
    }
    ++g_kleiner_play_actions;
    present_state(state,0u);
    return 1u;
}

void main(void){
    uint8_t prev=0u,repeat=0u;
    uint16_t start;

    DISPLAY_OFF;
    __WRITE_VDP_REG(VDP_R2,R2_MAP_0x3800);
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_palettes);
    init_base_tiles();

    g_kleiner_play_status=0u;
    g_kleiner_play_actions=0u;
    g_kleiner_play_ix=4u;
    g_kleiner_play_iy=1u;
    g_kleiner_play_yaw=7u;
    g_kleiner_play_pool=1u;
    g_kleiner_play_phases=0u;

    tsp_polar_nt_init();
    kleiner_play_load_dictionary();

    start=state_for(g_kleiner_play_ix,g_kleiner_play_iy,g_kleiner_play_yaw);
    if(start==0xffffu){
        g_kleiner_play_status=0xEE12u;
        for(;;)vsync();
    }
    present_state(start,1u);
    g_kleiner_play_status=1u;
    DISPLAY_ON;

    for(;;){
        uint8_t keys;
        vsync();
        keys=joypad();

        if(!keys){
            prev=0u;repeat=0u;
            continue;
        }

        /* Immediate first response. Holding a control repeats after two idle
         * polls; the expensive state publication itself naturally limits the
         * sustained movement rate, so this does not need a timer interrupt. */
        if(keys!=prev){
            repeat=0u;
            apply_controls(keys);
        }else if(++repeat>=2u){
            repeat=0u;
            apply_controls(keys);
        }
        prev=keys;
    }
}
