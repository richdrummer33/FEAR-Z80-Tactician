#ifndef E1M1_EDGE_VOCAB_H
#define E1M1_EDGE_VOCAB_H
#include <stdint.h>
/* R98 compact FULL-edge vocabulary.
 * 128 semantic (offset,slope-magnitude) cases collapse to 80 actual pixels.
 * The combined edge+vertical-border space (border L/R/both) collapses from
 * 384 semantic cases to 232 actual pixels. */
#define TSP_EDGE_UNIQUE_COUNT 80u
#define TSP_EDGE_BORDER_UNIQUE_COUNT 156u
#define TSP_TILE_EDGE_COMPACT_BASE 39u
#define TSP_TILE_EDGE_COMPACT_SHADE_STRIDE 80u
#define TSP_TILE_EDGE_BORDER_BASE 279u
#define TSP_EDGE_COMPACT_TILE_COUNT 435u
extern const uint8_t g_tsp_edge_unique_idx_home[128];
extern const uint8_t g_tsp_edge_border_b1_home[128];
extern const uint8_t g_tsp_edge_border_b2_home[128];
#endif
