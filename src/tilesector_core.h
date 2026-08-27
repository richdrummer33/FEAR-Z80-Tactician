#ifndef TILESECTOR_CORE_H
#define TILESECTOR_CORE_H

#include <stdint.h>

#define TS_COLS 20u
#define TS_ROWS 18u
#define TS_MAP_CELLS (TS_COLS * TS_ROWS)

#define TS_INPUT_UP            0x01u
#define TS_INPUT_DOWN          0x02u
#define TS_INPUT_LEFT          0x04u
#define TS_INPUT_RIGHT         0x08u
#define TS_INPUT_SPEED         0x10u
#define TS_INPUT_STRAFE_LEFT   0x20u
#define TS_INPUT_STRAFE_RIGHT  0x40u

#define TS_NO_WALL 0xffu
#define TS_SHADE_COUNT 3u
#define TS_BORDER_COUNT 4u
#define TS_CAP_COUNT 3u

/* Edge patterns store positive rises 0..7. Negative slopes reuse them with
 * TS_ATTR_FLIPX. The offset range is wide enough for a <=7px line to cross
 * a hardware-tile row without a visual break. */
#define TS_EDGE_OFF_MIN (-7)
#define TS_EDGE_OFF_COUNT 16u
#define TS_EDGE_SLOPE_COUNT 8u

/* SMS/GG name-table high-byte flags, stored in a little-endian uint16_t. */
#define TS_ATTR_FLIPX   0x0200u
#define TS_ATTR_FLIPY   0x0400u
#define TS_ATTR_PALETTE 0x0800u
#define TS_TILE_ID_MASK 0x01ffu

#define TS_TILE_CEILING 0u
#define TS_TILE_FLOOR   1u
#define TS_TILE_HORIZON 2u
#define TS_TILE_FULL_BASE 3u
#define TS_TILE_EDGE_BASE (TS_TILE_FULL_BASE + TS_SHADE_COUNT * TS_CAP_COUNT * TS_BORDER_COUNT)
#define TS_GENERATED_TILE_COUNT (TS_TILE_EDGE_BASE + TS_SHADE_COUNT * TS_EDGE_OFF_COUNT * TS_EDGE_SLOPE_COUNT)

#define TS_CAP_NONE   0u
#define TS_CAP_TOP    1u
#define TS_CAP_BOTTOM 2u

#define TS_TILE_FULL(shade, cap, border) \
    ((uint16_t)(TS_TILE_FULL_BASE + ((((shade) * TS_CAP_COUNT) + (cap)) * TS_BORDER_COUNT) + (border)))
#define TS_TILE_EDGE(shade, off_index, slope_index) \
    ((uint16_t)(TS_TILE_EDGE_BASE + ((((shade) * TS_EDGE_OFF_COUNT) + (off_index)) * TS_EDGE_SLOPE_COUNT) + (slope_index)))

typedef struct TSState {
    int16_t x_q4;
    int16_t y_q4;
    uint8_t yaw;
    int16_t speed_q4;
    int16_t strafe_q4;
    int16_t turn_q4;
    uint8_t speed_scale;
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
    int8_t top_step;
    int8_t bottom_step;
} TSColumn;

/* Exported so the Gearsystem profiler can split ts_build_tilemap() internally.
 * 0 idle, 1 clear, 2 camera transform, 3 sector/portal visibility+raster. */
extern volatile uint8_t g_ts_render_stage;

void ts_reset(TSState *s);
void ts_step(TSState *s, uint8_t input);
void ts_render_columns(const TSState *s, TSColumn cols[TS_COLS]);
void ts_build_tilemap(const TSState *s, uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]);
uint8_t ts_is_walkable_q4(int16_t x_q4, int16_t y_q4);

#ifndef __SDCC
/* Host-test visibility into the real Q4 camera transform. Excluded from the ROM. */
void ts_debug_transform_vertex_q4(const TSState *s, uint8_t vertex,
                                  int16_t *cam_x_q4, int16_t *cam_z_q4);
#endif

#endif
