#ifndef E1M1_EDGE_VOCAB_H
#define E1M1_EDGE_VOCAB_H
#include <stdint.h>
/* Thin-face geometry vocabulary.
 *
 * The steep-edge census measured p95 |dy|=13, p99=28 and max=36 pixels per
 * 8px coarse column. R124 proved exact 0..28, but consumed every pattern slot.
 * This rung keeps exact 0..24 (well above p95), freeing 36 IDs. Twenty of those
 * are spent on canonical one/two-line sub-column seam masks; HFLIP supplies
 * their reflected forms. The exact-projection census then found only nine
 * canonical 3+ line masks across rotate/strafe/long-sightline sweeps, so those
 * consume nine of the sixteen still-free pattern slots. Total physical
 * patterns: 441, leaving seven slots below the 0x3800 name-table boundary. */
#define TSP_P99_EDGE_MAX 24u
#define TSP_TILE_FULL_COMPACT_BASE 3u
#define TSP_TILE_SEAM_BASE 412u
#define TSP_SEAM_MASK_COUNT 29u
#define TSP_P99_TILE_COUNT 441u
/* 9-bit tile IDs packed as low bytes plus a high-bit bitmap.
 * Edge row = 37 low bytes + 5 bitmap bytes. Border table = 128 + 16. */
extern const uint8_t g_tsp_edge_p99_packed_home[1050];
extern const uint8_t g_tsp_edge_border_b1_packed_home[144];
extern const uint8_t g_tsp_edge_border_b2_packed_home[144];
extern const uint8_t g_tsp_seam_mask_home[TSP_SEAM_MASK_COUNT];
extern const uint8_t g_tsp_seam_reflect_home[TSP_SEAM_MASK_COUNT];
#endif
