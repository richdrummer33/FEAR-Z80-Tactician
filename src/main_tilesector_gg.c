#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_core.h"

#define C_BLACK 0u
#define C_OUT   1u
#define C_FLOOR 2u
#define C_FAR   3u
#define C_MID   4u
#define C_NEAR  5u

/* Palette zero maps C_OUT to ceiling; palette one maps the same pixel index to
 * floor. Bottom wall-edge tiles are therefore the top-edge tile V-flipped plus
 * palette 1: one geometric family serves both wall boundaries. */
static const palette_color_t k_palettes[32] = {
    RGB(0,0,0), RGB(1,1,3), RGB(2,2,3), RGB(3,4,6), RGB(6,7,9), RGB(10,11,13),
    RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0),
    RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0),
    RGB(0,0,0), RGB(2,2,3), RGB(2,2,3), RGB(3,4,6), RGB(6,7,9), RGB(10,11,13),
    RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0),
    RGB(0,0,0), RGB(0,0,0), RGB(0,0,0), RGB(0,0,0)
};

/* Positive rise over eight pixels. Negative projected slopes use H-flip plus
 * a phase correction, so eight patterns cover signed -7..+7. */
static const int8_t k_edge_lut[8][8] = {
    {0,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,1},
    {0,0,1,1,1,1,2,2},
    {0,0,1,1,2,2,3,3},
    {0,1,1,2,2,3,3,4},
    {0,1,1,2,3,4,4,5},
    {0,1,2,3,3,4,5,6},
    {0,1,2,3,4,5,6,7}
};

/* Diagnostic/export visibility only: keeping the exact same object/layout lets
 * the Gearsystem oracle dump the camera state used for each measured frame. */
TSState g_state;
static TSColumn g_cols[TS_COLS];
/* These two buffers are intentionally externally visible to the dedicated
 * Z80 uploader.  The hot upload path takes no C arguments at all. */
uint16_t g_map[TS_MAP_CELLS];
uint16_t g_prev_map[TS_MAP_CELLS];
static uint8_t g_tile[32u];
static uint8_t g_prev_pad;

/* Exported profiling markers. */
volatile uint8_t g_ts_prof_phase;
volatile uint16_t g_ts_loop_count;
volatile uint16_t g_ts_dirty_words;

/* Purpose-built GG name-table uploader in tilesector_vram_gg.s. */
void ts_upload_dirty_map_fast(void);

static uint8_t shade_color(uint8_t shade) {
    if (shade == 0u) return C_FAR;
    if (shade == 1u) return C_MID;
    return C_NEAR;
}

static void clear_tile(void) {
    uint8_t i;
    for (i=0u;i<32u;++i) g_tile[i]=0u;
}

static void paint_pixel(uint8_t x,uint8_t y,uint8_t color) {
    uint8_t plane;
    uint8_t bit=(uint8_t)(0x80u>>x);
    uint8_t *row=g_tile+(uint16_t)y*4u;
    for(plane=0u;plane<4u;++plane)
        if(color&(uint8_t)(1u<<plane)) row[plane]|=bit;
}

static void emit_solid(uint16_t tile_id,uint8_t color) {
    uint8_t x,y;
    clear_tile();
    for(y=0u;y<8u;++y) for(x=0u;x<8u;++x) paint_pixel(x,y,color);
    set_bkg_4bpp_data(tile_id,1u,g_tile);
}

static void emit_horizon(void) {
    uint8_t x,y;
    clear_tile();
    for(y=0u;y<8u;++y) for(x=0u;x<8u;++x)
        paint_pixel(x,y,y==0u?C_BLACK:C_FLOOR);
    set_bkg_4bpp_data(TS_TILE_HORIZON,1u,g_tile);
}

static uint8_t side_border(uint8_t border,uint8_t x) {
    if((border&1u)&&x==0u) return 1u;
    if((border&2u)&&x==7u) return 1u;
    return 0u;
}

static void emit_full(uint8_t shade,uint8_t cap,uint8_t border) {
    uint8_t x,y,color=shade_color(shade);
    clear_tile();
    for(y=0u;y<8u;++y) for(x=0u;x<8u;++x) {
        uint8_t black=side_border(border,x);
        if(cap==TS_CAP_TOP&&y==0u) black=1u;
        if(cap==TS_CAP_BOTTOM&&y==7u) black=1u;
        paint_pixel(x,y,black?C_BLACK:color);
    }
    set_bkg_4bpp_data(TS_TILE_FULL(shade,cap,border),1u,g_tile);
}

static void emit_edge(uint8_t shade,uint8_t off_index,uint8_t slope_index) {
    uint8_t x,y,color=shade_color(shade);
    int8_t off=(int8_t)TS_EDGE_OFF_MIN+(int8_t)off_index;
    clear_tile();
    for(y=0u;y<8u;++y) for(x=0u;x<8u;++x) {
        int8_t line=(int8_t)(off+k_edge_lut[slope_index][x]);
        uint8_t c;
        if((int8_t)y<line) c=C_OUT;
        else if((int8_t)y==line) c=C_BLACK;
        else c=color;
        paint_pixel(x,y,c);
    }
    set_bkg_4bpp_data(TS_TILE_EDGE(shade,off_index,slope_index),1u,g_tile);
}

static void init_tiles(void) {
    uint8_t shade,cap,border,off,slope;
    emit_solid(TS_TILE_CEILING,C_OUT);
    emit_solid(TS_TILE_FLOOR,C_FLOOR);
    emit_horizon();
    for(shade=0u;shade<TS_SHADE_COUNT;++shade)
        for(cap=0u;cap<TS_CAP_COUNT;++cap)
            for(border=0u;border<TS_BORDER_COUNT;++border)
                emit_full(shade,cap,border);
    for(shade=0u;shade<TS_SHADE_COUNT;++shade)
        for(off=0u;off<TS_EDGE_OFF_COUNT;++off)
            for(slope=0u;slope<TS_EDGE_SLOPE_COUNT;++slope)
                emit_edge(shade,off,slope);
}

static void invalidate_map(void) {
    uint16_t i;
    for(i=0u;i<TS_MAP_CELLS;++i) g_prev_map[i]=0xffffu;
}

/* Major-step experiment: the whole dirty scan + hardware map upload is one
 * fixed-geometry Z80 routine.  No row-wise C calls, no generic XY arithmetic,
 * no function arguments, and no second RAM copy pass. */
static uint16_t upload_dirty_map(void) {
    ts_upload_dirty_map_fast();
    return g_ts_dirty_words;
}

static uint8_t read_input(void) {
    uint8_t pad=joypad();
    uint8_t pressed=(uint8_t)(pad&(uint8_t)~g_prev_pad);
    uint8_t input=0u;
    if(pad&J_UP) input|=TS_INPUT_UP;
    if(pad&J_DOWN) input|=TS_INPUT_DOWN;
    if(pad&J_LEFT) input|=TS_INPUT_LEFT;
    if(pad&J_RIGHT) input|=TS_INPUT_RIGHT;
    if(pad&J_B) input|=TS_INPUT_STRAFE_LEFT;
    if(pad&J_A) input|=TS_INPUT_STRAFE_RIGHT;
    if((pressed&J_START)!=0u) input|=TS_INPUT_SPEED;
    g_prev_pad=pad;
    return input;
}

void main(void) {
    DISPLAY_OFF;
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u,2u,k_palettes);
    init_tiles();
    ts_reset(&g_state);
    invalidate_map();
    ts_build_tilemap(&g_state,g_map,g_cols);
    upload_dirty_map();
    g_ts_prof_phase=0u;
    g_ts_loop_count=0u;
    g_ts_dirty_words=0u;
    DISPLAY_ON;

    for(;;) {
        uint8_t input;
        g_ts_prof_phase=1u;
        input=read_input();
        ts_step(&g_state,input);
        g_ts_prof_phase=2u;
        ts_build_tilemap(&g_state,g_map,g_cols);
        g_ts_prof_phase=3u;
        vsync();
        g_ts_prof_phase=4u;
        g_ts_dirty_words=upload_dirty_map();
        g_ts_prof_phase=5u;
        ++g_ts_loop_count;
    }
}
