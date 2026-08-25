#ifndef TILESECTOR_CORE_H
#define TILESECTOR_CORE_H

#include <stdint.h>

#define TS_COLS 20u
#define TS_ROWS 18u
#define TS_MAP_CELLS (TS_COLS * TS_ROWS)

#define TS_INPUT_UP    0x01u
#define TS_INPUT_DOWN  0x02u
#define TS_INPUT_LEFT  0x04u
#define TS_INPUT_RIGHT 0x08u
#define TS_INPUT_START 0x10u

#define TS_TILE_CEILING 0u
#define TS_TILE_FLOOR   1u
#define TS_TILE_HORIZON 2u
#define TS_TILE_WALL_FULL_BASE 3u
#define TS_SHADE_COUNT 3u
#define TS_BORDER_COUNT 4u
#define TS_TILE_WALL_TOP_BASE (TS_TILE_WALL_FULL_BASE + TS_SHADE_COUNT * TS_BORDER_COUNT)
#define TS_TILE_WALL_BOTTOM_BASE (TS_TILE_WALL_TOP_BASE + TS_SHADE_COUNT * 8u * TS_BORDER_COUNT)
#define TS_GENERATED_TILE_COUNT (TS_TILE_WALL_BOTTOM_BASE + TS_SHADE_COUNT * 8u * TS_BORDER_COUNT)

#define TS_TILE_FULL(shade, border) ((uint8_t)(TS_TILE_WALL_FULL_BASE + (shade) * TS_BORDER_COUNT + (border)))
#define TS_TILE_TOP(shade, off, border) ((uint8_t)(TS_TILE_WALL_TOP_BASE + (((shade) * 8u + (off)) * TS_BORDER_COUNT) + (border)))
#define TS_TILE_BOTTOM(shade, off, border) ((uint8_t)(TS_TILE_WALL_BOTTOM_BASE + (((shade) * 8u + (off)) * TS_BORDER_COUNT) + (border)))

typedef struct TSState {
    int16_t x_q8;
    int16_t y_q8;
    uint8_t yaw;
    int16_t speed_q4;
    int16_t turn_q4;
    uint8_t manual;
    uint8_t demo_phase;
    uint16_t demo_ticks;
} TSState;

typedef struct TSColumn {
    uint8_t invz;
    uint8_t wall_id;
    uint8_t shade;
    uint8_t top;
    uint8_t bottom;
} TSColumn;

void ts_reset(TSState *s);
void ts_step(TSState *s, uint8_t input);
void ts_render_columns(const TSState *s, TSColumn cols[TS_COLS]);
void ts_build_tilemap(const TSState *s, uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]);
uint8_t ts_is_walkable_q8(int16_t x_q8, int16_t y_q8);

#endif
