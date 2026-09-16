/*
 * Silhouette shadow map: the properties that have to hold, on synthetic
 * geometry where the right answer is known analytically.
 *
 * The map is an approximation of a ray cast and the useful questions are not
 * "are they identical" but which way it is allowed to be wrong, and whether
 * the projection is sound. Both are checked here against cases a person can
 * verify by hand; the agreement number against real geometry is measured by
 * ROOM_BUNDLE_SHADOW_SELFCHECK in the bake itself.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "room_mesh_bake.h"

static int failures;

static void check(int ok,const char *what){
    if(ok){
        printf("  ok  %s\n",what);
        return;
    }
    printf("FAIL  %s\n",what);
    ++failures;
}

/* An axis-aligned square panel standing upright at y = 0, spanning
 * x in [-w,w] and z in [zlo,zhi]. */
static void add_panel(RMBScene *s,uint8_t obj,double w,double zlo,double zhi){
    int16_t xyz[12];
    static const uint16_t idx[6]={0u,1u,2u,0u,2u,3u};
    RMBTransform t=rmb_transform(0,0,0,0,0,0,1,1,1);
    xyz[0]=(int16_t)(-w*256);xyz[1]=0;xyz[2]=(int16_t)(zlo*256);
    xyz[3]=(int16_t)(w*256); xyz[4]=0;xyz[5]=(int16_t)(zlo*256);
    xyz[6]=(int16_t)(w*256); xyz[7]=0;xyz[8]=(int16_t)(zhi*256);
    xyz[9]=(int16_t)(-w*256);xyz[10]=0;xyz[11]=(int16_t)(zhi*256);
    rmb_add_indexed_mesh_q8(s,obj,&t,xyz,4u,idx,2u,0);
}

int main(void){
    static RMBScene s;
    const double lx=0.0,ly=-40.0,lz=10.0;
    uint8_t obj;
    int gx,gy,missed=0,extra=0,agree=0,total=0;

    rmb_scene_init(&s);
    obj=rmb_new_object(&s,0u);
    rmb_set_object_flags(&s,obj,0u,1u);      /* invisible, casts shadow */
    add_panel(&s,obj,6.0,0.0,12.0);

    check(rmb_shadow_map_build(&s,lx,ly,lz,512u)==1,"a map builds for a panel");
    check(rmb_shadow_map_ready(&s,lx,ly,lz)==1,"the map reports itself ready");
    check(rmb_shadow_map_ready(&s,lx,ly,lz+1.0)==0,
          "the map is not reused for a different light");

    /*
     * The map must never claim LESS shadow than the exact cast. Conservative
     * rasterisation over-covers by under a texel, so disagreement is allowed
     * in one direction only: an extra shadowed cell is a silhouette a hair too
     * wide, which nothing at this pixel scale can resolve, while a missing one
     * is a lit speckle inside a shadow and reads as noise.
     */
    for(gy=0;gy<120;++gy)for(gx=0;gx<120;++gx){
        double wx=-30.0+60.0*((double)gx+0.5)/120.0;
        double wy=-30.0+90.0*((double)gy+0.5)/120.0;
        int e=rmb_segment_occluded_exact(&s,lx,ly,lz,wx,wy,0.0);
        int a=rmb_shadow_coverage(&s,lx,ly,lz,wx,wy,0.0,0.0)<128;
        if(e&&!a)++missed;
        if(a&&!e)++extra;
        if(e==a)++agree;
        ++total;
    }
    printf("  (agreement %.3f%%, missed %d, extra %d of %d)\n",
           100.0*agree/total,missed,extra,total);
    check(missed==0,"the map never under-shadows");
    check(extra*200<total,"over-shadowing stays under half a percent");
    check(agree*100>=total*99,"agreement with the exact cast is at least 99%");

    /* A receiver in front of the panel, between it and the light, is lit. */
    check(rmb_shadow_coverage(&s,lx,ly,lz,0.0,-20.0,0.0,0.0)==255u,
          "a receiver nearer the light than the caster stays lit");
    /* Straight behind the panel at floor level is shadowed. */
    check(rmb_shadow_coverage(&s,lx,ly,lz,0.0,20.0,0.0,0.0)==0u,
          "a receiver behind the caster is shadowed");
    /* Well off to the side, outside the panel's angular extent, is lit. */
    check(rmb_shadow_coverage(&s,lx,ly,lz,40.0,20.0,0.0,0.0)==255u,
          "a receiver outside the caster's silhouette stays lit");

    /*
     * A soft source has to produce values BETWEEN the extremes somewhere along
     * the shadow edge, and must still agree with the hard cast deep inside and
     * far outside. A "soft" shadow that only ever answers 0 or 255 is a hard
     * one with extra steps.
     */
    {
        int partial=0,k;
        for(k=0;k<400;++k){
            double wx=-14.0+28.0*(double)k/399.0;
            uint8_t c=rmb_shadow_coverage(&s,lx,ly,lz,wx,26.0,0.0,2.5);
            if(c>0u&&c<255u)++partial;
        }
        printf("  (%d of 400 samples across the edge are partial)\n",partial);
        check(partial>=8,"a soft source produces a penumbra, not a step");
    }
    check(rmb_shadow_coverage(&s,lx,ly,lz,0.0,20.0,0.0,2.5)<64u,
          "deep inside the shadow stays dark with a soft source");
    check(rmb_shadow_coverage(&s,lx,ly,lz,40.0,20.0,0.0,2.5)==255u,
          "far outside the shadow stays lit with a soft source");

    /*
     * Ground contact is a light-independent measurement, so it must fall off
     * with distance from the caster and be full-open far away.
     */
    {
        uint8_t near_panel=rmb_ground_contact_openness(&s,0.0,0.5,0.0,8.0);
        uint8_t mid=rmb_ground_contact_openness(&s,0.0,4.0,0.0,8.0);
        uint8_t far_away=rmb_ground_contact_openness(&s,0.0,60.0,0.0,8.0);
        printf("  (openness near=%u mid=%u far=%u)\n",
               near_panel,mid,far_away);
        check(near_panel<mid,"the floor is more occluded close to the caster");
        check(mid<far_away,"occlusion falls off with distance");
        check(far_away==255u,"floor far from the caster is fully open");
        check(rmb_ground_contact_openness(&s,0.0,0.5,0.0,0.0)==255u,
              "a zero radius disables the contact term");
    }

    rmb_shadow_map_reset();
    check(rmb_shadow_map_ready(&s,lx,ly,lz)==0,"reset drops the map");
    /* With no map the coverage query must still answer, from the exact cast. */
    check(rmb_shadow_coverage(&s,lx,ly,lz,0.0,20.0,0.0,0.0)==0u,
          "without a map the query falls back to the exact cast");

    printf(failures?"\n%d failing\n":"\nall shadow map tests passed\n",failures);
    return failures?1:0;
}
