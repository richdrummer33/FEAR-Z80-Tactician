#include <stdint.h>
#include <stdio.h>
#include "tilesector_room_seam_poc.h"

#define Q(v) ((int16_t)((v)*16))

int main(void){
    const TSPSeamPocContract *s=tsp_seam_poc_contract();
    int16_t cx,cy,ty;
    uint32_t old_rays=0u,new_rays=0u;

    if(!s)return 10;
    if(!(s->retire_old_y_q4<s->reveal_new_y_q4))return 11;

    /* Exhaust every representable Q4 camera point in the safe middle leg and
     * every Q4 point across both room apertures. A single unblocked ray means
     * the room-retirement/prefetch contract is false. */
    for(cx=s->safe_x0_q4;cx<=s->safe_x1_q4;++cx){
        for(cy=s->retire_old_y_q4;cy<Q(19);++cy){
            for(ty=s->old_aperture.y0_q4;ty<=s->old_aperture.y1_q4;++ty){
                ++old_rays;
                if(!tsp_seam_poc_ray_blocked(cx,cy,s->old_aperture.x_q4,ty,
                                             &s->old_occluder)){
                    fprintf(stderr,"old-room ray escaped cx=%d cy=%d ty=%d\n",cx,cy,ty);
                    return 20;
                }
            }
            for(ty=s->new_aperture.y0_q4;ty<=s->new_aperture.y1_q4;++ty){
                ++new_rays;
                if(!tsp_seam_poc_ray_blocked(cx,cy,s->new_aperture.x_q4,ty,
                                             &s->new_occluder)){
                    fprintf(stderr,"new-room ray escaped cx=%d cy=%d ty=%d\n",cx,cy,ty);
                    return 21;
                }
            }
        }
    }

    /* Sanity controls: before the first bend the old aperture is genuinely
     * visible; after the second bend the new aperture is genuinely visible. */
    if(tsp_seam_poc_ray_blocked(Q(4),Q(0),s->old_aperture.x_q4,Q(0),
                                &s->old_occluder)){
        fprintf(stderr,"control old-room ray unexpectedly blocked\n");
        return 30;
    }
    if(tsp_seam_poc_ray_blocked(Q(28),Q(24),s->new_aperture.x_q4,Q(24),
                                &s->new_occluder)){
        fprintf(stderr,"control new-room ray unexpectedly blocked\n");
        return 31;
    }

    printf("canonical_seam_poc PASS old_rays=%lu new_rays=%lu safe_y=[%.2f,%.2f)\n",
           (unsigned long)old_rays,(unsigned long)new_rays,
           s->retire_old_y_q4/16.0,s->reveal_new_y_q4/16.0);
    return 0;
}
