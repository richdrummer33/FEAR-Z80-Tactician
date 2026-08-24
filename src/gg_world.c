#include <gbdk/platform.h>
#include "gg_world.h"

static uint8_t g_pattern_keys[GG_WORLD_PATTERN_CAP];
static uint8_t g_pattern_count;

static uint8_t terrain_color_from_kind(uint8_t kind) {
    if (kind == CELL_WALL) return 2u;
    if (kind == CELL_DOOR_CLOSED) return 3u;
    if (kind == CELL_DOOR_OPEN) return 4u;
    return 1u;
}

static void paint_pixel(uint8_t *tile, uint8_t x, uint8_t y, uint8_t color) {
    uint8_t p;
    uint8_t bit = (uint8_t)(0x80u >> x);
    uint8_t *row = tile + (uint16_t)y * 4u;
    for (p = 0u; p < 4u; ++p)
        if (color & (uint8_t)(1u << p)) row[p] |= bit;
}

void gg_world_reset_patterns(void) { g_pattern_count = 0u; }

uint8_t gg_world_pattern_key(const Sim *sim, uint8_t bx, uint8_t by) {
    uint8_t key = 0u, sx, sy;
    for (sy = 0u; sy < 2u; ++sy) {
        for (sx = 0u; sx < 2u; ++sx) {
            uint8_t cx = (uint8_t)(bx * 2u + sx);
            uint8_t cy = (uint8_t)(by * 2u + sy);
            uint8_t kind = (cx < SIM_W && cy < SIM_H) ? sim_cell_kind(sim, cx, cy) : CELL_WALL;
            key |= (uint8_t)(kind << ((sy * 2u + sx) * 2u));
        }
    }
    return key;
}

void gg_world_make_pattern(uint8_t key, uint8_t *tile) {
    uint8_t sx, sy, px, py, i;
    for (i = 0u; i < 32u; ++i) tile[i] = 0u;
    for (sy = 0u; sy < 2u; ++sy) {
        for (sx = 0u; sx < 2u; ++sx) {
            uint8_t shift = (uint8_t)((sy * 2u + sx) * 2u);
            uint8_t kind = (uint8_t)((key >> shift) & 3u);
            uint8_t color = terrain_color_from_kind(kind);
            for (py = 0u; py < 4u; ++py)
                for (px = 0u; px < 4u; ++px)
                    paint_pixel(tile, (uint8_t)(sx * 4u + px), (uint8_t)(sy * 4u + py), color);
        }
    }
}

uint16_t gg_world_pattern_for_block(const Sim *sim, uint8_t bx, uint8_t by) {
    uint8_t key = gg_world_pattern_key(sim, bx, by);
    uint8_t i;
    for (i = 0u; i < g_pattern_count; ++i)
        if (g_pattern_keys[i] == key) return i;
    if (g_pattern_count < GG_WORLD_PATTERN_CAP) {
        uint8_t tile[32u];
        uint8_t ti = g_pattern_count++;
        g_pattern_keys[ti] = key;
        gg_world_make_pattern(key, tile);
        set_bkg_4bpp_data(ti, 1u, tile);
        return ti;
    }
    return 0u;
}

void gg_world_make_block_tile(const Sim *sim, uint8_t bx, uint8_t by, uint8_t *tile) {
    gg_world_make_pattern(gg_world_pattern_key(sim, bx, by), tile);
}

void gg_world_upload_block(const Sim *sim, uint8_t bx, uint8_t by) {
    uint16_t ti = gg_world_pattern_for_block(sim, bx, by);
    set_attributed_tile_xy(bx, (uint8_t)(GG_WORLD_TILE_Y + by), ti);
}

void gg_world_upload_all(const Sim *sim) {
    uint8_t bx, by;
    for (by = 0u; by < GG_WORLD_BLOCK_H; ++by)
        for (bx = 0u; bx < GG_WORLD_BLOCK_W; ++bx)
            gg_world_upload_block(sim, bx, by);
}
