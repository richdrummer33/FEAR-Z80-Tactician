#ifndef TILESECTOR_WORLD_MODULE_H
#define TILESECTOR_WORLD_MODULE_H

#include <stdint.h>

/*
 * Compact deterministic streaming grammar.
 *
 * Directions are quarter-turns in the same sense as yaw:
 *   0 east, 1 south, 2 west, 3 north.
 *
 * Stair modules are deliberately QUARTER turns, not monolithic spirals.
 * Each authored geometry block can expose only a short 2-3-step run plus a
 * landing and central shaft/core wall. Four compatible quarter modules make
 * one revolution while the core self-occludes old/new risers.
 */
typedef enum TSPModuleKind {
    TSP_MODULE_HALL_STRAIGHT=0,
    TSP_MODULE_TURN_LEFT,
    TSP_MODULE_TURN_RIGHT,
    TSP_MODULE_ROOM_SPLIT,
    TSP_MODULE_T_SPLIT,
    TSP_MODULE_STAIR_QUARTER_UP_LEFT,
    TSP_MODULE_STAIR_QUARTER_UP_RIGHT,
    TSP_MODULE_STAIR_QUARTER_DOWN_LEFT,
    TSP_MODULE_STAIR_QUARTER_DOWN_RIGHT,
    TSP_MODULE_KIND_COUNT
} TSPModuleKind;

#define TSP_MODULE_MAX_EXITS 3u
#define TSP_MODULE_STAIR_RISE_Q4 (4<<4)

typedef struct TSPModuleExit {
    /* Relative quarter-turn from incoming heading: -1 left, 0 forward, +1 right. */
    int8_t turn;
    int16_t dz_q4;
} TSPModuleExit;

typedef struct TSPModuleTopology {
    uint8_t exit_count;
    TSPModuleExit exits[TSP_MODULE_MAX_EXITS];
} TSPModuleTopology;

/* Deterministically choose a compatible next geometry block from tiny state.
 * node_key can be a path/chunk index or a branch-derived key.
 * allow_vertical=0 folds stair rolls back into ordinary turns. */
TSPModuleKind tsp_module_choose(uint16_t seed,uint16_t node_key,uint8_t allow_vertical);

/* Resolve one local exit into an absolute heading and floor-height delta.
 * Returns 0 for an invalid kind/index. */
uint8_t tsp_module_exit(TSPModuleKind kind,uint8_t incoming_dir,uint8_t exit_index,
                        uint8_t *out_dir,int16_t *out_dz_q4);

const TSPModuleTopology *tsp_module_topology(TSPModuleKind kind);

#endif
