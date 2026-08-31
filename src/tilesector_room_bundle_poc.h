#ifndef TILESECTOR_ROOM_BUNDLE_POC_H
#define TILESECTOR_ROOM_BUNDLE_POC_H

#include <stdint.h>

#define TSP_ROOM_BUNDLE_ROUTE_MAX 6u

typedef struct TSPRoomBundleRoute {
    uint8_t entry_portal;
    uint8_t exit_portal;
    uint16_t first_patch;
    uint16_t patch_count;
} TSPRoomBundleRoute;

typedef struct TSPRoomBundle {
    uint8_t bundle_id;
    uint8_t route_count;
    uint8_t flags;
    uint8_t reserved;
    const TSPRoomBundleRoute *routes;
} TSPRoomBundle;

typedef struct TSPRoomBundlePlayer {
    const TSPRoomBundle *bundle;
    const TSPRoomBundleRoute *route;
    uint16_t local_patch;
} TSPRoomBundlePlayer;

void tsp_room_bundle_player_reset(TSPRoomBundlePlayer *p);
uint8_t tsp_room_bundle_begin(TSPRoomBundlePlayer *p,const TSPRoomBundle *bundle,
                              uint8_t entry_portal,uint8_t exit_portal);
uint8_t tsp_room_bundle_done(const TSPRoomBundlePlayer *p);
uint16_t tsp_room_bundle_global_patch(const TSPRoomBundlePlayer *p);
uint8_t tsp_room_bundle_advance(TSPRoomBundlePlayer *p);

#endif
