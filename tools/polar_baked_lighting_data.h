#ifndef POLAR_BAKED_LIGHTING_DATA_H
#define POLAR_BAKED_LIGHTING_DATA_H

#include <stdint.h>

/*
 * Host-only lighting authoring data.
 *
 * These records are consumed by the PC bake and deliberately never linked into
 * the Game Gear runtime.  The current experiment places one wall/portal light
 * just beyond Room A's east opening so the doorway silhouette can cast strong,
 * readable shadows back into the room.
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

#endif
