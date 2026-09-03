#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "e1m1_room1_world.h"
#include "tilesector_polar.h"
#include "generated/e1m1_room1_exact_floor.h"
#include "e1m1_room1_polar_meta.h"

extern uint8_t g_tspf_e1_host_all_segments;
extern uint8_t g_e1_host_owner_bits[8];

typedef struct SweepCfg {
    uint8_t cell;
    uint8_t yaw_bins;
    uint8_t cols;
    uint8_t rows;
    uint32_t count;
    uint64_t *masks;
} SweepCfg;

static uint64_t owner_mask(void){
    uint64_t m=0u;
    uint8_t i;
    for(i=0u;i<8u;++i)m|=((uint64_t)g_e1_host_owner_bits[i])<<((uint8_t)(i<<3));
    return m;
}

static uint8_t pop64(uint64_t v){
    uint8_t n=0u;
    while(v){v&=(v-1u);++n;}
    return n;
}

static int cmp_u64(const void *a,const void *b){
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;
    return x<y?-1:(x>y?1:0);
}

static void render_owner(int16_t xq,int16_t yq,uint8_t yaw,
                         uint16_t map[TSP_MAP_CELLS]){
    TSPState p;
    memset(&p,0,sizeof(p));
    p.x_q4=xq;
    p.y_q4=yq;
    p.z_q4=(int16_t)(E1_EYE_HEIGHT_Q4+e1_room1_floor_z_q4(xq,yq));
    p.yaw=yaw;
    p.speed_scale=1u;
    tsp_polar_renderer_reset();
    tsp_polar_render(&p,map,(TSPColumn *)0);
}

static void cfg_init(SweepCfg *c,uint8_t cell,uint8_t yaw_bins){
    c->cell=cell;
    c->yaw_bins=yaw_bins;
    c->cols=(uint8_t)((E1X_WORLD_MAX_X-E1X_WORLD_MIN_X)/cell);
    c->rows=(uint8_t)((E1X_WORLD_MAX_Y-E1X_WORLD_MIN_Y)/cell);
    c->count=(uint32_t)c->cols*c->rows*yaw_bins;
    c->masks=(uint64_t *)calloc(c->count,sizeof(uint64_t));
    if(!c->masks){fprintf(stderr,"allocation failed\n");exit(2);}
}

static uint32_t cfg_index(const SweepCfg *c,int16_t xq,int16_t yq,uint8_t yaw){
    uint8_t wx=(uint8_t)(xq>>4),wy=(uint8_t)(yq>>4);
    uint8_t gx=(uint8_t)((wx-E1X_WORLD_MIN_X)/c->cell);
    uint8_t gy=(uint8_t)((wy-E1X_WORLD_MIN_Y)/c->cell);
    uint8_t width=(uint8_t)(256u/c->yaw_bins);
    uint8_t half=(uint8_t)(width>>1);
    uint8_t yb=(uint8_t)(((uint16_t)(uint8_t)(yaw+half))/width);
    yb=(uint8_t)(yb&(uint8_t)(c->yaw_bins-1u));
    return ((uint32_t)gy*c->cols+gx)*c->yaw_bins+yb;
}

static void report(const SweepCfg *c){
    uint32_t i;
    uint64_t sum=0u;
    uint32_t nonzero=0u;
    uint8_t lo=64u,hi=0u;
    uint64_t *copy=(uint64_t *)malloc((size_t)c->count*sizeof(uint64_t));
    uint32_t unique=0u;
    uint32_t spawn_i;
    uint8_t spawn_count;
    if(!copy){fprintf(stderr,"allocation failed\n");exit(2);}
    memcpy(copy,c->masks,(size_t)c->count*sizeof(uint64_t));
    qsort(copy,c->count,sizeof(uint64_t),cmp_u64);
    for(i=0u;i<c->count;++i){
        uint8_t n=pop64(c->masks[i]);
        sum+=n;
        if(n){++nonzero;if(n<lo)lo=n;}
        if(n>hi)hi=n;
        if(i==0u||copy[i]!=copy[i-1u])++unique;
    }
    spawn_i=((uint32_t)((52-E1X_WORLD_MIN_Y)/c->cell)*c->cols+
             (uint32_t)((22-E1X_WORLD_MIN_X)/c->cell))*c->yaw_bins;
    spawn_count=pop64(c->masks[spawn_i]);
    printf("E1_OWNER_SWEEP cell=%u yaw_bins=%u masks=%lu raw_bytes=%lu "
           "unique_masks=%lu dedup_u16_bytes=%lu mean=%.2f mean_nonzero=%.2f "
           "min_nonzero=%u max=%u spawn=%u\n",
           (unsigned)c->cell,(unsigned)c->yaw_bins,
           (unsigned long)c->count,(unsigned long)c->count*8ul,
           (unsigned long)unique,
           (unsigned long)c->count*2ul+(unsigned long)unique*8ul,
           (double)sum/(double)c->count,
           nonzero?(double)sum/(double)nonzero:0.0,
           nonzero?(unsigned)lo:0u,(unsigned)hi,(unsigned)spawn_count);
    free(copy);
}

int main(void){
    SweepCfg cfgs[8];
    const uint8_t cells[8]={8u,8u,8u,4u,4u,4u,2u,2u};
    const uint8_t yaws [8]={16u,32u,64u,16u,32u,64u,32u,64u};
    uint16_t map[TSP_MAP_CELLS];
    int16_t xq,yq;
    uint16_t yaw16;
    uint8_t ci;
    uint32_t samples=0u;

    for(ci=0u;ci<8u;++ci)cfg_init(&cfgs[ci],cells[ci],yaws[ci]);

    g_tspf_appearance_mode=0u;
    g_tspf_e1_host_all_segments=1u;

    /* Half-world positional lattice and 64 headings. This is a diagnostic
     * sweep, not the final correctness bake. It is dense enough to compare
     * cell/yaw granularity before spending ROM or CI time on a chosen layout. */
    for(yq=(int16_t)(E1X_WORLD_MIN_Y<<4);
        yq<(int16_t)(E1X_WORLD_MAX_Y<<4);yq=(int16_t)(yq+8)){
        for(xq=(int16_t)(E1X_WORLD_MIN_X<<4);
            xq<(int16_t)(E1X_WORLD_MAX_X<<4);xq=(int16_t)(xq+8)){
            if(!e1_room1_is_walkable_q4(xq,yq))continue;
            for(yaw16=0u;yaw16<256u;yaw16+=4u){
                uint64_t m;
                render_owner(xq,yq,(uint8_t)yaw16,map);
                m=owner_mask();
                for(ci=0u;ci<8u;++ci)
                    cfgs[ci].masks[cfg_index(&cfgs[ci],xq,yq,(uint8_t)yaw16)]|=m;
                ++samples;
            }
        }
    }
    g_tspf_e1_host_all_segments=0u;

    printf("E1_OWNER_SWEEP_PASS rendered_states=%lu pos_step_q4=8 yaw_step=4\n",
           (unsigned long)samples);
    for(ci=0u;ci<8u;++ci){report(&cfgs[ci]);free(cfgs[ci].masks);}
    return 0;
}
