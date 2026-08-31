#include <stdint.h>
#include <gbdk/platform.h>

uint8_t room_poc_probe_bank4(void) BANKED;
uint8_t room_poc_probe_bank63(void) BANKED;
uint8_t room_poc_probe_bank64(void) BANKED;
uint8_t room_poc_probe_bank127(void) BANKED;
uint8_t room_poc_probe_bank191(void) BANKED;
uint8_t room_poc_probe_bank254(void) BANKED;

volatile uint16_t g_room_poc_bank_probe_status;
volatile uint8_t g_room_poc_bank_probe_mask;

static uint8_t g_tile[32u];
static uint8_t g_map[32u*28u*2u];

static void make_solid_tile(uint8_t color){
    uint8_t y,p;
    for(y=0u;y<8u;++y){
        for(p=0u;p<4u;++p)
            g_tile[(uint16_t)y*4u+p]=(color&(uint8_t)(1u<<p))?0xffu:0u;
    }
}

static void fill_map(void){
    uint16_t i;
    for(i=0u;i<32u*28u;++i){
        g_map[i*2u]=0u;
        g_map[i*2u+1u]=0u;
    }
}

void main(void){
    static const palette_color_t pal[16] = {
        RGB(0,0,0), RGB(15,0,0), RGB(0,15,0), RGB(15,15,15),
        RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0),
        RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0),
        RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0)
    };
    uint8_t ok=1u,mask=0u;

    DISPLAY_OFF;
    HIDE_SPRITES;
    SET_BORDER_COLOR(0u);
    g_room_poc_bank_probe_status=0u;
    g_room_poc_bank_probe_mask=0u;

    if(room_poc_probe_bank4()==4u)mask|=0x01u;else ok=0u;
    if(room_poc_probe_bank63()==63u)mask|=0x02u;else ok=0u;
    if(room_poc_probe_bank64()==64u)mask|=0x04u;else ok=0u;
    if(room_poc_probe_bank127()==127u)mask|=0x08u;else ok=0u;
    if(room_poc_probe_bank191()==191u)mask|=0x10u;else ok=0u;
    if(room_poc_probe_bank254()==254u)mask|=0x20u;else ok=0u;

    g_room_poc_bank_probe_mask=mask;
    make_solid_tile(ok?2u:1u);
    fill_map();
    set_bkg_palette(0u,1u,pal);
    set_bkg_4bpp_data(0u,1u,g_tile);
    set_tile_map(0u,0u,32u,28u,g_map);

    /* Little-endian RAM bytes are 01 3F on complete success:
     * low byte = pass/fail status; high byte = six sentinel bits. */
    g_room_poc_bank_probe_status=(uint16_t)(((uint16_t)mask<<8)|(ok?1u:2u));
    DISPLAY_ON;

    for(;;)vsync();
}
