#ifndef E1M1_EDGE_VOCAB_H
#define E1M1_EDGE_VOCAB_H
#include <stdint.h>
/* Direct-mixed geometry vocabulary.
 *
 * The steep-edge census measured p95 |dy|=13, p99=28 and max=36 pixels per
 * 8px coarse column. R124 proved exact 0..28, but consumed every pattern slot.
 * This rung keeps exact 0..24 (well above p95), freeing exactly 36 IDs.
 * Static geometry ends at tile 411. Direct mixed uses three permanent
 * internal-line body templates at 412..414 and two transient banks at
 * 415..431 (17) and 432..447 (16). No seam overlay pass exists: only critical
 * boundary cells are authored directly from the baked envelope. */
#define TSP_P99_EDGE_MAX 24u
#define TSP_TILE_FULL_COMPACT_BASE 3u
#define TSP_P99_TILE_COUNT 412u
/* 9-bit tile IDs packed as low bytes plus a high-bit bitmap.
 * Edge row = 37 low bytes + 5 bitmap bytes. Border table = 128 + 16. */
extern const uint8_t g_tsp_edge_p99_packed_home[1050];
extern const uint8_t g_tsp_edge_border_b1_packed_home[144];
extern const uint8_t g_tsp_edge_border_b2_packed_home[144];
#endif
