#include "tilesector_room_bundle_poc.h"

void tsp_room_bundle_player_reset(TSPRoomBundlePlayer *p){
    if(!p)return;
    p->bundle=(const TSPRoomBundle *)0;
    p->route=(const TSPRoomBundleRoute *)0;
    p->local_patch=0u;
}

uint8_t tsp_room_bundle_begin(TSPRoomBundlePlayer *p,const TSPRoomBundle *bundle,
                              uint8_t entry_portal,uint8_t exit_portal){
    uint8_t i;
    if(!p||!bundle||!bundle->routes)return 0u;
    for(i=0u;i<bundle->route_count;++i){
        const TSPRoomBundleRoute *r=&bundle->routes[i];
        if(r->entry_portal==entry_portal&&r->exit_portal==exit_portal&&r->patch_count){
            p->bundle=bundle;
            p->route=r;
            p->local_patch=0u;
            return 1u;
        }
    }
    return 0u;
}

uint8_t tsp_room_bundle_done(const TSPRoomBundlePlayer *p){
    if(!p||!p->route)return 1u;
    return (uint8_t)(p->local_patch>=p->route->patch_count);
}

uint16_t tsp_room_bundle_global_patch(const TSPRoomBundlePlayer *p){
    if(!p||!p->route||tsp_room_bundle_done(p))return 0xffffu;
    return (uint16_t)(p->route->first_patch+p->local_patch);
}

uint8_t tsp_room_bundle_advance(TSPRoomBundlePlayer *p){
    if(!p||!p->route||tsp_room_bundle_done(p))return 0u;
    ++p->local_patch;
    return 1u;
}
