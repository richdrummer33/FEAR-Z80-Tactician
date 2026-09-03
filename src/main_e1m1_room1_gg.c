#include <stdint.h>
#include <string.h>
#include <gbdk/platform.h>
#include "e1m1_room1_world.h"
#include "tilesector_polar.h"
#include "e1m1_room1_polar_tiles.h"

#ifndef E1_PROFILE_HOOKS
#define E1_PROFILE_HOOKS 0
#endif
#if E1_PROFILE_HOOKS
volatile uint8_t g_ts_prof_phase;
#define E1_PHASE(v) do { g_ts_prof_phase=(v); } while(0)
#else
#define E1_PHASE(v) ((void)0)
#endif

#define C_BLACK 0u

static const palette_color_t k_palettes[32] = {
    RGB(0,0,0),RGB(1,1,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),
    RGB(0,0,0),RGB(2,2,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),
    RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0),RGB(0,0,0)
};
/* Keep these symbols stable for the existing Gearsystem proof harness. */
E1Room1State g_e1m1_state;
uint16_t g_map[TSP_MAP_CELLS];
volatile uint16_t g_e1m1_loop_count;

static uint8_t g_prev_pad;

void tsp_polar_nt_init(void);
void tsp_polar_nt_upload_dirty(void);

static uint8_t read_input(void){
    uint8_t pad=joypad(),pressed=(uint8_t)(pad&(uint8_t)~g_prev_pad),input=0u;
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

static void render_room1(void){
    TSPState p;
    memset(&p,0,sizeof(p));
    p.x_q4=g_e1m1_state.x_q4;
    p.y_q4=g_e1m1_state.y_q4;
    p.z_q4=g_e1m1_state.z_q4;
    p.yaw=g_e1m1_state.yaw;
    p.speed_scale=g_e1m1_state.speed_scale;
    tsp_polar_render(&p,g_map,(TSPColumn *)0);
}

void main(void){
    DISPLAY_OFF;
    __WRITE_VDP_REG(VDP_R2,R2_MAP_0x3800);
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_palettes);

    /* Establish valid gameplay state before any substantial VRAM transfer.
     * The tile vocabulary itself is generated offline and uploaded in one
     * banked bulk operation, rather than synthesized pixel-by-pixel on Z80. */
    e1_room1_reset(&g_e1m1_state);
    e1m1_room1_polar_tiles_init();
    tsp_polar_renderer_reset();
    g_tspf_appearance_mode=0u;
    tsp_polar_nt_init();
    render_room1();
    tsp_polar_nt_upload_dirty();
    g_e1m1_loop_count=0u;
    E1_PHASE(0u);
    DISPLAY_ON;

    for(;;){
        uint8_t input;
        E1_PHASE(1u);
        input=read_input();
        e1_room1_step(&g_e1m1_state,input);
        E1_PHASE(2u);
        render_room1();
        E1_PHASE(3u);
        vsync();
        E1_PHASE(4u);
        tsp_polar_nt_upload_dirty();
        E1_PHASE(5u);
        ++g_e1m1_loop_count;
    }
}
