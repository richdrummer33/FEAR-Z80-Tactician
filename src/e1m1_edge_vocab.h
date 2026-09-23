#ifndef E1M1_EDGE_VOCAB_H
#define E1M1_EDGE_VOCAB_H
#include <stdint.h>
/* Direct-physical-endpoint vocabulary.
 *
 * Run 124 used exact edge magnitudes 0..28 and filled every pattern slot below
 * the 0x3800 name table. The direct-endpoint experiment needs arbitrary-X
 * vertical corner lines, so clamp the extreme tail to 0..24 (still above the
 * measured p95 |dy|=13). This releases exactly 36 physical pattern IDs.
 *
 * IDs 412..447 encode every one-line and two-line vertical mask in an 8-pixel
 * tile: 8 singles followed by C(8,2)=28 pairs. That is enough for both
 * physical endpoints of a one-column run without any post-pass compositor. */
#define TSP_P99_EDGE_MAX 24u
#define TSP_TILE_FULL_COMPACT_BASE 3u
#define TSP_DIRECT_SEAM_BASE 412u
#define TSP_DIRECT_SEAM_SINGLE_COUNT 8u
#define TSP_DIRECT_SEAM_PAIR_COUNT 28u
#define TSP_DIRECT_SEAM_COUNT (TSP_DIRECT_SEAM_SINGLE_COUNT+TSP_DIRECT_SEAM_PAIR_COUNT)
#define TSP_P99_TILE_COUNT 448u
/* 9-bit tile IDs packed as low bytes plus a high-bit bitmap.
 * Edge row = 37 low bytes + 5 bitmap bytes. Border table = 128 + 16. */
extern const uint8_t g_tsp_edge_p99_packed_home[1050];
extern const uint8_t g_tsp_edge_border_b1_packed_home[144];
extern const uint8_t g_tsp_edge_border_b2_packed_home[144];
#endif
