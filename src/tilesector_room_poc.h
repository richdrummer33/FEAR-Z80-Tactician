#ifndef TILESECTOR_ROOM_POC_H
#define TILESECTOR_ROOM_POC_H

#include <stdint.h>
#include "tilesector_world_module.h"

#define TSP_ROOM_POC_COUNT 10u
#define TSP_ROOM_POC_MAX_WALK_RECTS 4u
#define TSP_ROOM_POC_MAX_BLOCK_RECTS 2u
#define TSP_ROOM_POC_MAX_FLOOR_ZONES 5u
#define TSP_ROOM_POC_MAX_PORTALS 3u

typedef struct TSPRoomPocRect {
    int16_t x0_q4,y0_q4,x1_q4,y1_q4;
} TSPRoomPocRect;

typedef struct TSPRoomPocFloorZone {
    TSPRoomPocRect rect;
    int16_t z_q4;
} TSPRoomPocFloorZone;

typedef struct TSPRoomPocPortal {
    int16_t x_q4,y_q4;
    uint8_t dir;
    uint8_t portal_index;
} TSPRoomPocPortal;

typedef struct TSPRoomPocDef {
    uint8_t asset_id;
    uint8_t topology_kind;
    uint8_t walk_count;
    uint8_t block_count;
    uint8_t floor_zone_count;
    uint8_t portal_count;
    int16_t default_floor_z_q4;
    int16_t spawn_x_q4,spawn_y_q4;
    TSPRoomPocRect walk[TSP_ROOM_POC_MAX_WALK_RECTS];
    TSPRoomPocRect block[TSP_ROOM_POC_MAX_BLOCK_RECTS];
    TSPRoomPocFloorZone floor_zone[TSP_ROOM_POC_MAX_FLOOR_ZONES];
    TSPRoomPocPortal portal[TSP_ROOM_POC_MAX_PORTALS];
} TSPRoomPocDef;

const TSPRoomPocDef *tsp_room_poc_get(uint8_t asset_id);
uint8_t tsp_room_poc_set_active(uint8_t asset_id);
const TSPRoomPocDef *tsp_room_poc_active(void);
uint8_t tsp_room_poc_is_walkable(const TSPRoomPocDef *room,int16_t x_q4,int16_t y_q4);
int16_t tsp_room_poc_floor_z(const TSPRoomPocDef *room,int16_t x_q4,int16_t y_q4);

#endif
