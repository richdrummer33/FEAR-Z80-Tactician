/*
 * Compare the CURRENT run-level inv_mid sort with the actual front/back
 * relation on the angular interval where two projected wall spans overlap.
 *
 * For a fixed camera X/Y, yaw merely re-parameterizes world bearing onto
 * screen X.  For two non-intersecting static segments, the true near/far
 * relation across their common angular interval should therefore be stable.
 * If g_run_order flips while this shared-ray relation does not, the flip is
 * sort-approximation churn rather than an occlusion-topology event.
 */
#define main exact_overlap_order_yaw_probe_original_main
#include "exact_overlap_order_yaw_probe.c"
#undef main

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TR_MAX_ACTIVE 20u

typedef struct TruthRun {
    uint8_t key, sid, inv_mid, invd;
    uint8_t left_real, right_real;
    int16_t lo, hi;
} TruthRun;

static uint8_t tr_project(uint8_t key,int16_t x_q4,int16_t y_q4,uint8_t yaw,TruthRun *r){
    uint16_t w=k_tspf_keys[key];
    uint8_t sid=(uint8_t)(w&31u),v0=(uint8_t)((w>>5)&15u),v1=(uint8_t)((w>>9)&15u);
    uint16_t a0=bearing_q12((int16_t)((int16_t)k_tspf_vx[v0]<<4)-x_q4,
                              (int16_t)((int16_t)k_tspf_vy[v0]<<4)-y_q4);
    uint16_t a1=bearing_q12((int16_t)((int16_t)k_tspf_vx[v1]<<4)-x_q4,
                              (int16_t)((int16_t)k_tspf_vy[v1]<<4)-y_q4);
    uint16_t len=(uint16_t)((a1-a0)&4095u),yawq;
    int16_t st,en,lo,hi;uint8_t invd,inv0,inv1;
    if(len==0u||len>=2048u)return 0u;
    yawq=(uint16_t)yaw<<4;st=signed_q12((uint16_t)(a0-yawq));en=(int16_t)(st+(int16_t)len);
    while(en<-512){st=(int16_t)(st+4096);en=(int16_t)(en+4096);}
    while(st>512){st=(int16_t)(st-4096);en=(int16_t)(en-4096);}
    lo=st<-512?-512:st;hi=en>512?512:en;if(hi<=lo)return 0u;
    invd=inv_for_dq4(wall_d_q4(sid,k_tspf_seg_anchor[sid],x_q4,y_q4));
    inv0=inv_at_invd(sid,invd,(uint16_t)(yawq+lo)&4095u,lo);
    inv1=inv_at_invd(sid,invd,(uint16_t)(yawq+hi)&4095u,hi);
    r->key=key;r->sid=sid;r->invd=invd;r->inv_mid=(uint8_t)(((uint16_t)inv0+inv1)>>1);
    r->left_real=(uint8_t)(lo==st);r->right_real=(uint8_t)(hi==en);r->lo=lo;r->hi=hi;
    return 1u;
}

static uint8_t tr_build(int16_t x_q4,int16_t y_q4,uint8_t yaw,TruthRun out[TR_MAX_ACTIVE]){
    uint8_t gx=(uint8_t)((uint16_t)x_q4>>6),gy=(uint8_t)((uint16_t)y_q4>>6),lx,ly;
    uint16_t gi,off;uint8_t recipe,base_id,cond_count,i,n=0;const uint8_t *p,*b;TruthRun tmp;
    if(gx>=GRID_W||gy>=GRID_H)return 0u;
    gi=(uint16_t)(((uint16_t)gy<<5)+((uint16_t)gy<<4)+gx);recipe=k_tspf_recipe_grid[gi];if(recipe==0xffu)return 0u;
    lx=(uint8_t)((uint16_t)x_q4&63u);ly=(uint8_t)((uint16_t)y_q4&63u);
    off=k_tspf_recipe_off[recipe];p=&k_tspf_recipe_stream[off];base_id=*p++;cond_count=*p++;
    b=&k_tspf_base_stream[k_tspf_base_off[base_id]];i=*b++;
    for(;i;--i){uint8_t key=*b++;if(n<TR_MAX_ACTIVE&&tr_project(key,x_q4,y_q4,yaw,&tmp)){
        uint8_t j=n;while(j>0u&&out[j-1u].inv_mid>tmp.inv_mid){out[j]=out[j-1u];--j;}out[j]=tmp;++n;}}
    for(i=0;i<cond_count;++i){uint8_t key=*p++,sel=*p++;if(selector_pass(sel,lx,ly)&&n<TR_MAX_ACTIVE&&tr_project(key,x_q4,y_q4,yaw,&tmp)){
        uint8_t j=n;while(j>0u&&out[j-1u].inv_mid>tmp.inv_mid){out[j]=out[j-1u];--j;}out[j]=tmp;++n;}}
    return n;
}

/* Inverse ray distance before the screen-relative secant term.  At one shared
 * ray the omitted factors are common, so larger q means nearer. */
static uint16_t tr_ray_q(const TruthRun *r,uint16_t world_bearing){
    uint8_t bi=(uint8_t)(world_bearing>>4);
    int8_t sn=(int8_t)k_tspf_sin_q7[bi],cs=(int8_t)k_tspf_sin_q7[(uint8_t)(bi+64u)];
    int8_t nx=k_tspf_nx_q5[r->sid],ny=k_tspf_ny_q5[r->sid];int16_t dot;
    if(ny==0&&(nx==32||nx==-32))dot=cs;
    else if(nx==0&&(ny==32||ny==-32))dot=sn;
    else dot=shr_signed((int16_t)((int16_t)nx*cs+(int16_t)ny*sn),5);
    if(dot<0)dot=(int16_t)-dot;if(dot>127)dot=127;
    return (uint16_t)r->invd*(uint16_t)dot;
}

/* 1 => smaller key is farther (correct earlier draw order), 2 => larger key
 * is farther, 0 => quantized tie/ambiguous. */
static uint8_t tr_truth_dir(const TruthRun *a,const TruthRun *b,uint8_t yaw){
    int16_t lo=a->lo>b->lo?a->lo:b->lo,hi=a->hi<b->hi?a->hi:b->hi,mid;
    uint16_t world,qa,qb;uint8_t small_is_a;
    if(hi<lo)return 0u;
    mid=(int16_t)(lo+((hi-lo)>>1));world=(uint16_t)(((uint16_t)yaw<<4)+mid)&4095u;
    qa=tr_ray_q(a,world);qb=tr_ray_q(b,world);if(qa==qb)return 0u;
    small_is_a=(uint8_t)(a->key<b->key);
    /* Lower inverse depth = farther = should be earlier in far->near host order. */
    if(qa<qb)return small_is_a?1u:2u;
    return small_is_a?2u:1u;
}

static uint8_t tr_truth_seen[65536],tr_truth_flip[65536];
static uint16_t tr_touched[65536];

int main(int argc,char **argv){
    unsigned step=8u,gx,gy,lx,ly,yaw;TruthRun order[TR_MAX_ACTIVE];
    uint64_t positions=0,frames=0,obs=0,full_obs=0,ties=0,full_ties=0;
    uint64_t sort_mismatch=0,full_sort_mismatch=0,truth_flips=0,full_truth_flips=0;
    uint64_t pos_sort_mismatch=0,pos_full_sort_mismatch=0,pos_truth_flip=0,pos_full_truth_flip=0;
    uint8_t full_seen[65536]={0},full_flip[65536]={0};
    if(argc>1)step=(unsigned)strtoul(argv[1],0,0);
    if(step==0u||CELL_Q4%step){fprintf(stderr,"step must divide 64\n");return 2;}

    for(gy=0;gy<GRID_H;++gy)for(gx=0;gx<GRID_W;++gx){
        uint16_t gi=(uint16_t)(gy*GRID_W+gx);if(k_tspf_recipe_grid[gi]==0xffu)continue;
        for(ly=0;ly<CELL_Q4;ly+=step)for(lx=0;lx<CELL_Q4;lx+=step){
            int16_t x=(int16_t)(gx*CELL_Q4+lx),y=(int16_t)(gy*CELL_Q4+ly);
            uint32_t ntouched=0,t;uint8_t had_mismatch=0,had_full_mismatch=0,had_flip=0,had_full_flip=0;
            if(!tsp_is_walkable_q4(x,y))continue;++positions;
            for(yaw=0;yaw<256u;++yaw){
                uint8_t n=tr_build(x,y,(uint8_t)yaw,order),i,j;++frames;
                for(i=0;i<n;++i)for(j=(uint8_t)(i+1u);j<n;++j){
                    const TruthRun *ra=&order[i],*rb=&order[j];
                    int16_t olo=ra->lo>rb->lo?ra->lo:rb->lo,ohi=ra->hi<rb->hi?ra->hi:rb->hi;
                    uint8_t ka,kb,small,sortdir,truth,full;uint16_t id;
                    if(ohi<olo)continue;
                    ka=ra->key;kb=rb->key;small=ka<kb?ka:kb;id=(uint16_t)(((uint16_t)small<<8)|(ka<kb?kb:ka));
                    sortdir=(uint8_t)(ka==small?1u:2u);truth=tr_truth_dir(ra,rb,(uint8_t)yaw);
                    full=(uint8_t)(ra->left_real&&ra->right_real&&rb->left_real&&rb->right_real);
                    ++obs;if(full)++full_obs;
                    if(!truth){++ties;if(full)++full_ties;continue;}
                    if(sortdir!=truth){++sort_mismatch;had_mismatch=1u;if(full){++full_sort_mismatch;had_full_mismatch=1u;}}
                    if(!tr_truth_seen[id]&&!full_seen[id])tr_touched[ntouched++]=id;
                    if(!tr_truth_seen[id])tr_truth_seen[id]=truth;else if(tr_truth_seen[id]!=truth&&!tr_truth_flip[id]){tr_truth_flip[id]=1u;++truth_flips;had_flip=1u;}
                    if(full){if(!full_seen[id])full_seen[id]=truth;else if(full_seen[id]!=truth&&!full_flip[id]){full_flip[id]=1u;++full_truth_flips;had_full_flip=1u;}}
                }
            }
            if(had_mismatch)++pos_sort_mismatch;if(had_full_mismatch)++pos_full_sort_mismatch;
            if(had_flip)++pos_truth_flip;if(had_full_flip)++pos_full_truth_flip;
            for(t=0;t<ntouched;++t){uint16_t id=tr_touched[t];tr_truth_seen[id]=tr_truth_flip[id]=full_seen[id]=full_flip[id]=0u;}
        }
    }
    printf("=== POLAR SHARED-RAY OCCLUSION TRUTH PROBE ===\n");
    printf("q4_sample_step=%u\n",step);
    printf("walkable_xy_positions=%llu\n",(unsigned long long)positions);
    printf("yaw_frames=%llu\n",(unsigned long long)frames);
    printf("overlap_pair_observations=%llu\n",(unsigned long long)obs);
    printf("fully_unclipped_overlap_pair_observations=%llu\n",(unsigned long long)full_obs);
    printf("quantized_truth_ties=%llu\n",(unsigned long long)ties);
    printf("fully_unclipped_quantized_truth_ties=%llu\n",(unsigned long long)full_ties);
    printf("current_sort_mismatch_vs_shared_ray_truth=%llu\n",(unsigned long long)sort_mismatch);
    printf("fully_unclipped_sort_mismatch_vs_shared_ray_truth=%llu\n",(unsigned long long)full_sort_mismatch);
    printf("xy_positions_with_sort_mismatch=%llu\n",(unsigned long long)pos_sort_mismatch);
    printf("xy_positions_with_fully_unclipped_sort_mismatch=%llu\n",(unsigned long long)pos_full_sort_mismatch);
    printf("truth_pair_flips_across_yaw=%llu\n",(unsigned long long)truth_flips);
    printf("fully_unclipped_truth_pair_flips_across_yaw=%llu\n",(unsigned long long)full_truth_flips);
    printf("xy_positions_with_truth_pair_flip=%llu\n",(unsigned long long)pos_truth_flip);
    printf("xy_positions_with_fully_unclipped_truth_pair_flip=%llu\n",(unsigned long long)pos_full_truth_flip);
    printf("INTERPRETATION: if truth flips are zero/negligible while sort mismatches are substantial, yaw-dependent inv_mid order is an approximation artifact, not required occlusion topology.\n");
    return 0;
}
