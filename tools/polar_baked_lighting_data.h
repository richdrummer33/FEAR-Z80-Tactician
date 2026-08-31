#ifndef POLAR_BAKED_LIGHTING_DATA_H
#define POLAR_BAKED_LIGHTING_DATA_H

#include <stdint.h>

/*
 * Host-only lighting authoring data. None of this is linked into the GG
 * runtime; the cartridge still replays only precomputed tile/name patches.
 */
typedef struct TSPHostStaticLight {
    int16_t x_q4;
    int16_t y_q4;
    uint8_t height_q4;
    uint8_t radius_world;
    uint8_t intensity;
    uint8_t wall_angle_response;
    uint8_t view_term_strength;
} TSPHostStaticLight;

static const TSPHostStaticLight k_tsp_host_static_lights[] = {
    { (int16_t)(92 << 4), (int16_t)(50 << 4), 8u << 4, 76u, 255u, 1u, 2u }
};

#define TSP_HOST_STATIC_LIGHT_COUNT \
    ((uint8_t)(sizeof(k_tsp_host_static_lights)/sizeof(k_tsp_host_static_lights[0])))

enum {
    TSP_HOST_PROFILE_FULL   = 0u,
    TSP_HOST_PROFILE_LINTEL = 1u,
    TSP_HOST_PROFILE_RAISED = 2u,
    TSP_HOST_PROFILE_RISER  = 3u,
    TSP_HOST_PROFILE_WINDOW_SILL = 4u,
    TSP_HOST_PROFILE_WINDOW_REVEAL = 5u
};

typedef struct TSPHostWorldVertex {
    int16_t x;
    int16_t y;
} TSPHostWorldVertex;

/*
 * front_sign is measured against the directed segment's right-hand normal
 * (dy,-dx): +1 = that side, -1 = the opposite side, 0 = two-sided.
 *
 * light_front_sign applies only to RECEIVING the point light.
 * visual_front_sign applies to visibility itself. The Room-2 threshold riser
 * is front-visible only from the hallway because Room-2's virtual raised floor
 * is flush with its top edge.
 *
 * Every segment, including portal profiles, can now block light. Vertical
 * blocking extent comes from profile:
 *   FULL   z 0..32
 *   RAISED z 4..32
 *   LINTEL z 24..32
 *   RISER  z 0..4
 * These values are exactly the world-space heights implied by the existing
 * projection formulas with camera z=16.
 */
typedef struct TSPHostWorldSegment {
    uint8_t v0;
    uint8_t v1;
    uint8_t profile;
    uint8_t blocks_light;
    int8_t light_front_sign;
    int8_t visual_front_sign;
} TSPHostWorldSegment;

static const TSPHostWorldVertex k_tsp_host_world_vertices[] = {
    {16,16},{80,16},{80,36},{80,64},{80,80},{16,80},{112,36},
    {112,64},{112,14},{112,84},{136,6},{154,20},{176,10},{176,84}
};

static const TSPHostWorldSegment k_tsp_host_world_segments[] = {
    /* Room A + hallway opaque FULL walls. */
    {0,1,TSP_HOST_PROFILE_FULL,1,0,0},
    {1,2,TSP_HOST_PROFILE_FULL,1,0,0},
    {3,4,TSP_HOST_PROFILE_FULL,1,0,0},
    {4,5,TSP_HOST_PROFILE_FULL,1,0,0},
    {5,0,TSP_HOST_PROFILE_FULL,1,0,0},
    {2,6,TSP_HOST_PROFILE_FULL,1,0,0},
    {7,3,TSP_HOST_PROFILE_FULL,1,0,0},

    /* Room B walls begin at its raised floor z=4. */
    {8,6,TSP_HOST_PROFILE_RAISED,1,0,0},
    {7,9,TSP_HOST_PROFILE_RAISED,1,0,0},
    {8,10,TSP_HOST_PROFILE_RAISED,1,0,0},
    {10,11,TSP_HOST_PROFILE_RAISED,1,0,0},
    {11,12,TSP_HOST_PROFILE_RAISED,1,0,0},
    {12,13,TSP_HOST_PROFILE_RAISED,1,0,0},
    {13,9,TSP_HOST_PROFILE_RAISED,1,0,0},

    /*
     * Portal/profile planes.
     * x=80 portal faces hallway (+x). x=112 portal faces hallway (-x).
     * Lintels remain visually two-sided but only the hallway-facing material
     * receives this hallway light. The x=112 RISER is visually one-sided:
     * from Room B it is buried below the virtual raised floor.
     */
    {2,3,TSP_HOST_PROFILE_LINTEL,1, 1, 0},
    {6,7,TSP_HOST_PROFILE_LINTEL,1,-1, 0},
    {6,7,TSP_HOST_PROFILE_RISER, 1,-1,-1}
};

#define TSP_HOST_WORLD_VERTEX_COUNT \
    ((uint8_t)(sizeof(k_tsp_host_world_vertices)/sizeof(k_tsp_host_world_vertices[0])))
#define TSP_HOST_WORLD_SEGMENT_COUNT \
    ((uint8_t)(sizeof(k_tsp_host_world_segments)/sizeof(k_tsp_host_world_segments[0])))

#endif
