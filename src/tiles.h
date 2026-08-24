#ifndef GG_CQB_TILES_H
#define GG_CQB_TILES_H

#include <stdint.h>

#define TILE_SOLID_0 0u
#define TILE_FLOOR 1u
#define TILE_WALL 2u
#define TILE_DOOR_CLOSED 3u
#define TILE_DOOR_OPEN 4u
#define TILE_BLUE 5u
#define TILE_RED 6u
#define TILE_HIT 7u
#define TILE_TRACER 8u
#define TILE_BLUE_DIM 9u
#define TILE_RED_DIM 10u
#define TILE_TEXT_DIM 11u
#define TILE_DOOR_FLASH 12u
#define TILE_SUPPRESSED 13u
#define TILE_SOLID_14 14u
#define TILE_SOLID_15 15u
#define TILE_FONT_SPACE 16u
#define TILE_FONT_A 17u
#define TILE_FONT_B 18u
#define TILE_FONT_C 19u
#define TILE_FONT_H 20u
#define TILE_FONT_P 21u
#define TILE_FONT_R 22u
#define TILE_FONT_T 23u
#define TILE_FONT_0 24u
#define TILE_FONT_1 25u
#define TILE_FONT_2 26u
#define TILE_FONT_3 27u
#define TILE_FONT_4 28u
#define TILE_FONT_5 29u
#define TILE_FONT_6 30u
#define TILE_FONT_7 31u
#define TILE_FONT_8 32u
#define TILE_FONT_9 33u

#define GG_TILE_COUNT 34u
#define GG_TILE_BYTES 1088u

extern const uint8_t gg_tile_data[GG_TILE_BYTES];
uint8_t gg_font_tile(char c);

#endif
