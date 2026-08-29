#ifndef POLAR_BAKED_COMPOSITE_H
#define POLAR_BAKED_COMPOSITE_H

#include <stdint.h>
#include "tilesector_polar.h"

#define TSP_HOST_TILE_BYTES 32u
#define TSP_HOST_MAX_FRAME_LOADS TSP_MAP_CELLS

typedef struct TSPHostTileLoad {
    uint16_t slot;
    uint8_t bytes[TSP_HOST_TILE_BYTES];
} TSPHostTileLoad;

/*
 * Host-only sub-tile compositor + persistent 512-slot VRAM cache model.
 *
 * The host resolves partial edge coverage at 8x8 pixel granularity. Final
 * patterns are canonicalized under H/V flips, then assigned to a simulated
 * Game Gear tile cache. Patterns retained between frames keep their slot;
 * newly needed patterns generate explicit pre-baked tile-pattern uploads.
 */
void tsp_host_composite_begin_frame(void);
void tsp_host_composite_write(uint8_t row,uint8_t col,uint16_t word);
void tsp_host_composite_surface(uint8_t col,uint8_t clip_x0,uint8_t clip_x1,
                                int16_t tl,int16_t tr,int16_t bl,int16_t br,
                                uint8_t shade,uint8_t border);
void tsp_host_composite_export(uint16_t out[TSP_MAP_CELLS]);

uint16_t tsp_host_composite_frame_load_count(void);
const TSPHostTileLoad *tsp_host_composite_frame_loads(void);
uint16_t tsp_host_composite_frame_unique_count(void);
uint16_t tsp_host_composite_peak_unique_count(void);
uint16_t tsp_host_composite_peak_load_count(void);
uint32_t tsp_host_composite_total_load_count(void);

#endif
