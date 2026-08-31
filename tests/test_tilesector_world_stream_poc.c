#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tilesector_world_stream_poc.h"

static int same_node(const TSPStreamNodeDesc *a,const TSPStreamNodeDesc *b){
    return a->key==b->key &&
           a->module_kind==b->module_kind &&
           a->asset_variant==b->asset_variant &&
           a->rotation==b->rotation &&
           a->logical_floor_q4==b->logical_floor_q4;
}

int main(void){
    const uint32_t seed=0xC0FFEE42u;
    unsigned i;
    unsigned straight=0u,turn=0u,split=0u,vertical=0u;

    for(i=0u;i<10000u;++i){
        TSPStreamNodeDesc a,b;
        uint32_t key=tsp_stream_mix32((uint32_t)i^0x51ED270Bu);
        tsp_stream_describe(seed,key,(uint8_t)(i&3u),(int16_t)((int)(i&31u)-16),1u,&a);
        tsp_stream_describe(seed,key,(uint8_t)(i&3u),(int16_t)((int)(i&31u)-16),1u,&b);
        if(!same_node(&a,&b)){
            fprintf(stderr,"nondeterministic descriptor at %u\n",i);
            return 10;
        }
        if(a.module_kind==TSP_MODULE_HALL_STRAIGHT)++straight;
        else if(a.module_kind==TSP_MODULE_TURN_LEFT||a.module_kind==TSP_MODULE_TURN_RIGHT)++turn;
        else if(a.module_kind==TSP_MODULE_ROOM_SPLIT||a.module_kind==TSP_MODULE_T_SPLIT)++split;
        else ++vertical;
    }
    if(!straight||!turn||!split||!vertical){
        fprintf(stderr,"grammar coverage missing s=%u t=%u split=%u z=%u\n",
                straight,turn,split,vertical);
        return 11;
    }

    {
        TSPStreamWorld w;
        TSPStreamNodeDesc visited[49];
        uint8_t exits[48];
        tsp_stream_reset(&w,seed,1u);
        visited[0]=w.current;
        for(i=0u;i<48u;++i){
            const TSPModuleTopology *top=tsp_module_topology((TSPModuleKind)w.current.module_kind);
            uint8_t e;
            uint32_t expected;
            if(!top||!top->exit_count)return 12;
            e=(uint8_t)(tsp_stream_mix32(w.current.key^((uint32_t)i<<16))%top->exit_count);
            exits[i]=e;
            expected=tsp_stream_child_key(seed,w.current.key,e);
            if(!tsp_stream_enter(&w,e,1u))return 13;
            if(w.current.key!=expected){
                fprintf(stderr,"child key mismatch depth=%u\n",i+1u);
                return 14;
            }
            visited[i+1u]=w.current;
        }
        if(w.depth!=48u)return 15;
        for(i=48u;i>0u;--i){
            if(!same_node(&w.current,&visited[i])){
                fprintf(stderr,"forward descriptor drift at depth=%u\n",i);
                return 16;
            }
            if(!tsp_stream_back(&w))return 17;
            if(!same_node(&w.current,&visited[i-1u])){
                fprintf(stderr,"reverse regeneration mismatch at depth=%u exit=%u\n",
                        i-1u,(unsigned)exits[i-1u]);
                return 18;
            }
        }
        if(w.depth!=0u||!same_node(&w.current,&visited[0]))return 19;
    }

    {
        uint32_t k;
        TSPStreamNodeDesc split_node,left_a,left_b,right;
        TSPStreamWorld w;
        int found=0;
        for(k=1u;k<200000u;++k){
            TSPStreamNodeDesc d;
            const TSPModuleTopology *top;
            tsp_stream_describe(seed,k,0u,0,1u,&d);
            top=tsp_module_topology((TSPModuleKind)d.module_kind);
            if(top&&top->exit_count>=2u){split_node=d;found=1;break;}
        }
        if(!found){fprintf(stderr,"could not find split node\n");return 20;}
        memset(&w,0,sizeof(w));w.seed=seed;w.current=split_node;
        if(!tsp_stream_enter(&w,0u,1u))return 21;
        left_a=w.current;
        if(!tsp_stream_back(&w))return 22;
        if(!tsp_stream_enter(&w,1u,1u))return 23;
        right=w.current;
        if(left_a.key==right.key){
            fprintf(stderr,"split exits collided key=%08lX\n",(unsigned long)left_a.key);
            return 24;
        }
        if(!tsp_stream_back(&w))return 25;
        if(!tsp_stream_enter(&w,0u,1u))return 26;
        left_b=w.current;
        if(!same_node(&left_a,&left_b)){
            fprintf(stderr,"split left branch failed deterministic regeneration\n");
            return 27;
        }
    }

    {
        uint32_t k;
        int found=0;
        for(k=1u;k<200000u;++k){
            TSPStreamNodeDesc d;
            TSPStreamWorld w;
            int16_t before;
            tsp_stream_describe(seed,k,0u,0,1u,&d);
            if(d.module_kind<TSP_MODULE_STAIR_QUARTER_UP_LEFT)continue;
            memset(&w,0,sizeof(w));w.seed=seed;w.current=d;before=d.logical_floor_q4;
            if(!tsp_stream_enter(&w,0u,1u))return 28;
            if(w.current.logical_floor_q4==before){
                fprintf(stderr,"vertical module produced no floor delta\n");
                return 29;
            }
            if(!tsp_stream_back(&w)||w.current.logical_floor_q4!=before)return 30;
            found=1;break;
        }
        if(!found)return 31;
    }

    printf("room_stream_poc topology PASS seed=%08lX coverage straight=%u turn=%u split=%u vertical=%u\n",
           (unsigned long)seed,straight,turn,split,vertical);
    return 0;
}
