#ifndef POLAR_BAKED_COMPOSITE_H
#define POLAR_BAKED_COMPOSITE_H

#include <stdint.h>
#include "tilesector_polar.h"

/*
 * Host-only sub-tile compositor used by the baked patch generator.
 *
 * The renderer still computes visibility/projection in its normal coarse
 * name-table vocabulary. Every put_cell() is mirrored here before the opaque
 * name-table write happens. Full tiles replace a cell. Edge tiles contribute
 * only their wall/black coverage; the nominal "outside" pixels are transparent
 * and preserve the farther surface already present in the host pixel cell.
 *
 * Export then deduplicates the resulting 8x8 semantic-color patterns (including
 * H/V flip equivalence) into a <=512-tile dictionary for the Game Gear.
 */
void tsp_host_composite_begin_frame(void);
void tsp_host_composite_write(uint8_t row,uint8_t col,uint16_t word);
void tsp_host_composite_export(uint16_t out[TSP_MAP_CELLS]);
uint16_t tsp_host_composite_tile_count(void);
int tsp_host_composite_emit_tiles(const char *dir);

#endif
