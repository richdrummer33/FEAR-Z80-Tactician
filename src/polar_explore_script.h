#ifndef POLAR_EXPLORE_SCRIPT_H
#define POLAR_EXPLORE_SCRIPT_H

#include <stdint.h>
#include "tilesector_polar.h"

/*
 * ~23 s player-like traversal at the measured patch-player cadence.
 *
 * Intent:
 * - start with a small "look around before moving" pan
 * - sweep Room A's north side instead of beelining to the exit
 * - pause / strafe-peek around the first portal
 * - inspect the connector instead of sprinting through it
 * - sweep high and low corners of Room B
 * - strafe-peek again on the return
 * - come back through the connector on a different lateral line
 * - finish looking around Room A rather than freezing mid-run
 *
 * Inputs are ordinary Polar manual controls.  The host baker and the GG
 * patch-player both consume this exact script, so camera state is not faked
 * independently from the game's own tsp_step() motion/collision code.
 */
typedef struct PolarExploreSeg {
    uint16_t frames;
    uint8_t input;
} PolarExploreSeg;

static const PolarExploreSeg k_polar_explore_script[] = {
    /* Room A: orient, cross, then deliberately visit the north side. */
    {14u, TSP_INPUT_RIGHT},
    {14u, TSP_INPUT_LEFT},
    {42u, TSP_INPUT_UP},
    {28u, 0u},
    {22u, TSP_INPUT_LEFT},
    {35u, TSP_INPUT_UP},
    {28u, 0u},
    {22u, TSP_INPUT_RIGHT},
    {12u, 0u},
    {18u, TSP_INPUT_UP},
    {20u, 0u},

    /* Slide down the east wall and "slice the pie" around Portal A. */
    {28u, TSP_INPUT_STRAFE_RIGHT},
    {20u, 0u},
    {12u, TSP_INPUT_STRAFE_LEFT},
    {8u,  0u},
    {20u, TSP_INPUT_STRAFE_RIGHT},
    {14u, TSP_INPUT_STRAFE_LEFT},
    {44u, TSP_INPUT_UP},
    {24u, 0u},

    /* Connector: stop, inspect both directions, then continue. */
    {18u, TSP_INPUT_RIGHT},
    {34u, TSP_INPUT_LEFT},
    {16u, TSP_INPUT_RIGHT},
    {10u, 0u},
    {52u, TSP_INPUT_UP},
    {24u, 0u},

    /* Room B: north sweep, east-side look, then south sweep. */
    {20u, TSP_INPUT_LEFT},
    {34u, TSP_INPUT_UP},
    {24u, 0u},
    {20u, TSP_INPUT_RIGHT},
    {30u, TSP_INPUT_UP},
    {24u, 0u},
    {22u, TSP_INPUT_RIGHT},
    {48u, TSP_INPUT_UP},
    {28u, 0u},
    {22u, TSP_INPUT_RIGHT},
    {44u, TSP_INPUT_UP},
    {24u, 0u},

    /* Return path: lateral peek, then head back on a different line. */
    {16u, TSP_INPUT_STRAFE_LEFT},
    {10u, 0u},
    {20u, TSP_INPUT_STRAFE_RIGHT},
    {12u, TSP_INPUT_STRAFE_LEFT},
    {56u, TSP_INPUT_UP},
    {26u, 0u},

    /* End in Room A with a final deliberate look-around. */
    {18u, TSP_INPUT_RIGHT},
    {20u, TSP_INPUT_RIGHT},
    {16u, 0u}
};

#define POLAR_EXPLORE_SEG_COUNT ((uint8_t)(sizeof(k_polar_explore_script)/sizeof(k_polar_explore_script[0])))
#define POLAR_EXPLORE_FRAMES 1113u

typedef struct PolarExploreCursor {
    uint8_t seg;
    uint16_t left;
} PolarExploreCursor;

static void polar_explore_cursor_reset(PolarExploreCursor *c) {
    c->seg = 0u;
    c->left = k_polar_explore_script[0].frames;
}

static uint8_t polar_explore_next(PolarExploreCursor *c) {
    uint8_t input;
    if(c->seg >= POLAR_EXPLORE_SEG_COUNT) return 0u;
    input = k_polar_explore_script[c->seg].input;
    if(c->left) --c->left;
    if(!c->left) {
        ++c->seg;
        if(c->seg < POLAR_EXPLORE_SEG_COUNT)
            c->left = k_polar_explore_script[c->seg].frames;
    }
    return input;
}

#endif
