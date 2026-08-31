#include <stdint.h>
#include <stdio.h>
#include "tilesector_room_bundle_poc.h"

int main(void){
    static const TSPRoomBundleRoute routes_a[] = {
        {0u,1u,100u,7u},
        {1u,0u,107u,5u}
    };
    static const TSPRoomBundleRoute routes_b[] = {
        {0u,1u,300u,4u}
    };
    static const TSPRoomBundle a={3u,2u,0u,0u,routes_a};
    static const TSPRoomBundle b={9u,1u,0u,0u,routes_b};
    TSPRoomBundlePlayer p;
    uint16_t expect;
    unsigned i;

    tsp_room_bundle_player_reset(&p);
    if(!tsp_room_bundle_done(&p))return 10;
    if(tsp_room_bundle_begin(&p,&a,0u,2u))return 11;

    if(!tsp_room_bundle_begin(&p,&a,0u,1u))return 12;
    for(i=0u;i<7u;++i){
        expect=(uint16_t)(100u+i);
        if(tsp_room_bundle_global_patch(&p)!=expect){
            fprintf(stderr,"bundle A local=%u got=%u expect=%u\n",
                    i,tsp_room_bundle_global_patch(&p),expect);
            return 13;
        }
        if(!tsp_room_bundle_advance(&p))return 14;
    }
    if(!tsp_room_bundle_done(&p)||tsp_room_bundle_global_patch(&p)!=0xffffu)return 15;

    if(!tsp_room_bundle_begin(&p,&a,1u,0u))return 16;
    if(tsp_room_bundle_global_patch(&p)!=107u)return 17;

    if(!tsp_room_bundle_begin(&p,&b,0u,1u))return 18;
    for(i=0u;i<4u;++i){
        if(tsp_room_bundle_global_patch(&p)!=(uint16_t)(300u+i))return 19;
        if(!tsp_room_bundle_advance(&p))return 20;
    }
    if(!tsp_room_bundle_done(&p))return 21;

    printf("room_bundle_poc PASS bundle-local route addressing and handoff\n");
    return 0;
}
