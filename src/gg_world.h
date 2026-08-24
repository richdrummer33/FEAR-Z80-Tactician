#ifndef GG_CQB_GG_WORLD_H
#define GG_CQB_GG_WORLD_H

#include <stdint.h>
#include "sim.h"

#define GG_WORLD_BLOCK_W ((SIM_W + 1u) / 2u)
#define GG_WORLD_BLOCK_H ((SIM_H + 1u) / 2u)
#define GG_WORLD_TILE_Y 2u
#define GG_WORLD_PATTERN_CAP 32u

void gg_world_reset_patterns(void);
uint8_t gg_world_pattern_key(const Sim *sim, uint8_t bx, uint8_t by);
void gg_world_make_pattern(uint8_t key, uint8_t *tile);
uint16_t gg_world_pattern_for_block(const Sim *sim, uint8_t bx, uint8_t by);
void gg_world_make_block_tile(const Sim *sim, uint8_t bx, uint8_t by, uint8_t *tile);
void gg_world_upload_block(const Sim *sim, uint8_t bx, uint8_t by);
void gg_world_upload_all(const Sim *sim);

#endif
