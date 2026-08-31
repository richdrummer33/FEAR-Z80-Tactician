#include "tilesector_room_seam_poc.h"

#define Q(v) ((int16_t)((v)*16))

/*
 * Canonical S-throat in seam-local coordinates:
 *
 * old room -> [horizontal leg] -> turn north -> [safe vertical leg]
 *                                      -> turn east -> [horizontal leg] -> new room
 *
 * The two inner vertical walls guarantee that the safe middle leg cannot see
 * either arbitrary neighboring room. The runtime may therefore retire the old
 * room, reset/preload dynamic VRAM slots, and reveal the new room only after
 * crossing the second turn.
 */
static const TSPSeamPocContract k_contract = {
    { Q(0),  Q(-4), Q(4)  },
    { Q(36), Q(20), Q(28) },
    { Q(12), Q(4),  Q(28) },
    { Q(20), Q(-4), Q(20) },
    Q(13), Q(19),
    Q(8),
    (int16_t)(Q(16)+8)
};

const TSPSeamPocContract *tsp_seam_poc_contract(void){
    return &k_contract;
}

uint8_t tsp_seam_poc_ray_blocked(int16_t cx,int16_t cy,
                                 int16_t tx,int16_t ty,
                                 const TSPSeamPocVerticalWall *wall){
    int32_t dx,tnum,den,y_num,lo,hi;
    int16_t bx;
    if(!wall)return 0u;
    bx=wall->x_q4;
    if((cx<bx&&tx<bx)||(cx>bx&&tx>bx)||cx==tx)return 0u;
    if(cx==bx||tx==bx)return 1u;

    dx=(int32_t)tx-(int32_t)cx;
    tnum=(int32_t)bx-(int32_t)cx;
    y_num=(int32_t)cy*dx+((int32_t)ty-(int32_t)cy)*tnum;
    den=dx;
    if(den<0){den=-den;y_num=-y_num;}
    lo=(int32_t)wall->y0_q4*den;
    hi=(int32_t)wall->y1_q4*den;
    return (uint8_t)(y_num>=lo&&y_num<=hi);
}
