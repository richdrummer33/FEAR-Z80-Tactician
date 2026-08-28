#include "tilesector_polar.h"

#define RUN_SPEED_Q4 192
#define ACCEL_Q4 6
#define MANUAL_TURN_Q4 48
#define MANUAL_TURN_ACCEL_Q4 16
#define AUTO_TURN_Q4 40
#define AUTO_TURN_ACCEL_Q4 4

#ifndef TSP_CAPTURE_MOTION
#define TSP_CAPTURE_MOTION 0
#endif

typedef struct { uint16_t frames; uint8_t yaw; int8_t throttle; } DemoPhase;
static const DemoPhase k_demo[] = {
    {145u,0u,1},{10u,248u,1},{10u,236u,1},{12u,224u,1},{12u,212u,1},
    {12u,224u,1},{10u,236u,1},{10u,248u,1},{16u,0u,1},{36u,0u,0}
};
#define DEMO_COUNT ((uint8_t)(sizeof(k_demo)/sizeof(k_demo[0])))

static const int8_t k_sin[256] = {
0,3,6,9,12,16,19,22,25,28,31,34,37,40,43,46,49,51,54,57,60,63,65,68,71,73,76,78,81,83,85,88,
90,92,94,96,98,100,102,104,106,107,109,111,112,113,115,116,117,118,120,121,122,122,123,124,125,125,126,126,126,127,127,127,
127,127,127,127,126,126,126,125,125,124,123,122,122,121,120,118,117,116,115,113,112,111,109,107,106,104,102,100,98,96,94,92,
90,88,85,83,81,78,76,73,71,68,65,63,60,57,54,51,49,46,43,40,37,34,31,28,25,22,19,16,12,9,6,3,
0,-3,-6,-9,-12,-16,-19,-22,-25,-28,-31,-34,-37,-40,-43,-46,-49,-51,-54,-57,-60,-63,-65,-68,-71,-73,-76,-78,-81,-83,-85,-88,
-90,-92,-94,-96,-98,-100,-102,-104,-106,-107,-109,-111,-112,-113,-115,-116,-117,-118,-120,-121,-122,-122,-123,-124,-125,-125,-126,-126,-126,-127,-127,-127,
-127,-127,-127,-127,-126,-126,-126,-125,-125,-124,-123,-122,-122,-121,-120,-118,-117,-116,-115,-113,-112,-111,-109,-107,-106,-104,-102,-100,-98,-96,-94,-92,
-90,-88,-85,-83,-81,-78,-76,-73,-71,-68,-65,-63,-60,-57,-54,-51,-49,-46,-43,-40,-37,-34,-31,-28,-25,-22,-19,-16,-12,-9,-6,-3
};

static int16_t slew(int16_t cur,int16_t target,int16_t step){
    if(cur<target){cur=(int16_t)(cur+step);return cur>target?target:cur;}
    if(cur>target){cur=(int16_t)(cur-step);return cur<target?target:cur;}
    return cur;
}
static int16_t scale_small(int16_t v,uint8_t s){
    int16_t o=v;if(s>=2)o+=v;if(s>=3)o+=v;if(s>=4)o+=v;if(s>=5)o+=v;return o;
}
static int8_t yaw_error(uint8_t target,uint8_t yaw){return (int8_t)(target-yaw);}

uint8_t tsp_is_walkable_q4(int16_t xq,int16_t yq){
    int16_t x=(int16_t)(xq>>4),y=(int16_t)(yq>>4);
    if(x>=20&&x<=76&&y>=20&&y<=76)return 1u;
    if(x>=74&&x<=116&&y>=40&&y<=60)return 1u;
    if(x>=112&&x<=172&&y>=20&&y<=78)return 1u;
    return 0u;
}
void tsp_reset(TSPState *s){
    s->x_q4=(int16_t)(32<<4);s->y_q4=(int16_t)(48<<4);s->yaw=0u;
    s->speed_q4=0;s->strafe_q4=0;s->turn_q4=0;s->speed_scale=1u;s->manual=0u;s->demo_phase=0u;s->demo_ticks=0u;
}
static void apply_motion(TSPState *s,int8_t throttle,int8_t strafe,uint8_t target_yaw,uint8_t manual_turn){
    int16_t target_speed=(int16_t)throttle*RUN_SPEED_Q4,target_strafe=(int16_t)strafe*RUN_SPEED_Q4;
    int16_t dxq,dyq,fdx,fdy,sdx,sdy;int8_t sn,cs;uint8_t scale=s->speed_scale;
    s->speed_q4=slew(s->speed_q4,target_speed,ACCEL_Q4);s->strafe_q4=slew(s->strafe_q4,target_strafe,ACCEL_Q4);
    if(manual_turn){int16_t desired=manual_turn==1u?-MANUAL_TURN_Q4:MANUAL_TURN_Q4;s->turn_q4=slew(s->turn_q4,desired,MANUAL_TURN_ACCEL_Q4);}
    else if(s->manual)s->turn_q4=slew(s->turn_q4,0,MANUAL_TURN_ACCEL_Q4);
    else {int16_t desired=(int16_t)yaw_error(target_yaw,s->yaw)*4;if(desired>AUTO_TURN_Q4)desired=AUTO_TURN_Q4;if(desired<-AUTO_TURN_Q4)desired=-AUTO_TURN_Q4;s->turn_q4=slew(s->turn_q4,desired,AUTO_TURN_ACCEL_Q4);}
    {int16_t ys=s->turn_q4;if(!s->manual)ys=scale_small(ys,scale);if(ys>=0)s->yaw=(uint8_t)(s->yaw+(uint8_t)((ys+8)>>4));else s->yaw=(uint8_t)(s->yaw-(uint8_t)(((-ys)+8)>>4));}
    sn=k_sin[s->yaw];cs=k_sin[(uint8_t)(s->yaw+64u)];
    fdx=(int16_t)(((int16_t)s->speed_q4*cs)>>11);fdy=(int16_t)(((int16_t)s->speed_q4*sn)>>11);
    sdx=(int16_t)((-(int16_t)s->strafe_q4*sn)>>11);sdy=(int16_t)(((int16_t)s->strafe_q4*cs)>>11);
    dxq=scale_small((int16_t)(fdx+sdx),scale);dyq=scale_small((int16_t)(fdy+sdy),scale);
    if(tsp_is_walkable_q4((int16_t)(s->x_q4+dxq),s->y_q4))s->x_q4=(int16_t)(s->x_q4+dxq);
    if(tsp_is_walkable_q4(s->x_q4,(int16_t)(s->y_q4+dyq)))s->y_q4=(int16_t)(s->y_q4+dyq);
}
void tsp_step(TSPState *s,uint8_t input){
#if TSP_CAPTURE_MOTION == 1
    /* Visual-capture translation control: deterministic forward-only path. */
    (void)input;
    apply_motion(s,1,0,s->yaw,0u);
    ++s->demo_ticks;
    return;
#elif TSP_CAPTURE_MOTION == 2
    /* Visual-capture rotation control: rotate in place, no translation. */
    (void)input;
    apply_motion(s,0,0,s->yaw,2u);
    ++s->demo_ticks;
    return;
#elif TSP_CAPTURE_MOTION == 3
    /* Visual-capture mixed control: forward motion while turning right. */
    (void)input;
    apply_motion(s,1,0,s->yaw,2u);
    ++s->demo_ticks;
    return;
#endif
    uint8_t takeover=(uint8_t)(input&(TSP_INPUT_UP|TSP_INPUT_DOWN|TSP_INPUT_LEFT|TSP_INPUT_RIGHT|TSP_INPUT_STRAFE_LEFT|TSP_INPUT_STRAFE_RIGHT));
    int8_t throttle=0,strafe=0;uint8_t turn=0;
    if(input&TSP_INPUT_SPEED){if(++s->speed_scale>5u)s->speed_scale=1u;}if(takeover)s->manual=1u;
    if(s->manual){if((input&TSP_INPUT_UP)&&!(input&TSP_INPUT_DOWN))throttle=1;else if((input&TSP_INPUT_DOWN)&&!(input&TSP_INPUT_UP))throttle=-1;
        if((input&TSP_INPUT_STRAFE_LEFT)&&!(input&TSP_INPUT_STRAFE_RIGHT))strafe=-1;else if((input&TSP_INPUT_STRAFE_RIGHT)&&!(input&TSP_INPUT_STRAFE_LEFT))strafe=1;
        if((input&TSP_INPUT_LEFT)&&!(input&TSP_INPUT_RIGHT))turn=1u;else if((input&TSP_INPUT_RIGHT)&&!(input&TSP_INPUT_LEFT))turn=2u;
        apply_motion(s,throttle,strafe,s->yaw,turn);return;}
    {const DemoPhase *p=&k_demo[s->demo_phase];apply_motion(s,p->throttle,0,p->yaw,0u);s->demo_ticks=(uint16_t)(s->demo_ticks+s->speed_scale);
        if(s->demo_ticks>=p->frames){s->demo_ticks=0;if(s->demo_phase+1u<DEMO_COUNT)++s->demo_phase;else s->demo_ticks=p->frames;}}
}
