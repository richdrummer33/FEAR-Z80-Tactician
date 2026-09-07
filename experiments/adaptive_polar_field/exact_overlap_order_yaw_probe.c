/*
 * Control-group refinement of exact_order_yaw_probe.c.
 *
 * The current renderer globally sorts every visible run, but ordering between
 * disjoint screen intervals cannot affect the output.  This probe therefore
 * records yaw-dependent order changes ONLY while the two runs overlap in
 * projected X.  A fully-unclipped overlapping flip is the meaningful case for
 * deciding whether a translation-owned span chain also needs yaw events.
 */
#define main exact_order_yaw_probe_original_main
#include "exact_order_yaw_probe.c"
#undef main

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define OV_MAX_ACTIVE 20u

typedef struct OverlapRun {
    uint8_t key;
    uint8_t sid;
    uint8_t inv_mid;
    uint8_t left_real;
    uint8_t right_real;
    uint8_t x0;
    uint8_t x1;
} OverlapRun;

static uint8_t ov_angle_x(int16_t rel){
    uint16_t a=(uint16_t)(rel<0?-rel:rel),x;
    if(a>512u)a=512u;
    x=rel<0?(uint16_t)(160u-k_tspf_angle_x_pos[a]):k_tspf_angle_x_pos[a];
    return (uint8_t)(x>159u?159u:x);
}

static uint8_t ov_project_key(uint8_t key,int16_t x_q4,int16_t y_q4,uint8_t yaw,OverlapRun *r){
    uint16_t w=k_tspf_keys[key];
    uint8_t sid=(uint8_t)(w&31u),v0=(uint8_t)((w>>5)&15u),v1=(uint8_t)((w>>9)&15u);
    uint16_t a0=bearing_q12((int16_t)((int16_t)k_tspf_vx[v0]<<4)-x_q4,
                              (int16_t)((int16_t)k_tspf_vy[v0]<<4)-y_q4);
    uint16_t a1=bearing_q12((int16_t)((int16_t)k_tspf_vx[v1]<<4)-x_q4,
                              (int16_t)((int16_t)k_tspf_vy[v1]<<4)-y_q4);
    uint16_t len=(uint16_t)((a1-a0)&4095u),yawq;
    int16_t st,en,lo,hi;uint8_t invd,inv0,inv1,x0,x1;
    if(len==0u||len>=2048u)return 0u;
    yawq=(uint16_t)yaw<<4;st=signed_q12((uint16_t)(a0-yawq));en=(int16_t)(st+(int16_t)len);
    while(en<-512){st=(int16_t)(st+4096);en=(int16_t)(en+4096);}
    while(st>512){st=(int16_t)(st-4096);en=(int16_t)(en-4096);}
    lo=st<-512?-512:st;hi=en>512?512:en;if(hi<=lo)return 0u;
    x0=ov_angle_x(lo);x1=ov_angle_x(hi);if(x1<x0){uint8_t t=x0;x0=x1;x1=t;}if(x1==x0&&x1<159u)++x1;
    invd=inv_for_dq4(wall_d_q4(sid,k_tspf_seg_anchor[sid],x_q4,y_q4));
    inv0=inv_at_invd(sid,invd,(uint16_t)(yawq+lo)&4095u,lo);
    inv1=inv_at_invd(sid,invd,(uint16_t)(yawq+hi)&4095u,hi);
    r->key=key;r->sid=sid;r->inv_mid=(uint8_t)(((uint16_t)inv0+inv1)>>1);
    r->left_real=(uint8_t)(lo==st);r->right_real=(uint8_t)(hi==en);r->x0=x0;r->x1=x1;
    return 1u;
}

static uint8_t ov_build_order(int16_t x_q4,int16_t y_q4,uint8_t yaw,OverlapRun out[OV_MAX_ACTIVE]){
    uint8_t gx=(uint8_t)((uint16_t)x_q4>>6),gy=(uint8_t)((uint16_t)y_q4>>6),lx,ly;
    uint16_t gi,off;uint8_t recipe,base_id,cond_count,i,n=0;const uint8_t *p,*b;OverlapRun tmp;
    if(gx>=GRID_W||gy>=GRID_H)return 0u;
    gi=(uint16_t)(((uint16_t)gy<<5)+((uint16_t)gy<<4)+gx);recipe=k_tspf_recipe_grid[gi];if(recipe==0xffu)return 0u;
    lx=(uint8_t)((uint16_t)x_q4&63u);ly=(uint8_t)((uint16_t)y_q4&63u);
    off=k_tspf_recipe_off[recipe];p=&k_tspf_recipe_stream[off];base_id=*p++;cond_count=*p++;
    b=&k_tspf_base_stream[k_tspf_base_off[base_id]];i=*b++;
    for(;i;--i){uint8_t key=*b++;if(n<OV_MAX_ACTIVE&&ov_project_key(key,x_q4,y_q4,yaw,&tmp)){
        uint8_t j=n;while(j>0u&&out[j-1u].inv_mid>tmp.inv_mid){out[j]=out[j-1u];--j;}out[j]=tmp;++n;}}
    for(i=0;i<cond_count;++i){uint8_t key=*p++,sel=*p++;if(selector_pass(sel,lx,ly)&&n<OV_MAX_ACTIVE&&ov_project_key(key,x_q4,y_q4,yaw,&tmp)){
        uint8_t j=n;while(j>0u&&out[j-1u].inv_mid>tmp.inv_mid){out[j]=out[j-1u];--j;}out[j]=tmp;++n;}}
    return n;
}

static uint8_t ov_dir[65536],ov_dir_full[65536],ov_flipped[65536],ov_flipped_full[65536];
static uint16_t ov_touched[65536];

static void ov_touch(uint16_t id,uint32_t *n){
    if(!ov_dir[id]&&!ov_dir_full[id]&&!ov_flipped[id]&&!ov_flipped_full[id])ov_touched[(*n)++]=id;
}

int main(int argc,char **argv){
    unsigned step=8u,gx,gy,lx,ly,yaw;
    uint64_t positions=0,yaw_frames=0,overlap_obs=0,full_overlap_obs=0;
    uint64_t positions_flip=0,positions_full_flip=0,flips_total=0,full_flips_total=0;
    uint32_t max_flips=0,max_full_flips=0;
    int16_t first_x=0,first_y=0,first_full_x=0,first_full_y=0;
    uint8_t first_a=0,first_b=0,first_full_a=0,first_full_b=0;
    int have_first=0,have_first_full=0;OverlapRun order[OV_MAX_ACTIVE];
    if(argc>1)step=(unsigned)strtoul(argv[1],0,0);
    if(step==0u||CELL_Q4%step){fprintf(stderr,"step must divide 64\n");return 2;}

    for(gy=0;gy<GRID_H;++gy)for(gx=0;gx<GRID_W;++gx){
        uint16_t gi=(uint16_t)(gy*GRID_W+gx);if(k_tspf_recipe_grid[gi]==0xffu)continue;
        for(ly=0;ly<CELL_Q4;ly+=step)for(lx=0;lx<CELL_Q4;lx+=step){
            int16_t x=(int16_t)(gx*CELL_Q4+lx),y=(int16_t)(gy*CELL_Q4+ly);
            uint32_t ntouched=0,pos_flips=0,pos_full_flips=0,t;
            if(!tsp_is_walkable_q4(x,y))continue;++positions;
            for(yaw=0;yaw<256u;++yaw){
                uint8_t n=ov_build_order(x,y,(uint8_t)yaw,order),i,j;++yaw_frames;
                for(i=0;i<n;++i)for(j=(uint8_t)(i+1u);j<n;++j){
                    uint8_t ka,kb,a,b,dir,full;uint16_t id;
                    /* Pixel intervals are inclusive in the raster path. */
                    if(order[i].x1<order[j].x0||order[j].x1<order[i].x0)continue;
                    ka=order[i].key;kb=order[j].key;a=ka<kb?ka:kb;b=ka<kb?kb:ka;dir=(uint8_t)(ka==a?1u:2u);
                    id=(uint16_t)(((uint16_t)a<<8)|b);full=(uint8_t)(order[i].left_real&&order[i].right_real&&order[j].left_real&&order[j].right_real);
                    ov_touch(id,&ntouched);++overlap_obs;
                    if(!ov_dir[id])ov_dir[id]=dir;else if(ov_dir[id]!=dir&&!ov_flipped[id]){ov_flipped[id]=1u;++pos_flips;if(!have_first){have_first=1;first_x=x;first_y=y;first_a=a;first_b=b;}}
                    if(full){++full_overlap_obs;if(!ov_dir_full[id])ov_dir_full[id]=dir;else if(ov_dir_full[id]!=dir&&!ov_flipped_full[id]){ov_flipped_full[id]=1u;++pos_full_flips;if(!have_first_full){have_first_full=1;first_full_x=x;first_full_y=y;first_full_a=a;first_full_b=b;}}}
                }
            }
            if(pos_flips){++positions_flip;flips_total+=pos_flips;if(pos_flips>max_flips)max_flips=pos_flips;}
            if(pos_full_flips){++positions_full_flip;full_flips_total+=pos_full_flips;if(pos_full_flips>max_full_flips)max_full_flips=pos_full_flips;}
            for(t=0;t<ntouched;++t){uint16_t id=ov_touched[t];ov_dir[id]=ov_dir_full[id]=ov_flipped[id]=ov_flipped_full[id]=0u;}
        }
    }
    printf("=== EXACT POLAR OVERLAP YAW-ORDER PROBE ===\n");
    printf("q4_sample_step=%u\n",step);
    printf("walkable_xy_positions=%llu\n",(unsigned long long)positions);
    printf("yaw_frames=%llu\n",(unsigned long long)yaw_frames);
    printf("overlapping_pair_observations=%llu\n",(unsigned long long)overlap_obs);
    printf("fully_unclipped_overlapping_pair_observations=%llu\n",(unsigned long long)full_overlap_obs);
    printf("xy_positions_with_overlapping_pair_order_flip=%llu\n",(unsigned long long)positions_flip);
    printf("xy_positions_with_fully_unclipped_overlapping_pair_order_flip=%llu\n",(unsigned long long)positions_full_flip);
    printf("overlapping_flipped_pairs_total=%llu\n",(unsigned long long)flips_total);
    printf("fully_unclipped_overlapping_flipped_pairs_total=%llu\n",(unsigned long long)full_flips_total);
    printf("max_overlapping_flipped_pairs_at_one_xy=%u\n",max_flips);
    printf("max_fully_unclipped_overlapping_flipped_pairs_at_one_xy=%u\n",max_full_flips);
    if(have_first)printf("first_overlap_flip=xq4:%d,yq4:%d,keys:%u/%u\n",first_x,first_y,first_a,first_b);else printf("first_overlap_flip=none\n");
    if(have_first_full)printf("first_fully_unclipped_overlap_flip=xq4:%d,yq4:%d,keys:%u/%u\n",first_full_x,first_full_y,first_full_a,first_full_b);else printf("first_fully_unclipped_overlap_flip=none\n");
    printf("INTERPRETATION: only overlapping flips can change raster ownership; non-overlap flips are irrelevant sort noise.\n");
    return 0;
}
