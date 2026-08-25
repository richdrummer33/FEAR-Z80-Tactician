#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_core.h"

#define C_BLACK   0u
#define C_CEILING 1u
#define C_FLOOR   2u
#define C_FAR     3u
#define C_MID     4u
#define C_NEAR    5u

static const palette_color_t k_palette[16] = {
    RGB(0, 0, 0), RGB(1, 1, 3), RGB(2, 2, 3), RGB(3, 4, 6),
    RGB(6, 7, 9), RGB(10, 11, 13), RGB(0, 0, 0), RGB(0, 0, 0),
    RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0),
    RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0)
};

static TSState g_state;
static TSColumn g_cols[TS_COLS];
static uint16_t g_map[TS_MAP_CELLS];
static uint16_t g_prev_map[TS_MAP_CELLS];
static uint8_t g_tile[32u];
static uint8_t g_prev_pad;

static uint8_t shade_color(uint8_t shade) {
    if (shade == 0u) return C_FAR;
    if (shade == 1u) return C_MID;
    return C_NEAR;
}

static void clear_tile(void) {
    uint8_t i;
    for (i = 0u; i < 32u; ++i) g_tile[i] = 0u;
}

static void paint_pixel(uint8_t x, uint8_t y, uint8_t color) {
    uint8_t plane;
    uint8_t bit = (uint8_t)(0x80u >> x);
    uint8_t *row = g_tile + (uint16_t)y * 4u;
    for (plane = 0u; plane < 4u; ++plane)
        if (color & (uint8_t)(1u << plane)) row[plane] |= bit;
}

static void emit_solid(uint8_t tile_id, uint8_t color) {
    uint8_t x, y;
    clear_tile();
    for (y = 0u; y < 8u; ++y)
        for (x = 0u; x < 8u; ++x)
            paint_pixel(x, y, color);
    set_bkg_4bpp_data(tile_id, 1u, g_tile);
}

static void emit_horizon(void) {
    uint8_t x, y;
    clear_tile();
    for (y = 0u; y < 8u; ++y)
        for (x = 0u; x < 8u; ++x)
            paint_pixel(x, y, y == 0u ? C_BLACK : C_FLOOR);
    set_bkg_4bpp_data(TS_TILE_HORIZON, 1u, g_tile);
}

static uint8_t has_side_border(uint8_t border, uint8_t x) {
    if ((border & 1u) && x == 0u) return 1u;
    if ((border & 2u) && x == 7u) return 1u;
    return 0u;
}

static void emit_wall_full(uint8_t shade, uint8_t border) {
    uint8_t x, y, color = shade_color(shade);
    clear_tile();
    for (y = 0u; y < 8u; ++y)
        for (x = 0u; x < 8u; ++x)
            paint_pixel(x, y, has_side_border(border, x) ? C_BLACK : color);
    set_bkg_4bpp_data(TS_TILE_FULL(shade, border), 1u, g_tile);
}

static void emit_wall_top(uint8_t shade, uint8_t off, uint8_t border) {
    uint8_t x, y, color = shade_color(shade);
    clear_tile();
    for (y = 0u; y < 8u; ++y) for (x = 0u; x < 8u; ++x) {
        uint8_t c;
        if (y < off) c = C_CEILING;
        else if (y == off) c = C_BLACK;
        else c = has_side_border(border, x) ? C_BLACK : color;
        paint_pixel(x, y, c);
    }
    set_bkg_4bpp_data(TS_TILE_TOP(shade, off, border), 1u, g_tile);
}

static void emit_wall_bottom(uint8_t shade, uint8_t off, uint8_t border) {
    uint8_t x, y, color = shade_color(shade);
    clear_tile();
    for (y = 0u; y < 8u; ++y) for (x = 0u; x < 8u; ++x) {
        uint8_t c;
        if (y > off) c = C_FLOOR;
        else if (y == off) c = C_BLACK;
        else c = has_side_border(border, x) ? C_BLACK : color;
        paint_pixel(x, y, c);
    }
    set_bkg_4bpp_data(TS_TILE_BOTTOM(shade, off, border), 1u, g_tile);
}

static void init_tiles(void) {
    uint8_t shade, border, off;
    emit_solid(TS_TILE_CEILING, C_CEILING);
    emit_solid(TS_TILE_FLOOR, C_FLOOR);
    emit_horizon();
    for (shade = 0u; shade < TS_SHADE_COUNT; ++shade) {
        for (border = 0u; border < TS_BORDER_COUNT; ++border) emit_wall_full(shade, border);
        for (off = 0u; off < 8u; ++off)
            for (border = 0u; border < TS_BORDER_COUNT; ++border) {
                emit_wall_top(shade, off, border);
                emit_wall_bottom(shade, off, border);
            }
    }
}

static void invalidate_map(void) {
    uint16_t i;
    for (i = 0u; i < TS_MAP_CELLS; ++i) g_prev_map[i] = 0xffffu;
}

static void upload_dirty_map(void) {
    uint8_t y;
    for (y = 0u; y < TS_ROWS; ++y) {
        uint8_t x, first = TS_COLS, last = 0u;
        uint16_t row = (uint16_t)y * TS_COLS;
        for (x = 0u; x < TS_COLS; ++x) {
            if (g_map[row + x] != g_prev_map[row + x]) {
                if (first == TS_COLS) first = x;
                last = x;
            }
        }
        if (first != TS_COLS) {
            uint8_t w = (uint8_t)(last - first + 1u);
            set_tile_map(first, y, w, 1u, (const uint8_t *)&g_map[row + first]);
            for (x = first; x <= last; ++x) g_prev_map[row + x] = g_map[row + x];
        }
    }
}

static uint8_t read_input(void) {
    uint8_t pad = joypad();
    uint8_t input = 0u;
    if (pad & J_UP) input |= TS_INPUT_UP;
    if (pad & J_DOWN) input |= TS_INPUT_DOWN;
    if (pad & J_LEFT) input |= TS_INPUT_LEFT;
    if (pad & J_RIGHT) input |= TS_INPUT_RIGHT;
    if ((pad & J_START) && !(g_prev_pad & J_START)) input |= TS_INPUT_START;
    g_prev_pad = pad;
    return input;
}

void main(void) {
    DISPLAY_OFF;
    HIDE_SPRITES;
    SET_BORDER_COLOR(C_BLACK);
    set_bkg_palette(0u, 1u, k_palette);
    init_tiles();
    ts_reset(&g_state);
    invalidate_map();
    ts_build_tilemap(&g_state, g_map, g_cols);
    upload_dirty_map();
    DISPLAY_ON;

    for (;;) {
        ts_step(&g_state, read_input());
        ts_build_tilemap(&g_state, g_map, g_cols);
        vsync();
        upload_dirty_map();
    }
}
