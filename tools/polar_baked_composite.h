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

enum {
    TSP_HOST_LIGHT_BASELINE = 0u,
    TSP_HOST_LIGHT_AO = 1u,
    /* Correct world/portal hard cast, no soft edge. */
    TSP_HOST_LIGHT_HARD = 2u,
    /* Same hard cast plus one-sided ordered-dither penumbra. */
    TSP_HOST_LIGHT_POINT = 3u
};

/* Select one host-bake presentation layer and provide the exact camera state.
 * This API is host-only; the Game Gear runtime never sees a light record. */
void tsp_host_composite_set_lighting(uint8_t stage,const TSPState *camera);

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
                                uint8_t sid,uint8_t shade,uint8_t border,
                                uint8_t ao_left,uint8_t ao_right);
void tsp_host_composite_export(uint16_t out[TSP_MAP_CELLS]);

uint16_t tsp_host_composite_frame_load_count(void);
const TSPHostTileLoad *tsp_host_composite_frame_loads(void);
uint16_t tsp_host_composite_frame_unique_count(void);
uint16_t tsp_host_composite_peak_unique_count(void);
uint16_t tsp_host_composite_peak_load_count(void);
uint32_t tsp_host_composite_total_load_count(void);
/* Host-only semantic probes used to validate portal sidedness. */
uint16_t tsp_host_composite_owner_pixel_count(uint8_t sid);
uint16_t tsp_host_composite_lit_owner_pixel_count(uint8_t sid);
int tsp_host_composite_write_ppm(const char *path);

#endif
