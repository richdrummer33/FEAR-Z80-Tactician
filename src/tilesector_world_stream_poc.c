#include "tilesector_world_stream_poc.h"

uint32_t tsp_stream_mix32(uint32_t x){
    x^=x<<13;
    x^=x>>17;
    x^=x<<5;
    return x;
}

uint32_t tsp_stream_root_key(uint32_t seed){
    uint32_t k=tsp_stream_mix32(seed^0xA341316Cu);
    return k?k:0x6D2B79F5u;
}

uint32_t tsp_stream_child_key(uint32_t seed,uint32_t parent_key,uint8_t exit_index){
    uint32_t x=parent_key^seed^0x9E3779B9u;
    x^=(uint32_t)(exit_index+1u)*0x85EBCA6Bu;
    x=tsp_stream_mix32(x);
    x=tsp_stream_mix32(x^parent_key^(parent_key>>11));
    return x?x:0xC2B2AE35u;
}

TSPModuleKind tsp_stream_choose_kind(uint32_t seed,uint32_t node_key,uint8_t allow_vertical){
    uint8_t r=(uint8_t)(tsp_stream_mix32(node_key^seed^0x27D4EB2Du)&15u);
    if(r<5u)return TSP_MODULE_HALL_STRAIGHT;
    if(r<7u)return TSP_MODULE_TURN_LEFT;
    if(r<9u)return TSP_MODULE_TURN_RIGHT;
    if(r<11u)return TSP_MODULE_ROOM_SPLIT;
    if(r==11u)return TSP_MODULE_T_SPLIT;
    if(!allow_vertical)return (r&1u)?TSP_MODULE_TURN_LEFT:TSP_MODULE_TURN_RIGHT;
    if(r==12u)return TSP_MODULE_STAIR_QUARTER_UP_LEFT;
    if(r==13u)return TSP_MODULE_STAIR_QUARTER_UP_RIGHT;
    if(r==14u)return TSP_MODULE_STAIR_QUARTER_DOWN_LEFT;
    return TSP_MODULE_STAIR_QUARTER_DOWN_RIGHT;
}

void tsp_stream_describe(uint32_t seed,uint32_t node_key,uint8_t rotation,
                         int16_t logical_floor_q4,uint8_t allow_vertical,
                         TSPStreamNodeDesc *out){
    uint32_t v=tsp_stream_mix32(node_key^seed^0x165667B1u);
    if(!out)return;
    out->key=node_key;
    out->module_kind=(uint8_t)tsp_stream_choose_kind(seed,node_key,allow_vertical);
    out->asset_variant=(uint8_t)((v>>8)%3u);
    out->rotation=(uint8_t)(rotation&3u);
    out->logical_floor_q4=logical_floor_q4;
}

void tsp_stream_reset(TSPStreamWorld *w,uint32_t seed,uint8_t allow_vertical){
    if(!w)return;
    w->seed=seed;
    w->depth=0u;
    tsp_stream_describe(seed,tsp_stream_root_key(seed),0u,0,allow_vertical,&w->current);
}

uint8_t tsp_stream_enter(TSPStreamWorld *w,uint8_t exit_index,uint8_t allow_vertical){
    const TSPModuleTopology *top;
    uint8_t out_dir;
    int16_t dz;
    uint32_t child;
    if(!w||w->depth>=TSP_STREAM_BREADCRUMB_MAX)return 0u;
    top=tsp_module_topology((TSPModuleKind)w->current.module_kind);
    if(!top||exit_index>=top->exit_count)return 0u;
    if(!tsp_module_exit((TSPModuleKind)w->current.module_kind,w->current.rotation,
                        exit_index,&out_dir,&dz))return 0u;
    w->breadcrumbs[w->depth].node=w->current;
    w->breadcrumbs[w->depth].exit_taken=exit_index;
    ++w->depth;
    child=tsp_stream_child_key(w->seed,w->current.key,exit_index);
    tsp_stream_describe(w->seed,child,out_dir,
                        (int16_t)(w->current.logical_floor_q4+dz),
                        allow_vertical,&w->current);
    return 1u;
}

uint8_t tsp_stream_back(TSPStreamWorld *w){
    if(!w||!w->depth)return 0u;
    --w->depth;
    w->current=w->breadcrumbs[w->depth].node;
    return 1u;
}
