#include "e1m1_room1_world.h"
#include "generated/e1m1_room1_exact_floor.h"

#define E1_RUN_SPEED_Q4 112
#define E1_ACCEL_Q4 5
#define E1_TURN_Q4 32
#define E1_TURN_ACCEL_Q4 8

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

static int16_t slew(int16_t cur,int16_t target,int16_t step) {
    if(cur<target) {
        cur=(int16_t)(cur+step);
        return cur>target?target:cur;
    }
    if(cur>target) {
        cur=(int16_t)(cur-step);
        return cur<target?target:cur;
    }
    return cur;
}

static int16_t shr_signed(int16_t v,uint8_t n) {
    return v>=0?(int16_t)(v>>n):(int16_t)-(((-v)>>n));
}

static uint16_t mul_u8_u8(uint8_t a,uint8_t b) {
    uint16_t r=0u,x=a;
    while(b) {
        if(b&1u)r=(uint16_t)(r+x);
        x=(uint16_t)(x<<1);
        b>>=1;
    }
    return r;
}

static int16_t mul_s8_s8(int8_t a,int8_t b) {
    uint8_t neg=(uint8_t)((a<0)^(b<0));
    uint8_t ua=(uint8_t)(a<0?-a:a);
    uint8_t ub=(uint8_t)(b<0?-b:b);
    uint16_t r=mul_u8_u8(ua,ub);
    return neg?-(int16_t)r:(int16_t)r;
}

static uint8_t floor_world(int16_t xq,int16_t yq,int8_t *z) {
    int16_t x=(int16_t)(xq>>4),y=(int16_t)(yq>>4);
    uint16_t a,b,i;
    if(x<E1X_WORLD_MIN_X||x>E1X_WORLD_MAX_X||
       y<E1X_WORLD_MIN_Y||y>E1X_WORLD_MAX_Y)return 0u;
    a=k_e1x_floor_row_off[(uint8_t)(y-E1X_WORLD_MIN_Y)];
    b=k_e1x_floor_row_off[(uint8_t)(y-E1X_WORLD_MIN_Y+1)];
    for(i=a;i<b;++i) {
        const E1XFloorRun *r=&k_e1x_floor_runs[i];
        if(x>=r->x0&&x<=r->x1) {
            if(z)*z=r->z;
            return 1u;
        }
    }
    return 0u;
}

uint8_t e1_room1_is_walkable_q4(int16_t xq,int16_t yq) {
    int8_t z;
    if(!floor_world(xq,yq,&z))return 0u;
    if(!floor_world((int16_t)(xq-E1X_PLAYER_RADIUS_Q4),yq,&z))return 0u;
    if(!floor_world((int16_t)(xq+E1X_PLAYER_RADIUS_Q4),yq,&z))return 0u;
    if(!floor_world(xq,(int16_t)(yq-E1X_PLAYER_RADIUS_Q4),&z))return 0u;
    if(!floor_world(xq,(int16_t)(yq+E1X_PLAYER_RADIUS_Q4),&z))return 0u;
    return 1u;
}

int16_t e1_room1_floor_z_q4(int16_t xq,int16_t yq) {
    int8_t z=0;
    if(!floor_world(xq,yq,&z))return 0;
    return (int16_t)z<<4;
}

void e1_room1_reset(E1Room1State *s) {
    s->x_q4=(int16_t)(22<<4);
    s->y_q4=(int16_t)(52<<4);
    s->z_q4=(int16_t)(E1_EYE_HEIGHT_Q4+e1_room1_floor_z_q4(s->x_q4,s->y_q4));
    s->yaw=0u;
    s->speed_q4=0;
    s->strafe_q4=0;
    s->turn_q4=0;
    s->speed_scale=1u;
}

void e1_room1_step(E1Room1State *s,uint8_t input) {
    int8_t throttle=0,strafe=0;
    int16_t target_speed,target_strafe,desired_turn,dx,dy,fdx,fdy,sdx,sdy;
    int8_t sn,cs;
    int16_t old_floor,new_floor,nx,ny;

    if((input&E1_INPUT_SPEED)!=0u) {
        if(++s->speed_scale>3u)s->speed_scale=1u;
    }
    if((input&E1_INPUT_UP)&&!(input&E1_INPUT_DOWN))throttle=1;
    else if((input&E1_INPUT_DOWN)&&!(input&E1_INPUT_UP))throttle=-1;
    if((input&E1_INPUT_STRAFE_LEFT)&&!(input&E1_INPUT_STRAFE_RIGHT))strafe=-1;
    else if((input&E1_INPUT_STRAFE_RIGHT)&&!(input&E1_INPUT_STRAFE_LEFT))strafe=1;

    desired_turn=(input&E1_INPUT_LEFT)?-E1_TURN_Q4:
                 ((input&E1_INPUT_RIGHT)?E1_TURN_Q4:0);
    s->turn_q4=slew(s->turn_q4,desired_turn,E1_TURN_ACCEL_Q4);
    if(s->turn_q4>=0)
        s->yaw=(uint8_t)(s->yaw+(uint8_t)((s->turn_q4+8)>>4));
    else
        s->yaw=(uint8_t)(s->yaw-(uint8_t)(((-s->turn_q4)+8)>>4));

    target_speed=(int16_t)throttle*E1_RUN_SPEED_Q4;
    target_strafe=(int16_t)strafe*E1_RUN_SPEED_Q4;
    s->speed_q4=slew(s->speed_q4,target_speed,E1_ACCEL_Q4);
    s->strafe_q4=slew(s->strafe_q4,target_strafe,E1_ACCEL_Q4);

    sn=k_sin[s->yaw];
    cs=k_sin[(uint8_t)(s->yaw+64u)];
    fdx=shr_signed(mul_s8_s8((int8_t)s->speed_q4,cs),11);
    fdy=shr_signed(mul_s8_s8((int8_t)s->speed_q4,sn),11);
    sdx=shr_signed((int16_t)-mul_s8_s8((int8_t)s->strafe_q4,sn),11);
    sdy=shr_signed(mul_s8_s8((int8_t)s->strafe_q4,cs),11);
    dx=(int16_t)((fdx+sdx)*s->speed_scale);
    dy=(int16_t)((fdy+sdy)*s->speed_scale);

    old_floor=e1_room1_floor_z_q4(s->x_q4,s->y_q4);
    nx=(int16_t)(s->x_q4+dx);
    if(e1_room1_is_walkable_q4(nx,s->y_q4)) {
        new_floor=e1_room1_floor_z_q4(nx,s->y_q4);
        if(new_floor-old_floor<=(3<<4)) {
            s->x_q4=nx;
            old_floor=new_floor;
        }
    }
    ny=(int16_t)(s->y_q4+dy);
    if(e1_room1_is_walkable_q4(s->x_q4,ny)) {
        new_floor=e1_room1_floor_z_q4(s->x_q4,ny);
        if(new_floor-old_floor<=(3<<4)) {
            s->y_q4=ny;
            old_floor=new_floor;
        }
    }

    new_floor=e1_room1_floor_z_q4(s->x_q4,s->y_q4);
    s->z_q4=slew(s->z_q4,(int16_t)(E1_EYE_HEIGHT_Q4+new_floor),4);
}
