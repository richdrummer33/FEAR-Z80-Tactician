#ifndef TILESECTOR_POLAR_H
#define TILESECTOR_POLAR_H

#include <stdint.h>

#ifndef TSPF_PROFILE_HOOKS
#define TSPF_PROFILE_HOOKS 1
#endif

#if defined(__SDCC)
#include <gbdk/platform.h>
#else
#ifndef BANKED
#define BANKED
#endif
#endif

#define TSP_COLS 20u
#define TSP_ROWS 18u
#define TSP_MAP_CELLS (TSP_COLS*TSP_ROWS)

#define TSP_INPUT_UP            0x01u
#define TSP_INPUT_DOWN          0x02u
#define TSP_INPUT_LEFT          0x04u
#define TSP_INPUT_RIGHT         0x08u
#define TSP_INPUT_SPEED         0x10u
#define TSP_INPUT_STRAFE_LEFT   0x20u
#define TSP_INPUT_STRAFE_RIGHT  0x40u

#define TSP_SHADE_COUNT 3u
#define TSP_BORDER_COUNT 4u
#define TSP_CAP_COUNT 3u
#define TSP_EDGE_OFF_MIN (-7)
#define TSP_EDGE_OFF_COUNT 16u
#define TSP_EDGE_SLOPE_COUNT 8u

#define TSP_ATTR_FLIPX   0x0200u
#define TSP_ATTR_FLIPY   0x0400u
#define TSP_ATTR_PALETTE 0x0800u
#define TSP_TILE_ID_MASK 0x01ffu

#define TSP_TILE_CEILING 0u
#define TSP_TILE_FLOOR   1u
#define TSP_TILE_HORIZON 2u
#define TSP_TILE_FULL_BASE 3u
#define TSP_TILE_EDGE_BASE (TSP_TILE_FULL_BASE + TSP_SHADE_COUNT*TSP_CAP_COUNT*TSP_BORDER_COUNT)
#define TSP_GENERATED_TILE_COUNT (TSP_TILE_EDGE_BASE + TSP_SHADE_COUNT*TSP_EDGE_OFF_COUNT*TSP_EDGE_SLOPE_COUNT)

#define TSP_CAP_NONE   0u
#define TSP_CAP_TOP    1u
#define TSP_CAP_BOTTOM 2u
#define TSP_TILE_FULL(shade,cap,border) \
    ((uint16_t)(TSP_TILE_FULL_BASE + ((((shade)*TSP_CAP_COUNT)+(cap))*TSP_BORDER_COUNT)+(border)))
#define TSP_TILE_EDGE(shade,off_index,slope_index) \
    ((uint16_t)(TSP_TILE_EDGE_BASE + ((((shade)*TSP_EDGE_OFF_COUNT)+(off_index))*TSP_EDGE_SLOPE_COUNT)+(slope_index)))

typedef enum TSPProfile {
    TSP_PROFILE_FULL=0,
    TSP_PROFILE_LINTEL=1,
    TSP_PROFILE_RAISED=2,
    TSP_PROFILE_RISER=3
} TSPProfile;

typedef struct TSPState {
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
} TSPState;

typedef struct TSPColumn {
    uint8_t invz;
    uint8_t wall_id;
    uint8_t shade;
    uint8_t top;
    uint8_t bottom;
    int8_t top_step;
    int8_t bottom_step;
} TSPColumn;

extern volatile uint8_t g_tspf_stage;
extern volatile uint8_t g_tspf_active_runs;
extern volatile uint8_t g_tspf_selector_tests;
extern volatile uint16_t g_tspf_touched_cells;
extern volatile uint8_t g_tspf_appearance_mode;

void tsp_reset(TSPState *s);
void tsp_step(TSPState *s,uint8_t input);
uint8_t tsp_is_walkable_q4(int16_t x_q4,int16_t y_q4);
void tsp_polar_renderer_reset(void) BANKED;
void tsp_polar_render(const TSPState *s,uint16_t out_map[TSP_MAP_CELLS],TSPColumn cols[TSP_COLS]) BANKED;

#endif
