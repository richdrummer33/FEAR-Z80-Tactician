#ifndef TILESECTOR_WORLD_STREAM_POC_H
#define TILESECTOR_WORLD_STREAM_POC_H

#include <stdint.h>
#include "tilesector_world_module.h"

#define TSP_STREAM_BREADCRUMB_MAX 64u

typedef struct TSPStreamNodeDesc {
    uint32_t key;
    uint8_t module_kind;
    uint8_t asset_variant;
    uint8_t rotation;
    int16_t logical_floor_q4;
} TSPStreamNodeDesc;

typedef struct TSPStreamBreadcrumb {
    TSPStreamNodeDesc node;
    uint8_t exit_taken;
} TSPStreamBreadcrumb;

typedef struct TSPStreamWorld {
    uint32_t seed;
    TSPStreamNodeDesc current;
    uint8_t depth;
    TSPStreamBreadcrumb breadcrumbs[TSP_STREAM_BREADCRUMB_MAX];
} TSPStreamWorld;

uint32_t tsp_stream_mix32(uint32_t x);
uint32_t tsp_stream_root_key(uint32_t seed);
uint32_t tsp_stream_child_key(uint32_t seed,uint32_t parent_key,uint8_t exit_index);
TSPModuleKind tsp_stream_choose_kind(uint32_t seed,uint32_t node_key,uint8_t allow_vertical);
void tsp_stream_describe(uint32_t seed,uint32_t node_key,uint8_t rotation,
                         int16_t logical_floor_q4,uint8_t allow_vertical,
                         TSPStreamNodeDesc *out);
void tsp_stream_reset(TSPStreamWorld *w,uint32_t seed,uint8_t allow_vertical);
uint8_t tsp_stream_enter(TSPStreamWorld *w,uint8_t exit_index,uint8_t allow_vertical);
uint8_t tsp_stream_back(TSPStreamWorld *w);

#endif
