#ifndef TILESECTOR_POLAR_H
#define TILESECTOR_POLAR_H

#include <stdint.h>

#ifndef TSPF_PROFILE_HOOKS
#define TSPF_PROFILE_HOOKS 1
#endif
#ifndef TSPF_DIRECT_MIXED
#define TSPF_DIRECT_MIXED 0
#endif
#ifndef TSPF_MIX_ELIDE_CONNECTED
/* Visual/perf affordance: internal vertices of a simultaneously-visible
 * connected wall chain keep their exact ownership/top-edge handoff but omit
 * the vertical black crease. Set to 0 for an immediate A/B build. */
#define TSPF_MIX_ELIDE_CONNECTED 1
#endif

#if defined(__SDCC)
#include <gbdk/platform.h>
#else
#ifndef BANKED
#define BANKED
#endif
#ifndef NONBANKED
#define NONBANKED
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

#if TSPF_DIRECT_MIXED && defined(TSPF_E1M1_P99_EDGE_VOCAB) && TSPF_E1M1_P99_EDGE_VOCAB
/* IDs 412..447 are the only pattern space before the 0x3800 name table.
 * Three permanent MID-wall internal-line templates cover split pixels 1..6
 * using H-flip pairs. Pixel 7 reuses the ordinary FULL right-border tile.
 * The remaining 33 IDs form asymmetric 17/16 transient banks. */
#define TSP_MIX_LINE_BASE        412u
#define TSP_MIX_LINE_COUNT       3u
#define TSP_MIX_BASE0            415u
#define TSP_MIX_SLOTS0           17u
#define TSP_MIX_BASE1            432u
#define TSP_MIX_SLOTS1           16u
#define TSP_MIX_SLOTS_MAX        17u
#endif

#define TSP_TILE_CEILING 0u
#define TSP_TILE_FLOOR   1u
#define TSP_TILE_HORIZON 2u
#define TSP_TILE_FULL_BASE 3u
#define TSP_TILE_EDGE_BASE (TSP_TILE_FULL_BASE + TSP_SHADE_COUNT*TSP_CAP_COUNT*TSP_BORDER_COUNT)
#if defined(TSPF_E1M1_P99_EDGE_VOCAB) && TSPF_E1M1_P99_EDGE_VOCAB
/* p24 static geometry occupies IDs 0..411. Direct mixed cells own 412..447. */
#define TSP_GENERATED_TILE_COUNT 412u
#elif defined(TSPF_E1M1_EDGE_VOCAB) && TSPF_E1M1_EDGE_VOCAB
#define TSP_GENERATED_TILE_COUNT 435u
#else
#define TSP_GENERATED_TILE_COUNT (TSP_TILE_EDGE_BASE + TSP_SHADE_COUNT*TSP_EDGE_OFF_COUNT*TSP_EDGE_SLOPE_COUNT)
#endif

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

#if defined(TSPF_OPTIMIZED_MAP)
#if defined(TSPF_E1M1_FULL_ONLY)
#define TSP_OPT_EYE_Q4 (5<<4)
#else
#define TSP_OPT_EYE_Q4 (16<<4)
#endif
#endif

typedef struct TSPState {
    int16_t x_q4;
    int16_t y_q4;
#if defined(TSPF_OPTIMIZED_MAP)
    int16_t z_q4;
#endif
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

#if defined(TSPF_E1M1_FRONT_ENVELOPE)
/* Native exact-envelope span geometry. DIRECT_MIXED consumes this exact same
 * solved run state: the critical cell changes ownership at true X, but each
 * side's Y plane must agree with the coarse run at the hardware-cell edges. */
typedef struct TSPPolarRun {
    uint8_t sid;
    uint8_t v0;
    uint8_t v1;
    uint8_t x0;
    uint8_t x1;
    uint8_t inv0;
    uint8_t inv1;
    uint8_t inv_mid;
    uint8_t left_real;
    uint8_t right_real;
    uint8_t c0;
    uint8_t c1;
    uint8_t depth_plane;
    uint8_t right_connected;
    int16_t iq;
    int16_t step;
} TSPPolarRun;
#if TSPF_DIRECT_MIXED
extern TSPPolarRun g_tspf_runs[];
extern uint8_t g_tspf_mixed_run_count;
#endif
#endif

extern volatile uint8_t g_tspf_stage;
extern volatile uint8_t g_tspf_active_runs;
extern volatile uint8_t g_tspf_selector_tests;
extern volatile uint16_t g_tspf_touched_cells;
extern volatile uint8_t g_tspf_appearance_mode;

void tsp_reset(TSPState *s);
void tsp_step(TSPState *s,uint8_t input);
uint8_t tsp_is_walkable_q4(int16_t x_q4,int16_t y_q4);
/* Reset is boot/cold-path work. Keep it in fixed HOME rather than spending
 * scarce renderer-bank bytes on code that never executes during a frame. */
void tsp_polar_renderer_reset(void) NONBANKED;
void tsp_polar_render(const TSPState *s,uint16_t out_map[TSP_MAP_CELLS],TSPColumn cols[TSP_COLS]) BANKED;

#endif
