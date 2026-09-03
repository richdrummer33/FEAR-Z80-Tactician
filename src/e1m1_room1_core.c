#include "e1m1_room1_core.h"
#include "generated/e1m1_room1_exact_geometry.h"
#include <string.h>

#ifndef E1_PROFILE_HOOKS
#define E1_PROFILE_HOOKS 0
#endif
#if E1_PROFILE_HOOKS
volatile uint8_t g_ts_render_stage;
#define E1_STAGE(v) do { g_ts_render_stage=(v); } while(0)
#else
#define E1_STAGE(v) ((void)0)
#endif

#define E1_HORIZON 72
#define E1_NEAR_Z_Q4 (6<<4)
#define E1_FAR_Z_Q4 (127<<4)
#define E1_EYE_HEIGHT_Q4 (5<<4)
#define E1_RUN_SPEED_Q4 112
#define E1_ACCEL_Q4 5
#define E1_TURN_Q4 32
#define E1_TURN_ACCEL_Q4 8

typedef E1XSegment E1Surface;
#define E1_SURFACE_COUNT E1X_SEGMENT_COUNT

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

static const uint8_t k_invz[128] = {
255,255,255,255,255,255,255,255,255,255,255,233,213,197,183,171,
160,151,142,135,128,122,116,111,107,102,98,95,91,88,85,83,
80,78,75,73,71,69,67,66,64,62,61,60,58,57,56,54,
53,52,51,50,49,48,47,47,46,45,44,43,43,42,41,41,
40,39,39,38,38,37,37,36,36,35,35,34,34,33,33,32,
32,32,31,31,30,30,30,29,29,29,28,28,28,28,27,27,
27,26,26,26,26,25,25,25,25,24,24,24,24,23,23,23,
23,23,22,22,22,22,22,22,21,21,21,21,21,20,20,20
};

static const uint16_t k_row_base[E1_ROWS] = {
    0u,20u,40u,60u,80u,100u,120u,140u,160u,
    180u,200u,220u,240u,260u,280u,300u,320u,340u
};

static const uint8_t k_span_recip_q8[21] = {
    0u,255u,128u,85u,64u,51u,43u,36u,32u,28u,26u,
    23u,21u,20u,18u,17u,16u,15u,14u,13u,13u
};

typedef struct E1CameraCtx {
    int16_t px,py;
    int16_t frac_z,frac_x;
    int8_t sn,cs;
} E1CameraCtx;

typedef struct E1Run {
    uint8_t sid;
    int8_t c0,c1;
    uint8_t iv0,iv1;
    uint8_t left_real,right_real;
} E1Run;

static uint8_t g_depth[E1_MAP_CELLS];
static uint8_t g_full_inv[E1_COLS];
static E1Run g_runs[E1_SURFACE_COUNT];
static uint16_t g_base_map[E1_MAP_CELLS];
static uint8_t g_base_ready;

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

/* Z80-cheap multiplies. All hot-path geometry operands are genuinely 8-bit:
 * Room-1 local delta XY is within +/-96, sin/cos is Q7, height deltas are
 * within +/-29. Avoid SDCC's generic signed-long multiply helpers. */
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

static int16_t mul_s8_u8(int8_t a,uint8_t b) {
    uint8_t ua=(uint8_t)(a<0?-a:a);
    uint16_t r=mul_u8_u8(ua,b);
    return a<0?-(int16_t)r:(int16_t)r;
}

static uint8_t point_in_outer(int16_t x,int16_t y) {
    if(x>=24&&x<=80&&y>=24&&y<=80)return 1u;
    if(x>=16&&x<24&&y>=32&&y<=72)return 1u;
    if(x>80&&x<=88&&y>=32&&y<=72)return 1u;
    if(x>=88&&x<=112&&y>=48&&y<=64)return 1u;
    return 0u;
}

static uint8_t in_pillar(int16_t x,int16_t y) {
    if(x>=52&&x<=60&&y>=56&&y<=64)return 1u;
    if(x>=68&&x<=76&&y>=56&&y<=64)return 1u;
    return 0u;
}

uint8_t e1_room1_is_walkable_q4(int16_t xq,int16_t yq) {
    int16_t x=(int16_t)(xq>>4),y=(int16_t)(yq>>4);
    if(x>=112)return 0u; /* temporary Room-2 portal terminator */
    return (uint8_t)(point_in_outer(x,y)&&!in_pillar(x,y));
}

int16_t e1_room1_floor_z_q4(int16_t xq,int16_t yq) {
    int16_t x=(int16_t)(xq>>4),y=(int16_t)(yq>>4);
    int16_t z=0;
    if(y>=32&&y<=48) {
        if(x>=24&&x<44)z=14;
        else if(x<48)z=12;
        else if(x<52)z=10;
        else if(x<56)z=8;
        else if(x<60)z=6;
        else if(x<64)z=4;
        else if(x<68)z=2;
    }
    return (int16_t)(z<<4);
}

void e1_room1_reset(E1Room1State *s) {
    s->x_q4=(int16_t)(80<<4);
    s->y_q4=(int16_t)(56<<4);
    s->z_q4=(int16_t)(E1_EYE_HEIGHT_Q4+e1_room1_floor_z_q4(s->x_q4,s->y_q4));
    s->yaw=128u;
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
    /* Stair vertical motion is interpolated instead of snapping the camera. */
    s->z_q4=slew(s->z_q4,(int16_t)(E1_EYE_HEIGHT_Q4+new_floor),4);
}

static uint8_t inv_for_zq4(int16_t zq4) {
    uint16_t a=(uint16_t)(zq4<0?-zq4:zq4);
    uint8_t zi,frac,u,v;
    int16_t d;
    if(a<=E1_NEAR_Z_Q4)return 255u;
    if(a>=E1_FAR_Z_Q4)return k_invz[127];
    zi=(uint8_t)(a>>4);
    frac=(uint8_t)(a&15u);
    u=k_invz[zi];
    v=k_invz[(uint8_t)(zi+1u)];
    d=(int16_t)v-(int16_t)u;
    return (uint8_t)((int16_t)u+
        shr_signed((int16_t)(d*frac+(d>=0?8:-8)),4));
}

static uint8_t ratio_q8(uint16_t a,uint16_t b) {
    uint8_t i,q=0u;
    uint16_t r=a;
    if(b==0u||a==0u)return 0u;
    if(a>=b)return 255u;
    for(i=0u;i<8u;++i) {
        r=(uint16_t)(r<<1);
        q=(uint8_t)(q<<1);
        if(r>=b) {
            r=(uint16_t)(r-b);
            q|=1u;
        }
    }
    return q;
}

static int16_t scale_q8_s16(int16_t v,uint8_t q8) {
    uint16_t a=(uint16_t)(v<0?-v:v);
    uint16_t hi=(uint16_t)(a>>8);
    uint16_t lo=(uint16_t)(a&255u);
    uint16_t out=(uint16_t)(hi*q8+((lo*q8)>>8));
    return v<0?-(int16_t)out:(int16_t)out;
}

static int16_t project_x(int16_t xq4,uint8_t inv) {
    uint16_t ax,xi,xf,p,rem,extra;
    int16_t px;
    uint8_t neg=0u;
    if(xq4<0) {
        neg=1u;
        ax=(uint16_t)(-xq4);
    } else ax=(uint16_t)xq4;
    if(ax>(uint16_t)(127u<<4))ax=(uint16_t)(127u<<4);
    xi=(uint16_t)(ax>>4);
    xf=(uint16_t)(ax&15u);
    p=mul_u8_u8((uint8_t)xi,inv);
    px=(int16_t)(p>>5);
    rem=(uint16_t)(p&31u);
    extra=(uint16_t)(((rem<<4)+mul_u8_u8((uint8_t)xf,inv))>>9);
    px=(int16_t)(px+(int16_t)extra);
    if(neg)px=(int16_t)-px;
    return (int16_t)(80+px);
}

static int8_t col_floor(int16_t px) {
    return px>=0?(int8_t)(px>>3):(int8_t)-(((-px)+7)>>3);
}

static int8_t row_floor(int16_t py) {
    return py>=0?(int8_t)(py>>3):(int8_t)-(((-py)+7)>>3);
}

static uint8_t shade_for(uint8_t inv,int8_t bias) {
    int8_t shade=inv>=82u?2:(inv>=46u?1:0);
    shade=(int8_t)(shade+bias);
    if(shade<0)shade=0;
    if(shade>2)shade=2;
    return (uint8_t)shade;
}

static uint8_t project_surface(const E1CameraCtx *cam,const E1Surface *w,
                               int16_t *sx0,int16_t *sx1,
                               uint8_t *iv0,uint8_t *iv1,
                               uint8_t *left_real,uint8_t *right_real) {
    const E1XVertex *va=&k_e1x_vertices[w->a];
    const E1XVertex *vb=&k_e1x_vertices[w->b];
    int16_t dx0=(int16_t)va->x-cam->px,dy0=(int16_t)va->y-cam->py;
    int16_t dx1=(int16_t)vb->x-cam->px,dy1=(int16_t)vb->y-cam->py;
    int16_t z0=(int16_t)(shr_signed((int16_t)(
        mul_s8_s8((int8_t)dx0,cam->cs)+mul_s8_s8((int8_t)dy0,cam->sn)),3)-cam->frac_z);
    int16_t x0=(int16_t)(shr_signed((int16_t)(
        -mul_s8_s8((int8_t)dx0,cam->sn)+mul_s8_s8((int8_t)dy0,cam->cs)),3)-cam->frac_x);
    int16_t z1=(int16_t)(shr_signed((int16_t)(
        mul_s8_s8((int8_t)dx1,cam->cs)+mul_s8_s8((int8_t)dy1,cam->sn)),3)-cam->frac_z);
    int16_t x1=(int16_t)(shr_signed((int16_t)(
        -mul_s8_s8((int8_t)dx1,cam->sn)+mul_s8_s8((int8_t)dy1,cam->cs)),3)-cam->frac_x);
    uint8_t clip0=0u,clip1=0u;

    if(z0<E1_NEAR_Z_Q4&&z1<E1_NEAR_Z_Q4)return 0u;
    if(z0<E1_NEAR_Z_Q4) {
        uint8_t q=ratio_q8((uint16_t)(E1_NEAR_Z_Q4-z0),
                           (uint16_t)(z1-z0));
        x0=(int16_t)(x0+scale_q8_s16((int16_t)(x1-x0),q));
        z0=E1_NEAR_Z_Q4;
        clip0=1u;
    }
    if(z1<E1_NEAR_Z_Q4) {
        uint8_t q=ratio_q8((uint16_t)(E1_NEAR_Z_Q4-z1),
                           (uint16_t)(z0-z1));
        x1=(int16_t)(x1+scale_q8_s16((int16_t)(x0-x1),q));
        z1=E1_NEAR_Z_Q4;
        clip1=1u;
    }

    if(x0<-z0&&x1<-z1)return 0u;
    if(x0>z0&&x1>z1)return 0u;

    *iv0=inv_for_zq4(z0);
    *iv1=inv_for_zq4(z1);
    *sx0=project_x(x0,*iv0);
    *sx1=project_x(x1,*iv1);
    *left_real=(uint8_t)!clip0;
    *right_real=(uint8_t)!clip1;

    if(*sx0>*sx1) {
        int16_t tx=*sx0;
        uint8_t ti=*iv0,tr=*left_real;
        *sx0=*sx1;
        *sx1=tx;
        *iv0=*iv1;
        *iv1=ti;
        *left_real=*right_real;
        *right_real=tr;
    }
    if(*sx1<0||*sx0>159||*sx0==*sx1)return 0u;
    return 1u;
}

void e1_room1_render(const E1Room1State *s,uint16_t out[E1_MAP_CELLS]) {
    uint8_t r,c,si,run_count=0u;
    E1CameraCtx cam;
    uint8_t fx=(uint8_t)(s->x_q4&15),fy=(uint8_t)(s->y_q4&15);

    E1_STAGE(2u);
    cam.px=(int16_t)(s->x_q4>>4);
    cam.py=(int16_t)(s->y_q4>>4);
    cam.sn=k_sin[s->yaw];
    cam.cs=k_sin[(uint8_t)(s->yaw+64u)];
    cam.frac_z=shr_signed((int16_t)(
        mul_s8_s8((int8_t)fx,cam.cs)+mul_s8_s8((int8_t)fy,cam.sn)),7);
    cam.frac_x=shr_signed((int16_t)(
        -mul_s8_s8((int8_t)fx,cam.sn)+mul_s8_s8((int8_t)fy,cam.cs)),7);

    E1_STAGE(1u);
    if(!g_base_ready) {
        for(r=0u;r<E1_ROWS;++r) {
            for(c=0u;c<E1_COLS;++c) {
                uint16_t idx=k_row_base[r]+c;
                g_base_map[idx]=(r<9u)?E1_TILE_CEILING:
                                (r==9u?E1_TILE_HORIZON:E1_TILE_FLOOR);
            }
        }
        g_base_ready=1u;
    }
    memcpy(out,g_base_map,sizeof(g_base_map));
    memset(g_depth,0,sizeof(g_depth));
    memset(g_full_inv,0,sizeof(g_full_inv));

    E1_STAGE(3u);
    /*
     * Project every surface exactly once. In the same pass establish the
     * nearest full-height occluder for each of the twenty hardware columns.
     * Any partial/full surface behind that per-column depth cannot contribute
     * a visible tile, so its expensive vertical raster loop is never entered.
     */
    for(si=0u;si<E1_SURFACE_COUNT;++si) {
        const E1Surface *w=&k_e1x_segments[si];
        int16_t sx0,sx1;
        uint8_t iv0,iv1,left_real,right_real;
        int8_t c0,c1;

        if(!project_surface(&cam,w,&sx0,&sx1,&iv0,&iv1,
                            &left_real,&right_real))continue;
        c0=col_floor(sx0);
        c1=col_floor(sx1);
        if(c0<0)c0=0;
        if(c1>=(int8_t)E1_COLS)c1=(int8_t)(E1_COLS-1u);
        if(c1<c0)continue;

        {
            E1Run *run=&g_runs[run_count++];
            run->sid=si;
            run->c0=c0;
            run->c1=c1;
            run->iv0=iv0;
            run->iv1=iv1;
            run->left_real=left_real;
            run->right_real=right_real;
        }

        if(w->z0==0&&w->z1==29) {
            int16_t span=(int16_t)(c1-c0+1);
            int16_t iq=(int16_t)iv0<<6,step=0;
            int8_t cc;
            if(span>1) {
                uint8_t d=(uint8_t)(span-1);
                step=shr_signed((int16_t)(
                    ((int16_t)iv1-(int16_t)iv0)*k_span_recip_q8[d]),2);
            }
            for(cc=c0;cc<=c1;++cc) {
                uint8_t inv=(uint8_t)((iq+32)>>6);
                if(inv>g_full_inv[(uint8_t)cc])
                    g_full_inv[(uint8_t)cc]=inv;
                iq=(int16_t)(iq+step);
            }
        }
    }

    E1_STAGE(4u);
    for(si=0u;si<run_count;++si) {
        const E1Run *run=&g_runs[si];
        const E1Surface *w=&k_e1x_segments[run->sid];
        int8_t cc;
        int16_t span=(int16_t)(run->c1-run->c0+1);
        int16_t iq=(int16_t)run->iv0<<6,step=0;

        if(span>1) {
            uint8_t d=(uint8_t)(span-1);
            step=shr_signed((int16_t)(
                ((int16_t)run->iv1-(int16_t)run->iv0)*k_span_recip_q8[d]),2);
        }

        for(cc=run->c0;cc<=run->c1;++cc) {
            uint8_t inv=(uint8_t)((iq+32)>>6),shade,border=0u;
            int16_t top,bot;
            int8_t rt,rb,rr;

            if(inv<g_full_inv[(uint8_t)cc]) {
                iq=(int16_t)(iq+step);
                continue;
            }

            shade=shade_for(inv,w->shade_bias);
            top=(int16_t)(E1_HORIZON-
                shr_signed(mul_s8_u8((int8_t)((int16_t)w->z1-(s->z_q4>>4)),inv),5));
            bot=(int16_t)(E1_HORIZON-
                shr_signed(mul_s8_u8((int8_t)((int16_t)w->z0-(s->z_q4>>4)),inv),5));
            rt=row_floor(top);
            rb=row_floor(bot);
            if(rt>rb) {
                int8_t t=rt;
                rt=rb;
                rb=t;
            }
            if(rt<0)rt=0;
            if(rb>=(int8_t)E1_ROWS)rb=(int8_t)(E1_ROWS-1u);
            if(cc==run->c0&&run->left_real)border|=1u;
            if(cc==run->c1&&run->right_real)border|=2u;

            for(rr=rt;rr<=rb;++rr) {
                uint16_t idx=k_row_base[(uint8_t)rr]+(uint8_t)cc;
                if(inv>=g_depth[idx]) {
                    uint8_t cap=(rr==rt)?E1_CAP_TOP:
                                ((rr==rb)?E1_CAP_BOTTOM:E1_CAP_NONE);
                    out[idx]=E1_TILE_FULL(shade,cap,border);
                    g_depth[idx]=inv;
                }
            }
            iq=(int16_t)(iq+step);
        }
    }
    E1_STAGE(0u);
}

uint8_t e1_room1_surface_count(void) {
    return E1_SURFACE_COUNT;
}
