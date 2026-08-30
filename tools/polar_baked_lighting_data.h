#ifndef POLAR_BAKED_LIGHTING_DATA_H
#define POLAR_BAKED_LIGHTING_DATA_H

#include <stdint.h>

/*
 * Host-only lighting authoring data.
 *
 * These records are consumed by the PC bake and deliberately never linked into
 * the Game Gear runtime. The point light sits just beyond Room A's east opening
 * so the doorway/corners make the world-space visibility boundary obvious.
 */
typedef struct TSPHostStaticLight {
    int16_t x_q4;
    int16_t y_q4;
    uint8_t height_q4;
    uint8_t radius_world;
    uint8_t intensity;
} TSPHostStaticLight;

static const TSPHostStaticLight k_tsp_host_static_lights[] = {
    { (int16_t)(92 << 4), (int16_t)(50 << 4), 8u << 4, 76u, 255u }
};

#define TSP_HOST_STATIC_LIGHT_COUNT \
    ((uint8_t)(sizeof(k_tsp_host_static_lights)/sizeof(k_tsp_host_static_lights[0])))

/*
 * Authoritative demo world topology, mirrored from
 * experiments/adaptive_polar_field/polar_field_demo_v1.py.
 *
 * Segment IDs 0..13 are the same opaque SOLIDS used by the polar visibility
 * oracle. 14..16 are portal/profile surfaces: they can receive light but do not
 * occlude the light in this first hard-shadow experiment.
 *
 * Keeping this host-only is intentional. It is lighting-bake input, not a new
 * runtime geometry dependency.
 */
typedef struct TSPHostWorldVertex {
    int16_t x;
    int16_t y;
} TSPHostWorldVertex;

typedef struct TSPHostWorldSegment {
    uint8_t v0;
    uint8_t v1;
    uint8_t blocks_light;
} TSPHostWorldSegment;

static const TSPHostWorldVertex k_tsp_host_world_vertices[] = {
    {16,16},{80,16},{80,36},{80,64},{80,80},{16,80},{112,36},
    {112,64},{112,14},{112,84},{136,6},{154,20},{176,10},{176,84}
};

static const TSPHostWorldSegment k_tsp_host_world_segments[] = {
    {0,1,1},{1,2,1},{3,4,1},{4,5,1},{5,0,1},{2,6,1},{7,3,1},
    {8,6,1},{7,9,1},{8,10,1},{10,11,1},{11,12,1},{12,13,1},{13,9,1},
    {2,3,0},{6,7,0},{6,7,0}
};

#define TSP_HOST_WORLD_VERTEX_COUNT \
    ((uint8_t)(sizeof(k_tsp_host_world_vertices)/sizeof(k_tsp_host_world_vertices[0])))
#define TSP_HOST_WORLD_SEGMENT_COUNT \
    ((uint8_t)(sizeof(k_tsp_host_world_segments)/sizeof(k_tsp_host_world_segments[0])))

#endif
