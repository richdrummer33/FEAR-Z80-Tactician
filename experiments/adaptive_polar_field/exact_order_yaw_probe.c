/*
 * Host-only oracle probe for the current Polar run-order rule.
 *
 * Question: at one fixed Q4 camera X/Y, when two baked visible spans are on
 * screen at more than one yaw, can their relative inv_mid order flip solely
 * because yaw changed?  If not (or only at clipped FOV edges), translation can
 * own an ordered span chain and yaw can cheaply clip it.  If it does, a final
 * chain representation needs angular order events too.
 *
 * This duplicates the small, exact host-side projection/order math from
 * tilesector_polar_renderer.c on purpose. It does not mutate the renderer.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tilesector_polar.h"
#include "generated/tilesector_polar_data.inc"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64u
#define MAX_ACTIVE 20u
#define NEAR_Z_Q4 (10<<4)
#define FAR_Z_Q4  (127<<4)

typedef struct ProbeRun {
    uint8_t key;
    uint8_t sid;
    uint8_t inv_mid;
    uint8_t left_real;
    uint8_t right_real;
} ProbeRun;

static int16_t shr_signed(int16_t v,uint8_t n){return v>=0?(int16_t)(v>>n):(int16_t)-(((-v)>>n));}
static int16_t signed_q12(uint16_t v){v&=4095u;return v>=2048u?(int16_t)v-4096:(int16_t)v;}

static uint8_t ratio_q8_exact(uint8_t n,uint8_t d){
    uint16_t rec,p_lo,p_hi,q;
    if(!d)return 0u;
    rec=k_tspf_recip8_q16[d];
    p_lo=(uint16_t)n*(uint8_t)rec;
    p_hi=(uint16_t)n*(uint8_t)(rec>>8);
    q=(uint16_t)(p_hi+((p_lo+128u)>>8));
    return (uint8_t)q;
}

static uint16_t bearing_q12(int16_t dxq4,int16_t dyq4){
    uint8_t sx,sy,ax8,ay8,ratio;uint16_t ax,ay,a;
    if(dxq4==0&&dyq4==0)return 0u;
    sx=(uint8_t)(dxq4<0);sy=(uint8_t)(dyq4<0);
    ax=(uint16_t)(dxq4<0?-dxq4:dxq4);ay=(uint16_t)(dyq4<0?-dyq4:dyq4);
    while(ax>255u||ay>255u){ax=(uint16_t)((ax+1u)>>1);ay=(uint16_t)((ay+1u)>>1);}
    ax8=(uint8_t)ax;ay8=(uint8_t)ay;
    if(ax8>=ay8){ratio=ratio_q8_exact(ay8,ax8);a=k_tspf_atan_q12[ratio];}
    else {ratio=ratio_q8_exact(ax8,ay8);a=(uint16_t)(1024u-k_tspf_atan_q12[ratio]);}
    if(sx)a=(uint16_t)(2048u-a);
    if(sy)a=(uint16_t)(0u-a);
    return (uint16_t)(a&4095u);
}

static uint8_t inv_for_dq4(int16_t dq4){
    uint16_t a=(uint16_t)(dq4<0?-dq4:dq4);uint8_t z,f,x0,x1;int16_t d;
    if(a<=NEAR_Z_Q4)return 255u;
    if(a>=FAR_Z_Q4)return k_tspf_invz[127];
    z=(uint8_t)(a>>4);f=(uint8_t)(a&15u);x0=k_tspf_invz[z];x1=k_tspf_invz[(uint8_t)(z+1u)];
    d=(int16_t)x1-(int16_t)x0;
    return (uint8_t)((int16_t)x0+shr_signed((int16_t)(d*(int16_t)f+(d>=0?8:-8)),4));
}

static int16_t wall_d_q4(uint8_t sid,uint8_t anchor_vid,int16_t x_q4,int16_t y_q4){
    int8_t nx=k_tspf_nx_q5[sid],ny=k_tspf_ny_q5[sid];
    if(ny==0&&(nx==32||nx==-32)){
        int16_t wall=(int16_t)((int16_t)k_tspf_vx[anchor_vid]<<4);
        return nx>0?(int16_t)(wall-x_q4):(int16_t)(x_q4-wall);
    }
    if(nx==0&&(ny==32||ny==-32)){
        int16_t wall=(int16_t)((int16_t)k_tspf_vy[anchor_vid]<<4);
        return ny>0?(int16_t)(wall-y_q4):(int16_t)(y_q4-wall);
    }
    {
        int16_t xi=(int16_t)(x_q4>>4),yi=(int16_t)(y_q4>>4);
        uint8_t fx=(uint8_t)(x_q4&15),fy=(uint8_t)(y_q4&15);
        int16_t dx=(int16_t)k_tspf_vx[anchor_vid]-xi,dy=(int16_t)k_tspf_vy[anchor_vid]-yi;
        int16_t whole=(int16_t)nx*dx+(int16_t)ny*dy;
        int16_t frac=(int16_t)nx*fx+(int16_t)ny*fy;
        return (int16_t)(shr_signed(whole,1)-shr_signed(frac,5));
    }
}

static uint8_t inv_at_invd(uint8_t sid,uint8_t invd,uint16_t world_bearing,int16_t rel){
    uint8_t bi=(uint8_t)(world_bearing>>4);
    int8_t sn=(int8_t)k_tspf_sin_q7[bi],cs=(int8_t)k_tspf_sin_q7[(uint8_t)(bi+64u)];
    int8_t nx=k_tspf_nx_q5[sid],ny=k_tspf_ny_q5[sid];
    int16_t dot;uint16_t q,sec;
    if(ny==0&&(nx==32||nx==-32))dot=cs;
    else if(nx==0&&(ny==32||ny==-32))dot=sn;
    else dot=shr_signed((int16_t)((int16_t)nx*cs+(int16_t)ny*sn),5);
    if(dot<0)dot=(int16_t)-dot;if(dot>127)dot=127;
    q=((uint16_t)invd*(uint16_t)dot+64u)>>7;
    sec=k_tspf_sec_q7[(uint16_t)(rel<0?-rel:rel)];
    q=(q*sec+64u)>>7;
    return (uint8_t)(q>255u?255u:q);
}

static uint8_t selector_pass(uint8_t sid,uint8_t lx,uint8_t ly){
    int16_t v=(int16_t)k_tspf_sel_a[sid]*(int16_t)lx+
              (int16_t)k_tspf_sel_b[sid]*(int16_t)ly+k_tspf_sel_c[sid];
    return (uint8_t)(((v>=0)?1u:0u)^k_tspf_sel_inv[sid]);
}

static uint8_t project_key(uint8_t key,int16_t x_q4,int16_t y_q4,uint8_t yaw,ProbeRun *r){
    uint16_t w=k_tspf_keys[key];
    uint8_t sid=(uint8_t)(w&31u),v0=(uint8_t)((w>>5)&15u),v1=(uint8_t)((w>>9)&15u);
    uint16_t a0=bearing_q12((int16_t)((int16_t)k_tspf_vx[v0]<<4)-x_q4,
                              (int16_t)((int16_t)k_tspf_vy[v0]<<4)-y_q4);
    uint16_t a1=bearing_q12((int16_t)((int16_t)k_tspf_vx[v1]<<4)-x_q4,
                              (int16_t)((int16_t)k_tspf_vy[v1]<<4)-y_q4);
    uint16_t len=(uint16_t)((a1-a0)&4095u),yawq;int16_t st,en,lo,hi;
    uint8_t invd,inv0,inv1;
    if(len==0u||len>=2048u)return 0u;
    yawq=(uint16_t)yaw<<4;st=signed_q12((uint16_t)(a0-yawq));en=(int16_t)(st+(int16_t)len);
    while(en<-512){st=(int16_t)(st+4096);en=(int16_t)(en+4096);}
    while(st>512){st=(int16_t)(st-4096);en=(int16_t)(en-4096);}
    lo=st<-512?-512:st;hi=en>512?512:en;if(hi<=lo)return 0u;
    invd=inv_for_dq4(wall_d_q4(sid,k_tspf_seg_anchor[sid],x_q4,y_q4));
    inv0=inv_at_invd(sid,invd,(uint16_t)(yawq+lo)&4095u,lo);
    inv1=inv_at_invd(sid,invd,(uint16_t)(yawq+hi)&4095u,hi);
    r->key=key;r->sid=sid;r->inv_mid=(uint8_t)(((uint16_t)inv0+inv1)>>1);
    r->left_real=(uint8_t)(lo==st);r->right_real=(uint8_t)(hi==en);
    return 1u;
}

static uint8_t build_order(int16_t x_q4,int16_t y_q4,uint8_t yaw,ProbeRun out[MAX_ACTIVE]){
    uint8_t gx=(uint8_t)((uint16_t)x_q4>>6),gy=(uint8_t)((uint16_t)y_q4>>6),lx,ly;
    uint16_t gi,off;uint8_t recipe,base_id,cond_count,i,n=0;const uint8_t *p,*b;
    ProbeRun tmp;
    if(gx>=GRID_W||gy>=GRID_H)return 0u;
    gi=(uint16_t)(((uint16_t)gy<<5)+((uint16_t)gy<<4)+gx);
    recipe=k_tspf_recipe_grid[gi];if(recipe==0xffu)return 0u;
    lx=(uint8_t)((uint16_t)x_q4&63u);ly=(uint8_t)((uint16_t)y_q4&63u);
    off=k_tspf_recipe_off[recipe];p=&k_tspf_recipe_stream[off];base_id=*p++;cond_count=*p++;
    b=&k_tspf_base_stream[k_tspf_base_off[base_id]];i=*b++;
    for(;i;--i){
        uint8_t key=*b++;
        if(n<MAX_ACTIVE&&project_key(key,x_q4,y_q4,yaw,&tmp)){
            uint8_t j=n;
            while(j>0u&&out[j-1u].inv_mid>tmp.inv_mid){out[j]=out[j-1u];--j;}
            out[j]=tmp;++n;
        }
    }
    for(i=0;i<cond_count;++i){
        uint8_t key=*p++,sel=*p++;
        if(selector_pass(sel,lx,ly)&&n<MAX_ACTIVE&&project_key(key,x_q4,y_q4,yaw,&tmp)){
            uint8_t j=n;
            while(j>0u&&out[j-1u].inv_mid>tmp.inv_mid){out[j]=out[j-1u];--j;}
            out[j]=tmp;++n;
        }
    }
    return n;
}

static uint8_t pair_dir[65536];
static uint8_t pair_dir_full[65536];
static uint8_t pair_flipped[65536];
static uint8_t pair_flipped_full[65536];
static uint16_t touched[65536];

static void touch_pair(uint16_t id,uint32_t *ntouched){
    if(pair_dir[id]==0u&&pair_dir_full[id]==0u&&pair_flipped[id]==0u&&pair_flipped_full[id]==0u)
        touched[(*ntouched)++]=id;
}

int main(int argc,char **argv){
    unsigned step=8u;unsigned gx,gy,lx,ly,yaw;
    uint64_t positions=0u,yaw_frames=0u,pair_observations=0u,full_pair_observations=0u;
    uint64_t flipped_pairs_total=0u,flipped_full_pairs_total=0u;
    uint64_t positions_with_flip=0u,positions_with_full_flip=0u;
    uint32_t max_flipped_pairs=0u,max_flipped_full_pairs=0u;
    int16_t first_flip_x=0,first_flip_y=0,first_full_x=0,first_full_y=0;
    uint8_t first_flip_a=0,first_flip_b=0,first_full_a=0,first_full_b=0;
    int have_first_flip=0,have_first_full=0;
    ProbeRun order[MAX_ACTIVE];
    if(argc>1)step=(unsigned)strtoul(argv[1],0,0);
    if(step==0u||CELL_Q4%step){fprintf(stderr,"step must divide 64\n");return 2;}

    for(gy=0;gy<GRID_H;++gy)for(gx=0;gx<GRID_W;++gx){
        uint16_t gi=(uint16_t)(gy*GRID_W+gx);
        if(k_tspf_recipe_grid[gi]==0xffu)continue;
        for(ly=0;ly<CELL_Q4;ly+=step)for(lx=0;lx<CELL_Q4;lx+=step){
            int16_t x=(int16_t)(gx*CELL_Q4+lx),y=(int16_t)(gy*CELL_Q4+ly);
            uint32_t ntouched=0u,pos_flips=0u,pos_full_flips=0u,t;
            /* Restrict the question to camera positions the current motion
             * model considers traversable. */
            if(!tsp_is_walkable_q4(x,y))continue;
            ++positions;
            for(yaw=0;yaw<256u;++yaw){
                uint8_t n=build_order(x,y,(uint8_t)yaw,order),i,j;
                ++yaw_frames;
                for(i=0;i<n;++i)for(j=(uint8_t)(i+1u);j<n;++j){
                    uint8_t ka=order[i].key,kb=order[j].key;
                    uint8_t a=ka<kb?ka:kb,b=ka<kb?kb:ka;
                    uint8_t dir=(uint8_t)(ka==a?1u:2u);
                    uint16_t id=(uint16_t)(((uint16_t)a<<8)|b);
                    uint8_t full=(uint8_t)(order[i].left_real&&order[i].right_real&&order[j].left_real&&order[j].right_real);
                    touch_pair(id,&ntouched);++pair_observations;
                    if(pair_dir[id]==0u)pair_dir[id]=dir;
                    else if(pair_dir[id]!=dir&&!pair_flipped[id]){
                        pair_flipped[id]=1u;++pos_flips;
                        if(!have_first_flip){have_first_flip=1;first_flip_x=x;first_flip_y=y;first_flip_a=a;first_flip_b=b;}
                    }
                    if(full){
                        ++full_pair_observations;
                        if(pair_dir_full[id]==0u)pair_dir_full[id]=dir;
                        else if(pair_dir_full[id]!=dir&&!pair_flipped_full[id]){
                            pair_flipped_full[id]=1u;++pos_full_flips;
                            if(!have_first_full){have_first_full=1;first_full_x=x;first_full_y=y;first_full_a=a;first_full_b=b;}
                        }
                    }
                }
            }
            if(pos_flips){++positions_with_flip;flipped_pairs_total+=pos_flips;if(pos_flips>max_flipped_pairs)max_flipped_pairs=pos_flips;}
            if(pos_full_flips){++positions_with_full_flip;flipped_full_pairs_total+=pos_full_flips;if(pos_full_flips>max_flipped_full_pairs)max_flipped_full_pairs=pos_full_flips;}
            for(t=0;t<ntouched;++t){uint16_t id=touched[t];pair_dir[id]=pair_dir_full[id]=pair_flipped[id]=pair_flipped_full[id]=0u;}
        }
    }

    printf("=== EXACT POLAR YAW-ORDER PROBE ===\n");
    printf("q4_sample_step=%u\n",step);
    printf("walkable_xy_positions=%llu\n",(unsigned long long)positions);
    printf("yaw_frames=%llu\n",(unsigned long long)yaw_frames);
    printf("pair_observations=%llu\n",(unsigned long long)pair_observations);
    printf("fully_unclipped_pair_observations=%llu\n",(unsigned long long)full_pair_observations);
    printf("xy_positions_with_any_pair_order_flip=%llu\n",(unsigned long long)positions_with_flip);
    printf("xy_positions_with_fully_unclipped_pair_order_flip=%llu\n",(unsigned long long)positions_with_full_flip);
    printf("flipped_pairs_total=%llu\n",(unsigned long long)flipped_pairs_total);
    printf("fully_unclipped_flipped_pairs_total=%llu\n",(unsigned long long)flipped_full_pairs_total);
    printf("max_flipped_pairs_at_one_xy=%u\n",max_flipped_pairs);
    printf("max_fully_unclipped_flipped_pairs_at_one_xy=%u\n",max_flipped_full_pairs);
    if(have_first_flip)printf("first_flip=xq4:%d,yq4:%d,keys:%u/%u\n",first_flip_x,first_flip_y,first_flip_a,first_flip_b);
    else printf("first_flip=none\n");
    if(have_first_full)printf("first_fully_unclipped_flip=xq4:%d,yq4:%d,keys:%u/%u\n",first_full_x,first_full_y,first_full_a,first_full_b);
    else printf("first_fully_unclipped_flip=none\n");
    printf("INTERPRETATION: a fully-unclipped flip proves that exact current inv_mid ordering is yaw-dependent even away from FOV clipping.\n");
    return 0;
}
