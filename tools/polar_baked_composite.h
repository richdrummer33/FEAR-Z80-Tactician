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

typedef struct TSPHostSceneVertex {
    int16_t x;
    int16_t y;
} TSPHostSceneVertex;

typedef struct TSPHostSceneSegment {
    uint8_t v0;
    uint8_t v1;
    uint8_t profile;
    uint8_t blocks_light;
    int8_t light_front_sign;
    int8_t visual_front_sign;
} TSPHostSceneSegment;

typedef struct TSPHostSceneLight {
    int16_t x_q4;
    int16_t y_q4;
    uint8_t height_q4;
    uint8_t radius_world;
    uint8_t intensity;
    /* Host-bake wall response. When enabled, illumination is quantized from
     * wall normal vs wall-center-to-light. view_term_strength adds at most
     * that many of 15 quantization steps from reflected-light/view alignment. */
    uint8_t wall_angle_response;
    uint8_t view_term_strength;
} TSPHostSceneLight;

/* Horizontal receiver volume. x/y bounds are inclusive for host ray tests.
 * The floor/ceiling pair lets room-local bakes retain raised/sunken spaces
 * without hard-coding one global world polygon into the compositor. */
typedef struct TSPHostSceneRect {
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
    int16_t floor_z;
    int16_t ceiling_z;
} TSPHostSceneRect;

typedef struct TSPHostCompositeScene {
    const TSPHostSceneVertex *vertices;
    uint8_t vertex_count;
    const TSPHostSceneSegment *segments;
    uint8_t segment_count;
    const TSPHostSceneLight *lights;
    uint8_t light_count;
    const TSPHostSceneRect *rects;
    uint8_t rect_count;
} TSPHostCompositeScene;

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

/* A/B diagnostic control. Enabled by default. Disabling this restores the
 * previous binary ambient/+1 point-light response while retaining occlusion. */
void tsp_host_composite_set_wall_angle_mode(uint8_t enabled);

enum {
    TSP_HOST_LIGHT_QUANT_DITHER16=0u,
    TSP_HOST_LIGHT_QUANT_SOLID8=1u
};
/* DITHER16 preserves the previous palette transform and is the control-friendly
 * mode. SOLID8 uses eight true wall colours and no spatial halftone. */
void tsp_host_composite_set_wall_quant_mode(uint8_t mode);

/* Optional room-local host scene. NULL restores the original static Polar
 * lighting scene. This never exists on the Game Gear runtime path. */
void tsp_host_composite_set_scene(const TSPHostCompositeScene *scene);

/*
 * Host-only sub-tile compositor + persistent 512-slot VRAM cache model.
 *
 * The host resolves partial edge coverage at 8x8 pixel granularity. Final
 * patterns are canonicalized under H/V flips, then assigned to a simulated
 * Game Gear tile cache. Patterns retained between frames keep their slot;
 * newly needed patterns generate explicit pre-baked tile-pattern uploads.
 */
/* Reset the simulated VRAM cache to the canonical permanent base tiles.
 * Room-bundle bakes call this before each independent bundle so no route
 * inherits dynamic slot state from a previous room. */
void tsp_host_composite_reset_cache(void);
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
