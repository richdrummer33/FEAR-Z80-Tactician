#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tilesector_room_catalog_poc.h"

int main(void){
    const uint32_t seed=UINT32_C(0xC0FFEE42);
    unsigned seen[8]={0};
    unsigned i;

    /* Hall nodes must deterministically cover all five straight visual classes. */
    for(i=0u;i<20000u;++i){
        TSPStreamNodeDesc n;
        TSPRoomCatalogChoice a,b;
        tsp_stream_describe(seed,tsp_stream_mix32((uint32_t)i+1u),0u,0,0u,&n);
        n.module_kind=TSP_MODULE_HALL_STRAIGHT;
        if(!tsp_room_catalog_choose(seed,&n,0u,&a))return 10;
        if(!tsp_room_catalog_choose(seed,&n,0u,&b)||memcmp(&a,&b,sizeof(a))!=0)return 11;
        if(a.bundle_id>=8u)return 12;
        ++seen[a.bundle_id];
    }
    if(!seen[0]||!seen[1]||!seen[4]||!seen[6]||!seen[7]){
        fprintf(stderr,"straight catalog coverage missing\n");
        return 13;
    }

    {
        TSPStreamNodeDesc n={0};
        TSPRoomCatalogChoice c;
        n.key=7u;

        n.module_kind=TSP_MODULE_TURN_LEFT;
        if(!tsp_room_catalog_choose(seed,&n,0u,&c)||c.bundle_id!=5u||
           c.entry_portal!=0u||c.exit_portal!=1u)return 20;

        n.module_kind=TSP_MODULE_TURN_RIGHT;
        if(!tsp_room_catalog_choose(seed,&n,0u,&c)||c.bundle_id!=5u||
           c.entry_portal!=1u||c.exit_portal!=0u)return 21;

        n.module_kind=TSP_MODULE_T_SPLIT;
        if(!tsp_room_catalog_choose(seed,&n,0u,&c)||c.bundle_id!=2u||c.exit_portal!=1u)return 22;
        if(!tsp_room_catalog_choose(seed,&n,1u,&c)||c.bundle_id!=2u||c.exit_portal!=2u)return 23;

        n.module_kind=TSP_MODULE_STAIR_QUARTER_UP_RIGHT;
        if(!tsp_room_catalog_choose(seed,&n,0u,&c)||c.bundle_id!=3u||
           c.entry_portal!=0u||c.exit_portal!=1u)return 24;

        n.module_kind=TSP_MODULE_STAIR_QUARTER_DOWN_LEFT;
        if(!tsp_room_catalog_choose(seed,&n,0u,&c)||c.bundle_id!=3u||
           c.entry_portal!=1u||c.exit_portal!=0u)return 25;

        n.module_kind=TSP_MODULE_ROOM_SPLIT;
        if(tsp_room_catalog_choose(seed,&n,0u,&c))return 26;
        n.module_kind=TSP_MODULE_STAIR_QUARTER_UP_LEFT;
        if(tsp_room_catalog_choose(seed,&n,0u,&c))return 27;
    }

    printf("room_catalog_poc PASS straight=[%u,%u,%u,%u,%u] turn/split/stair mappings exact\n",
           seen[0],seen[1],seen[4],seen[6],seen[7]);
    return 0;
}
