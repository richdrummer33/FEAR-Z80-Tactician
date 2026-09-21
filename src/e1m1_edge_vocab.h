#ifndef E1M1_EDGE_VOCAB_H
#define E1M1_EDGE_VOCAB_H
#include <stdint.h>
/* P99 geometry-only wall vocabulary.
 * The measured spin trace put 21.8% of FULL top edges beyond the old +/-7
 * slope cap, with p95 |dy|=13, p99=28 and worst=36 pixels per 8px column.
 * This build specializes the visible material to MID, compacts FULL to 12
 * patterns, and spends the reclaimed pattern IDs on exact slopes 0..28.
 * 448 patterns end exactly at VRAM 0x3800, leaving the name table untouched. */
#define TSP_P99_EDGE_MAX 28u
#define TSP_TILE_FULL_COMPACT_BASE 3u
#define TSP_P99_TILE_COUNT 448u
/* 9-bit tile IDs packed as low bytes plus a high-bit bitmap.
 * Edge row = 37 low bytes + 5 bitmap bytes. Border table = 128 + 16. */
extern const uint8_t g_tsp_edge_p99_packed_home[1218];
extern const uint8_t g_tsp_edge_border_b1_packed_home[144];
extern const uint8_t g_tsp_edge_border_b2_packed_home[144];
#endif
