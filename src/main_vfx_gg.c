#include <stdint.h>
#include <gbdk/platform.h>
#include "sim.h"
#include "tiles.h"
#include "vfx_lab.h"
#include "gg_vfx.h"

#define HUD_TILE_BASE 300u
#define HUD_FONT_COUNT 18u
#define SPRITE_TILE_BLUE 0u
#define SPRITE_TILE_RED 1u
#define SPRITE_TILE_HIT 2u
#define SPRITE_TILE_SUPPRESSED 3u
#define CAMERA_X 12u
#define MAP_SCROLL_SCANLINE ((((GG_WORLD_TILE_Y + DEVICE_SCREEN_Y_OFFSET) * 8u)) - 2u)

#ifndef DEFAULT_SEED
#define DEFAULT_SEED 2u
#endif

static Sim g_sim;
static VfxLab g_lab;
static GgVfx g_vfx;
static uint16_t g_last_hud[40u];
static uint8_t g_marker_tiles[4u * 32u];
static uint8_t g_world_scroll_x;

static void paint_pixel(uint8_t *tile,uint8_t x,uint8_t y,uint8_t color){uint8_t p,bit=(uint8_t)(0x80u>>x);uint8_t *row=tile+(uint16_t)y*4u;for(p=0u;p<4u;++p)if(color&(uint8_t)(1u<<p))row[p]|=bit;}
static void init_marker_tile(uint8_t tile_id,uint8_t color){uint8_t *tile=g_marker_tiles+(uint16_t)tile_id*32u;uint8_t i,x,y;for(i=0u;i<32u;++i)tile[i]=0u;for(y=2u;y<6u;++y)for(x=2u;x<6u;++x)paint_pixel(tile,x,y,color);}
static uint16_t hud_font_tile(char c){uint8_t src=gg_font_tile(c);return (uint16_t)(HUD_TILE_BASE+(uint16_t)(src-TILE_FONT_SPACE));}
static void invalidate_hud(void){uint8_t i;for(i=0u;i<40u;++i)g_last_hud[i]=0xffffu;}
static void put_u4(uint16_t *hud,uint8_t pos,uint16_t value){
    uint8_t d0=0u,d1=0u,d2=0u;
    while(value>=1000u){value=(uint16_t)(value-1000u);++d0;}
    while(value>=100u){value=(uint16_t)(value-100u);++d1;}
    while(value>=10u){value=(uint16_t)(value-10u);++d2;}
    hud[pos]=hud_font_tile((char)('0'+d0));hud[(uint8_t)(pos+1u)]=hud_font_tile((char)('0'+d1));hud[(uint8_t)(pos+2u)]=hud_font_tile((char)('0'+d2));hud[(uint8_t)(pos+3u)]=hud_font_tile((char)('0'+(uint8_t)value));
}
static void put_u3(uint16_t *hud,uint8_t pos,uint16_t value){uint8_t h=0u,t=0u;while(value>=100u){value=(uint16_t)(value-100u);++h;}while(value>=10u){value=(uint16_t)(value-10u);++t;}hud[pos]=hud_font_tile((char)('0'+h));hud[(uint8_t)(pos+1u)]=hud_font_tile((char)('0'+t));hud[(uint8_t)(pos+2u)]=hud_font_tile((char)('0'+(uint8_t)value));}
static void put_u2(uint16_t *hud,uint8_t pos,uint8_t value){uint8_t t=0u;while(value>=10u){value=(uint8_t)(value-10u);++t;}hud[pos]=hud_font_tile((char)('0'+t));hud[(uint8_t)(pos+1u)]=hud_font_tile((char)('0'+value));}

static void render_hud(void){
    uint16_t hud[40u];uint8_t i;uint16_t f=g_lab.scene_frame,b=g_vfx.vram_bytes_last;
    for(i=0u;i<40u;++i)hud[i]=hud_font_tile(' ');
    hud[0]=hud_font_tile('P');hud[1]=hud_font_tile((char)('0'+g_lab.scene));
    hud[3]=hud_font_tile('T');put_u4(hud,4,f);
    hud[9]=hud_font_tile('A');hud[10]=hud_font_tile(g_lab.ai_tick_div_hint?(char)('0'+g_lab.ai_tick_div_hint):'0');
    hud[12]=hud_font_tile('C');put_u2(hud,13,g_vfx.scratch_used_count);
    hud[20]=hud_font_tile('B');put_u3(hud,21,b>999u?999u:b);
    hud[25]=hud_font_tile('R');put_u2(hud,26,g_vfx.queue.count);
    hud[29]=hud_font_tile('P');hud[30]=hud_font_tile((char)('0'+g_lab.work_units_hint));
    for(i=0u;i<20u;++i)if(hud[i]!=g_last_hud[i]){set_attributed_tile_xy(i,0u,hud[i]);g_last_hud[i]=hud[i];}
    for(i=0u;i<20u;++i){uint8_t j=(uint8_t)(i+20u);if(hud[j]!=g_last_hud[j]){set_attributed_tile_xy(i,1u,hud[j]);g_last_hud[j]=hud[j];}}
}

/* Keep the proven single HUD/world split interrupt. The multi-band shockwave is
   deliberately not installed in the validation ROM. */
static void vblank_scroll_isr(void){__WRITE_VDP_REG_UNSAFE(VDP_R10,MAP_SCROLL_SCANLINE);__WRITE_VDP_REG_UNSAFE(VDP_RSCX,0u);}
static void scanline_scroll_isr(void){__WRITE_VDP_REG_UNSAFE(VDP_RSCX,(uint8_t)(0u-g_world_scroll_x));__WRITE_VDP_REG_UNSAFE(VDP_R10,R10_INT_OFF);}

static void init_video(void){
    uint8_t x,y;
    set_bkg_4bpp_data(HUD_TILE_BASE,HUD_FONT_COUNT,gg_tile_data+(uint16_t)TILE_FONT_SPACE*32u);
    init_marker_tile(SPRITE_TILE_BLUE,5u);init_marker_tile(SPRITE_TILE_RED,6u);init_marker_tile(SPRITE_TILE_HIT,7u);init_marker_tile(SPRITE_TILE_SUPPRESSED,13u);
    set_sprite_4bpp_data(0u,4u,g_marker_tiles);
    for(y=0u;y<18u;++y)for(x=0u;x<GG_WORLD_BLOCK_W;++x)set_attributed_tile_xy(x,y,hud_font_tile(' '));
    CRITICAL {add_LCD(scanline_scroll_isr);add_VBL(vblank_scroll_isr);}
    set_interrupts(VBL_IFLAG|LCD_IFLAG);
    __WRITE_VDP_REG(VDP_RSCX,0u);
}

static void render_demo_agents(void){
    uint8_t i;
    for(i=0u;i<SIM_MAX_AGENTS;++i){
        if(i>=g_sim.agent_count||!g_sim.agents[i].alive){hide_sprite(i);continue;}
        {const Agent *a=&g_sim.agents[i];int16_t sx=(int16_t)a->x*4+2-CAMERA_X+g_lab.shake_x;int16_t sy=(int16_t)GG_WORLD_TILE_Y*8+(int16_t)a->y*4+2+g_lab.shake_y;uint8_t tile;
         if(sx<-6||sx>165||sy<-6||sy>149){hide_sprite(i);continue;}
         if(a->hit_flash)tile=SPRITE_TILE_HIT;else if(a->suppressed)tile=SPRITE_TILE_SUPPRESSED;else tile=i<SIM_BLUE_COUNT?SPRITE_TILE_BLUE:SPRITE_TILE_RED;
         set_sprite_tile(i,tile);move_sprite(i,(uint8_t)(DEVICE_SPRITE_PX_OFFSET_X+sx),(uint8_t)(DEVICE_SPRITE_PX_OFFSET_Y+sy));}
    }
}

void main(void){
    DISPLAY_OFF;HIDE_SPRITES;SET_BORDER_COLOR(0u);SPRITES_8x8;
    sim_init(&g_sim,DEFAULT_SEED);
    vfx_lab_target_init(&g_lab,0xA5E1u);
    init_video();
    gg_vfx_target_init(&g_vfx,&g_sim);
    vfx_lab_target_step(&g_lab,&g_sim);
    g_world_scroll_x=(uint8_t)(CAMERA_X-g_lab.shake_x);
    render_demo_agents();invalidate_hud();render_hud();
    DISPLAY_ON;

    for(;;){
        vfx_lab_target_step(&g_lab,&g_sim);
        g_world_scroll_x=(uint8_t)(CAMERA_X-g_lab.shake_x);
        render_demo_agents();
        if((g_lab.total_frame&7u)==0u)render_hud();
        wait_vbl_done();
        gg_vfx_target_frame(&g_vfx,&g_lab,&g_sim,CAMERA_X);
    }
}
