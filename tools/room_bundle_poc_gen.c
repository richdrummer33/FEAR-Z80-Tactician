/*
 * Host-only independent room-bundle baker.
 *
 * This deliberately keeps the current heavyweight exact-output philosophy:
 * every camera frame is rasterized on the host, reduced to an exact GG name
 * table plus explicit 32-byte tile-pattern loads, and delta-patched against
 * the preceding frame. The experiment is about composability, NOT ROM size.
 *
 * Two different authored rooms share one canonical S-shaped seam. Each bundle
 * starts from a freshly reset simulated VRAM cache, explores its room, returns
 * into the mathematically safe seam leg, resets the dynamic cache there, and
 * must finish with the exact same canonical name-table words as it started.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar.h"
#include "polar_baked_composite.h"

#define BUNDLE_COUNT 10u
#define ROUTE_FRAMES 192u
#define MAX_SEGMENTS 64u
#define MAX_SCENE_VERTICES (MAX_SEGMENTS*2u)
#define MAX_SCENE_RECTS 12u
#define PATCH_MAX (2u + TSP_MAP_CELLS * 5u)
#define TILEPATCH_MAX (2u + TSP_MAP_CELLS * (2u + TSP_HOST_TILE_BYTES))
#define PI 3.14159265358979323846

static uint8_t g_view_term_steps=2u;

typedef struct V2 { double x,y; } V2;
typedef struct Seg {
    V2 a,b;
    double z0,z1;
    int8_t shade_bias;
} Seg;
typedef struct World {
    Seg seg[MAX_SEGMENTS];
    uint8_t count;
    TSPHostSceneVertex scene_vertices[MAX_SCENE_VERTICES];
    TSPHostSceneSegment scene_segments[MAX_SEGMENTS];
    TSPHostSceneLight scene_lights[1];
    TSPHostSceneRect scene_rects[MAX_SCENE_RECTS];
    TSPHostCompositeScene scene;
    uint8_t scene_vertex_count;
    uint8_t scene_rect_count;
    uint8_t lighting_stage;
    /* Window/porthole bundles need multiple depth layers in one screen
     * column. Keep the legacy nearest-surface bake untouched elsewhere. */
    uint8_t multi_surface;
} World;
typedef struct Pose {
    double x,y,z;
    uint8_t yaw;
} Pose;
typedef struct FramePack {
    uint16_t patch_len;
    uint16_t changed;
    uint16_t runs;
    uint16_t tile_len;
    uint16_t tile_loads;
    uint8_t patch[PATCH_MAX];
    uint8_t tile[TILEPATCH_MAX];
} FramePack;
typedef struct BundleStats {
    uint32_t patch_bytes;
    uint32_t tile_bytes;
    uint32_t tile_loads;
    uint16_t peak_tile_loads;
    uint16_t raw_peak_tile_loads;
    uint16_t scheduled_budget;
    uint16_t changed_words;
} BundleStats;
typedef struct TileJob {
    uint16_t release;
    uint16_t deadline;
    uint16_t slot;
    uint16_t assigned;
    uint8_t bytes[TSP_HOST_TILE_BYTES];
} TileJob;

static void die(const char *msg){
    fprintf(stderr,"fatal: %s\n",msg);
    exit(2);
}
static void add_seg_profile(World *w,double ax,double ay,double bx,double by,
                            double z0,double z1,int8_t bias,uint8_t profile){
    Seg *s;
    TSPHostSceneSegment *ls;
    uint8_t sid=w->count;
    uint8_t v0=w->scene_vertex_count;
    uint8_t v1=(uint8_t)(v0+1u);
    if(w->count>=MAX_SEGMENTS||v1>=MAX_SCENE_VERTICES)
        die("too many room PoC segments");
    s=&w->seg[w->count++];
    s->a.x=ax;s->a.y=ay;s->b.x=bx;s->b.y=by;
    s->z0=z0;s->z1=z1;s->shade_bias=bias;

    w->scene_vertices[v0].x=(int16_t)lround(ax);
    w->scene_vertices[v0].y=(int16_t)lround(ay);
    w->scene_vertices[v1].x=(int16_t)lround(bx);
    w->scene_vertices[v1].y=(int16_t)lround(by);
    w->scene_vertex_count=(uint8_t)(v1+1u);

    ls=&w->scene_segments[sid];
    ls->v0=v0;ls->v1=v1;
    ls->profile=profile;
    ls->blocks_light=1u;
    ls->light_front_sign=0;
    ls->visual_front_sign=0;
}
static void add_seg(World *w,double ax,double ay,double bx,double by,
                    double z0,double z1,int8_t bias){
    add_seg_profile(w,ax,ay,bx,by,z0,z1,bias,TSP_PROFILE_FULL);
}
static void add_rect(World *w,int16_t x0,int16_t y0,int16_t x1,int16_t y1,
                     int16_t floor_z,int16_t ceiling_z){
    TSPHostSceneRect *r;
    if(w->scene_rect_count>=MAX_SCENE_RECTS)die("too many room scene rectangles");
    r=&w->scene_rects[w->scene_rect_count++];
    r->x0=x0;r->y0=y0;r->x1=x1;r->y1=y1;
    r->floor_z=floor_z;r->ceiling_z=ceiling_z;
}
static void finalize_scene(World *w){
    w->scene.vertices=w->scene_vertices;
    w->scene.vertex_count=w->scene_vertex_count;
    w->scene.segments=w->scene_segments;
    w->scene.segment_count=w->count;
    w->scene.lights=w->scene_lights;
    w->scene.light_count=(uint8_t)(w->lighting_stage>=TSP_HOST_LIGHT_HARD?1u:0u);
    if(w->scene.light_count){
        /* All current authored room lights use the experiment response:
         * sixteen apparent wall levels plus a subtle two-step view term. */
        w->scene_lights[0].wall_angle_response=1u;
        w->scene_lights[0].view_term_strength=g_view_term_steps;
    }
    w->scene.rects=w->scene_rects;
    w->scene.rect_count=w->scene_rect_count;
}
static void add_transformed_exit_seg(World *w,double ax,double ay,double bx,double by){
    add_seg(w,152.0-ax,48.0-ay,152.0-bx,48.0-by,0,32,0);
}

static void transform_point(double x,double y,double tx,double ty,uint8_t rot,
                            double *ox,double *oy){
    double rx=x-36.0,ry=y-24.0,nx,ny;
    switch(rot&3u){
        case 1u:nx=-ry;ny=rx;break;
        case 2u:nx=-rx;ny=-ry;break;
        case 3u:nx=ry;ny=-rx;break;
        default:nx=rx;ny=ry;break;
    }
    *ox=tx+nx;*oy=ty+ny;
}
static void add_xform_seg(World *w,double ax,double ay,double bx,double by,
                          double tx,double ty,uint8_t rot){
    double x0,y0,x1,y1;
    transform_point(ax,ay,tx,ty,rot,&x0,&y0);
    transform_point(bx,by,tx,ty,rot,&x1,&y1);
    add_seg(w,x0,y0,x1,y1,0,32,0);
}
static void add_xform_seg_z(World *w,double ax,double ay,double bx,double by,
                            double tx,double ty,uint8_t rot,
                            double z0,double z1){
    double x0,y0,x1,y1;
    transform_point(ax,ay,tx,ty,rot,&x0,&y0);
    transform_point(bx,by,tx,ty,rot,&x1,&y1);
    add_seg(w,x0,y0,x1,y1,z0,z1,0);
}
static void add_canonical_seam(World *w,double tx,double ty,uint8_t rot,
                               double mouth_lo,double mouth_hi){
    add_xform_seg(w,0,-4,20,-4,tx,ty,rot);
    add_xform_seg(w,20,-4,20,20,tx,ty,rot);
    add_xform_seg(w,20,20,28,20,tx,ty,rot);
    add_xform_seg(w,28,20,36,mouth_lo,tx,ty,rot);
    add_xform_seg(w,12,4,12,28,tx,ty,rot);
    add_xform_seg(w,12,28,28,28,tx,ty,rot);
    add_xform_seg(w,28,28,36,mouth_hi,tx,ty,rot);
}
static void add_canonical_seam_z(World *w,double tx,double ty,uint8_t rot,
                                 double mouth_lo,double mouth_hi,double zbase){
    add_xform_seg_z(w,0,-4,20,-4,tx,ty,rot,zbase,zbase+32.0);
    add_xform_seg_z(w,20,-4,20,20,tx,ty,rot,zbase,zbase+32.0);
    add_xform_seg_z(w,20,20,28,20,tx,ty,rot,zbase,zbase+32.0);
    add_xform_seg_z(w,28,20,36,mouth_lo,tx,ty,rot,zbase,zbase+32.0);
    add_xform_seg_z(w,12,4,12,28,tx,ty,rot,zbase,zbase+32.0);
    add_xform_seg_z(w,12,28,28,28,tx,ty,rot,zbase,zbase+32.0);
    add_xform_seg_z(w,28,28,36,mouth_hi,tx,ty,rot,zbase,zbase+32.0);
}
static void add_two_portal_seams(World *w,double mouth_lo,double mouth_hi){
    add_canonical_seam(w,36.0,24.0,0u,mouth_lo,mouth_hi);
    add_canonical_seam(w,116.0,24.0,2u,mouth_lo,mouth_hi);
}

/* Shared S-shaped seam plus one room beyond its east aperture.
 *
 * seam:
 *   old aperture x=0, y=-4..4
 *   inner wall x=12, y=4..28
 *   inner wall x=20, y=-4..20
 *   new aperture x=36, y=20..28
 *
 * The route uses only the new/east room. The old side exists only to make the
 * seam geometry complete and symmetric for later predecessor handoff tests.
 */
static void make_linear_world(uint8_t bundle,World *w){
    double mouth_lo=bundle==0u?12.0:16.0;
    double mouth_hi=bundle==0u?36.0:32.0;
    double room_y0=bundle==0u?-28.0:-4.0;
    double room_y1=bundle==0u?76.0:52.0;

    memset(w,0,sizeof(*w));

    /* Canonical inner S-throat remains narrow. Only the final hidden leg
     * flares toward the room. This preserves the seam serialization proof
     * while avoiding player-width room apertures. */
    add_seg(w,0,-4,20,-4,0,32,0);
    add_seg(w,20,-4,20,20,0,32,0);
    add_seg(w,20,20,28,20,0,32,0);
    add_seg(w,28,20,36,mouth_lo,0,32,0);
    add_seg(w,12,4,12,28,0,32,0);
    add_seg(w,12,28,28,28,0,32,0);
    add_seg(w,28,28,36,mouth_hi,0,32,0);

    /* Exact 180-degree transformed exit throat. */
    add_transformed_exit_seg(w,0,-4,20,-4);
    add_transformed_exit_seg(w,20,-4,20,20);
    add_transformed_exit_seg(w,20,20,28,20);
    add_transformed_exit_seg(w,28,20,36,mouth_lo);
    add_transformed_exit_seg(w,12,4,12,28);
    add_transformed_exit_seg(w,12,28,28,28);
    add_transformed_exit_seg(w,28,28,36,mouth_hi);

    /* Main room. Bundle zero is intentionally very wide; bundle one remains
     * tighter so the stream demonstrates spatial rhythm rather than one
     * repeated corridor width. */
    add_seg(w,36,room_y0,36,mouth_lo,0,32,0);
    add_seg(w,36,mouth_hi,36,room_y1,0,32,0);
    add_seg(w,116,room_y0,116,(48.0-mouth_hi),0,32,0);
    add_seg(w,116,(48.0-mouth_lo),116,room_y1,0,32,0);
    add_seg(w,36,room_y0,116,room_y0,0,32,0);
    add_seg(w,116,room_y1,36,room_y1,0,32,0);

    add_rect(w,36,(int16_t)room_y0,116,(int16_t)room_y1,0,32);

    if(bundle==0u){
        /* Portal-shadow room: put the lamp BEHIND the aperture. The two flared
         * jambs are therefore deliberate shadow casters, producing diverging
         * opening-corner shadows plus long far-wall cuts. */
        w->scene_lights[0].x_q4=(int16_t)(28<<4);
        w->scene_lights[0].y_q4=(int16_t)(24<<4);
        w->scene_lights[0].height_q4=(uint8_t)(12<<4);
        w->scene_lights[0].radius_world=112u;
        w->scene_lights[0].intensity=255u;
    }else{
        /* Inset/spooky room: a low side light sits below a short baffle.
         * Light escaping around its ends should form a curious asymmetric pool
         * while the deeper occluder contributes a strong vertical shadow cut. */
        add_seg(w,68,8,84,8,0,32,0);
        add_seg(w,94,18,94,38,0,32,1);
        w->scene_lights[0].x_q4=(int16_t)(76<<4);
        w->scene_lights[0].y_q4=(int16_t)(0<<4);
        w->scene_lights[0].height_q4=(uint8_t)(8<<4);
        w->scene_lights[0].radius_world=88u;
        w->scene_lights[0].intensity=255u;
    }

    w->lighting_stage=TSP_HOST_LIGHT_POINT;
    finalize_scene(w);
}

static void make_split_world(World *w){
    const double mouth_lo=12.0,mouth_hi=36.0;
    memset(w,0,sizeof(*w));

    /* Portal 0: west entry. Portal 1: north exit. Portal 2: south exit.
     * All three are exact rigid transforms of the canonical hidden seam. */
    add_canonical_seam(w,36.0,24.0,0u,mouth_lo,mouth_hi);
    add_canonical_seam(w,76.0,88.0,3u,mouth_lo,mouth_hi);
    add_canonical_seam(w,76.0,-40.0,1u,mouth_lo,mouth_hi);

    /* Broad T-room. The two exit mouths are simultaneously visible from much
     * of the centre, while each successor remains hidden beyond its S-throat. */
    add_seg(w,36,-40,36,12,0,32,0);
    add_seg(w,36,36,36,88,0,32,0);
    add_seg(w,116,-40,116,88,0,32,0);
    add_seg(w,36,88,64,88,0,32,0);
    add_seg(w,88,88,116,88,0,32,0);
    add_seg(w,36,-40,64,-40,0,32,0);
    add_seg(w,88,-40,116,-40,0,32,0);

    /* Central divider gives the junction a readable silhouette without
     * preventing both branch mouths being visible from the main floor. */
    add_seg(w,94,10,94,38,0,32,1);

    add_rect(w,36,-40,116,88,0,32);
    w->lighting_stage=TSP_HOST_LIGHT_BASELINE;
    finalize_scene(w);
}

static void make_stair_world(World *w){
    memset(w,0,sizeof(*w));

    /* Entry floor zero, exit floor +4 after a clockwise quarter turn. */
    add_canonical_seam_z(w,36.0,24.0,0u,12.0,36.0,0.0);
    add_canonical_seam_z(w,76.0,80.0,3u,12.0,36.0,4.0);

    /* Outer L-shaped stairwell walls, partitioned by local floor band so wall
     * bottoms climb with the steps instead of extending below the floor. */
    add_seg(w,36,12,56,12,0,32,0);
    add_seg(w,56,12,72,12,1,33,0);
    add_seg(w,72,12,96,12,2,34,0);

    add_seg(w,96,12,96,52,2,34,0);
    add_seg(w,96,52,96,64,3,35,0);
    add_seg(w,96,64,96,80,4,36,0);

    add_seg(w,56,80,64,80,4,36,0);
    add_seg(w,88,80,96,80,4,36,0);

    add_seg(w,56,36,56,52,2,34,0);
    add_seg(w,56,52,56,64,3,35,0);
    add_seg(w,56,64,56,80,4,36,0);
    add_seg(w,36,36,56,36,0,32,0);

    /* Actual vertical riser faces. */
    add_seg(w,56,12,56,36,0,1,1);
    add_seg(w,72,12,72,36,1,2,1);
    add_seg(w,56,52,96,52,2,3,1);
    add_seg(w,56,64,96,64,3,4,1);

    /* Horizontal receivers: non-overlapping bands with matched ceiling shift. */
    add_rect(w,36,12,55,36,0,32);
    add_rect(w,56,12,71,36,1,33);
    add_rect(w,72,12,96,36,2,34);
    add_rect(w,56,37,96,51,2,34);
    add_rect(w,56,52,96,63,3,35);
    add_rect(w,56,64,96,80,4,36);

    w->lighting_stage=TSP_HOST_LIGHT_BASELINE;
    finalize_scene(w);
}

static void make_gallery_world(World *w){
    memset(w,0,sizeof(*w));
    add_two_portal_seams(w,8.0,40.0);

    add_seg(w,36,-48,36,8,0,32,0);
    add_seg(w,36,40,36,96,0,32,0);
    add_seg(w,116,-48,116,8,0,32,0);
    add_seg(w,116,40,116,96,0,32,0);
    add_seg(w,36,-48,116,-48,0,32,0);
    add_seg(w,116,96,36,96,0,32,0);

    /* Sparse architectural fins leave the centre broad and open. */
    add_seg(w,68,-48,68,-18,0,32,1);
    add_seg(w,90,66,90,96,0,32,1);

    add_rect(w,36,-48,116,96,0,32);
    w->lighting_stage=TSP_HOST_LIGHT_BASELINE;
    finalize_scene(w);
}

static void make_turn_world(World *w){
    memset(w,0,sizeof(*w));

    /* West entry, north exit: a genuine flat quarter-turn module. */
    add_canonical_seam(w,36.0,24.0,0u,12.0,36.0);
    add_canonical_seam(w,76.0,80.0,3u,12.0,36.0);

    add_seg(w,36,8,96,8,0,32,0);
    add_seg(w,96,8,96,80,0,32,0);
    add_seg(w,60,80,64,80,0,32,0);
    add_seg(w,88,80,96,80,0,32,0);
    add_seg(w,60,40,60,80,0,32,0);
    add_seg(w,36,40,60,40,0,32,0);
    add_seg(w,36,8,36,12,0,32,0);
    add_seg(w,36,36,36,40,0,32,0);

    /* Short corner baffle gives the inset light a deliberate shadow edge. */
    add_seg(w,80,48,92,48,0,32,1);

    add_rect(w,36,8,96,40,0,32);
    add_rect(w,60,40,96,80,0,32);

    w->scene_lights[0].x_q4=(int16_t)(88<<4);
    w->scene_lights[0].y_q4=(int16_t)(60<<4);
    w->scene_lights[0].height_q4=(uint8_t)(9<<4);
    w->scene_lights[0].radius_world=82u;
    w->scene_lights[0].intensity=255u;
    w->lighting_stage=TSP_HOST_LIGHT_POINT;
    finalize_scene(w);
}

static void make_step_world(World *w){
    static const int16_t x0[5]={36,52,68,84,100};
    static const int16_t x1[5]={51,67,83,99,116};
    static const int16_t z[5]={0,2,4,2,0};
    uint8_t i;
    memset(w,0,sizeof(*w));
    add_two_portal_seams(w,12.0,36.0);

    /* End walls around the wide entry/exit mouths. */
    add_seg(w,36,-4,36,12,0,32,0);
    add_seg(w,36,36,36,52,0,32,0);
    add_seg(w,116,-4,116,12,0,32,0);
    add_seg(w,116,36,116,52,0,32,0);

    for(i=0u;i<5u;++i){
        add_seg(w,x0[i],-4,x1[i],-4,z[i],z[i]+32,0);
        add_seg(w,x1[i],52,x0[i],52,z[i],z[i]+32,0);
        add_rect(w,x0[i],-4,x1[i],52,z[i],z[i]+32);
    }

    /* Two steps up, two steps down. */
    add_seg(w,52,-4,52,52,0,2,1);
    add_seg(w,68,-4,68,52,2,4,1);
    add_seg(w,84,-4,84,52,2,4,1);
    add_seg(w,100,-4,100,52,0,2,1);

    w->lighting_stage=TSP_HOST_LIGHT_BASELINE;
    finalize_scene(w);
}

static void make_pillar_world(World *w){
    memset(w,0,sizeof(*w));
    add_two_portal_seams(w,8.0,40.0);

    add_seg(w,36,-40,36,8,0,32,0);
    add_seg(w,36,40,36,88,0,32,0);
    add_seg(w,116,-40,116,8,0,32,0);
    add_seg(w,116,40,116,88,0,32,0);
    add_seg(w,36,-40,116,-40,0,32,0);
    add_seg(w,116,88,36,88,0,32,0);

    /* Two compact full-height pillars, offset away from the travel rail. */
    add_seg(w,64,2,76,2,0,32,1);
    add_seg(w,76,2,76,16,0,32,1);
    add_seg(w,76,16,64,16,0,32,1);
    add_seg(w,64,16,64,2,0,32,1);

    add_seg(w,90,44,102,44,0,32,1);
    add_seg(w,102,44,102,58,0,32,1);
    add_seg(w,102,58,90,58,0,32,1);
    add_seg(w,90,58,90,44,0,32,1);

    add_rect(w,36,-40,116,88,0,32);

    /* Side light makes both pillars cast long, readable room-scale shadows. */
    w->scene_lights[0].x_q4=(int16_t)(76<<4);
    w->scene_lights[0].y_q4=(int16_t)(76<<4);
    w->scene_lights[0].height_q4=(uint8_t)(12<<4);
    w->scene_lights[0].radius_world=120u;
    w->scene_lights[0].intensity=255u;
    w->lighting_stage=TSP_HOST_LIGHT_POINT;
    finalize_scene(w);
}

static void make_world(uint8_t bundle,World *w){
    if(bundle<2u)make_linear_world(bundle,w);
    else if(bundle==2u)make_split_world(w);
    else if(bundle==3u)make_stair_world(w);
    else if(bundle==4u)make_gallery_world(w);
    else if(bundle==5u)make_turn_world(w);
    else if(bundle==6u)make_step_world(w);
    else if(bundle==7u)make_pillar_world(w);
    else die("invalid room bundle id");
}

static uint8_t yaw_lerp(uint8_t a,uint8_t b,double q){
    int d=(int)(int8_t)(b-a);
    int v=(int)a+(int)lround((double)d*q);
    return (uint8_t)v;
}
static double lerp(double a,double b,double q){return a+(b-a)*q;}

static Pose entry_outbound_pose(uint16_t f){
    Pose p;
    double q;
    p.z=16.0;
    if(f<16u){
        q=(double)f/15.0;
        p.x=16.0;p.y=lerp(12.0,16.0,q);p.yaw=64u;return p;
    }
    if(f<32u){
        q=(double)(f-16u)/15.0;
        p.x=16.0;p.y=lerp(16.0,24.0,q);p.yaw=yaw_lerp(64u,0u,q);return p;
    }
    q=(double)(f-32u)/31.0;
    p.x=lerp(16.0,62.0,q);p.y=24.0;p.yaw=0u;return p;
}

static Pose exit_transform(Pose p){
    p.x=152.0-p.x;
    p.y=48.0-p.y;
    p.yaw=(uint8_t)(p.yaw+128u);
    return p;
}

static Pose portal_transform_pose(Pose p,uint8_t portal){
    double tx,ty,nx,ny;
    uint8_t rot;
    if(portal==0u){tx=36.0;ty=24.0;rot=0u;}
    else if(portal==1u){tx=76.0;ty=88.0;rot=3u;}
    else if(portal==2u){tx=76.0;ty=-40.0;rot=1u;}
    else die("invalid split portal");
    transform_point(p.x,p.y,tx,ty,rot,&nx,&ny);
    p.x=nx;p.y=ny;p.yaw=(uint8_t)(p.yaw+(uint8_t)(rot*64u));
    return p;
}
static Pose portal_transform_pose_z(Pose p,double tx,double ty,uint8_t rot,double zbase){
    double nx,ny;
    transform_point(p.x,p.y,tx,ty,rot,&nx,&ny);
    p.x=nx;p.y=ny;p.z+=zbase;
    p.yaw=(uint8_t)(p.yaw+(uint8_t)(rot*64u));
    return p;
}
static double stair_floor_z(double x,double y){
    if(y>=64.0)return 4.0;
    if(y>=52.0&&x>=56.0)return 3.0;
    if(y>=36.0&&x>=56.0)return 2.0;
    if(x>=72.0)return 2.0;
    if(x>=56.0)return 1.0;
    return 0.0;
}
static double step_room_floor_z(double x){
    if(x<52.0)return 0.0;
    if(x<68.0)return 2.0;
    if(x<84.0)return 4.0;
    if(x<100.0)return 2.0;
    return 0.0;
}
static uint8_t yaw_from_vec(double dx,double dy){
    double a=atan2(dy,dx);
    int v=(int)lround(a*(256.0/(2.0*PI)));
    return (uint8_t)v;
}

static Pose route_pose(uint16_t f,uint8_t bundle){
    Pose p;
    double q,inspect_y=30.0;

    if(bundle==0u)inspect_y=52.0;      /* wide portal-shadow room */
    else if(bundle==1u)inspect_y=46.0; /* clear the inset baffle */
    else if(bundle==4u)inspect_y=54.0; /* broad gallery */
    else if(bundle==6u)inspect_y=44.0; /* stepped room */
    else if(bundle==7u)inspect_y=70.0; /* sweep around both pillars */

    /* 0..63: canonical entry seam -> inside room. */
    if(f<64u)return entry_outbound_pose(f);

    p.z=16.0;

    /* 64..79: lateral excursion with an intentional side-look. */
    if(f<80u){
        q=(double)(f-64u)/15.0;
        p.x=62.0;
        p.y=lerp(24.0,inspect_y,q);
        p.yaw=yaw_lerp(0u,56u,q);
        return p;
    }

    /* 80..95: travel deep along the offset line, still not facing perfectly
     * along motion. This is the closest analogue to the old scripted strafe. */
    if(f<96u){
        q=(double)(f-80u)/15.0;
        p.x=lerp(62.0,106.0,q);
        p.y=inspect_y;
        p.yaw=yaw_lerp(56u,16u,q);
        return p;
    }

    /* 96..111: cross back toward the exit line while turning to look behind.
     * On the pillar room this happens after the camera has passed the pillars,
     * avoiding an invalid through-solid path while still stressing occlusion. */
    if(f<112u){
        q=(double)(f-96u)/15.0;
        p.x=106.0;
        p.y=lerp(inspect_y,24.0,q);
        p.yaw=yaw_lerp(16u,176u,q);
        return p;
    }

    /* 112..127: settle onto the exit anchor while mostly looking back toward
     * the room we just crossed. */
    q=(double)(f-112u)/15.0;
    p.x=lerp(106.0,90.0,q);
    p.y=24.0;
    p.yaw=yaw_lerp(176u,128u,q);
    if(f<128u)return p;

    /* 128..191: back through the exit throat while still looking toward the
     * old room until the seam mathematically occludes it. */
    p=entry_outbound_pose((uint16_t)(191u-f));
    return exit_transform(p);
}

static Pose split_route_pose(uint16_t f,uint8_t entry_portal,uint8_t exit_portal){
    Pose a,b,p;
    double q,cx=76.0,cy=24.0;
    if(entry_portal>2u||exit_portal>2u||entry_portal==exit_portal)
        die("invalid split route pair");

    if(f<64u)
        return portal_transform_pose(entry_outbound_pose(f),entry_portal);

    if(f>=128u)
        return portal_transform_pose(entry_outbound_pose((uint16_t)(191u-f)),exit_portal);

    a=portal_transform_pose(entry_outbound_pose(63u),entry_portal);
    b=portal_transform_pose(entry_outbound_pose(63u),exit_portal);
    q=(double)(f-64u)/63.0;

    {
        double u=1.0-q;
        p.x=u*u*a.x+2.0*u*q*cx+q*q*b.x;
        p.y=u*u*a.y+2.0*u*q*cy+q*q*b.y;
        p.z=16.0;
    }
    {
        double dx=2.0*(1.0-q)*(cx-a.x)+2.0*q*(b.x-cx);
        double dy=2.0*(1.0-q)*(cy-a.y)+2.0*q*(b.y-cy);
        int look=(int)lround(sin(q*PI)*32.0);
        if(((uint8_t)(entry_portal+exit_portal)&1u)==0u)look=-look;
        p.yaw=(uint8_t)(yaw_from_vec(dx,dy)+(uint8_t)look);
    }
    return p;
}

static Pose stair_forward_pose(uint16_t f){
    Pose p,a,b;
    double q,cx=84.0,cy=36.0;
    if(f<64u){
        p=entry_outbound_pose(f);
        p.z=16.0+stair_floor_z(p.x,p.y);
        return p;
    }
    if(f>=128u){
        p=entry_outbound_pose((uint16_t)(191u-f));
        return portal_transform_pose_z(p,76.0,80.0,3u,4.0);
    }

    a=entry_outbound_pose(63u);
    a.z=16.0+stair_floor_z(a.x,a.y);
    b=portal_transform_pose_z(entry_outbound_pose(63u),76.0,80.0,3u,4.0);
    q=(double)(f-64u)/63.0;
    {
        double u=1.0-q;
        p.x=u*u*a.x+2.0*u*q*cx+q*q*b.x;
        p.y=u*u*a.y+2.0*u*q*cy+q*q*b.y;
        p.z=16.0+stair_floor_z(p.x,p.y);
    }
    {
        double dx=2.0*(1.0-q)*(cx-a.x)+2.0*q*(b.x-cx);
        double dy=2.0*(1.0-q)*(cy-a.y)+2.0*q*(b.y-cy);
        p.yaw=(uint8_t)(yaw_from_vec(dx,dy)+(uint8_t)lround(sin(q*PI)*20.0));
    }
    return p;
}

static Pose turn_forward_pose(uint16_t f){
    Pose p,a,b;
    double q,cx=86.0,cy=38.0;
    if(f<64u)return entry_outbound_pose(f);
    if(f>=128u)
        return portal_transform_pose_z(entry_outbound_pose((uint16_t)(191u-f)),
                                       76.0,80.0,3u,0.0);

    a=entry_outbound_pose(63u);
    b=portal_transform_pose_z(entry_outbound_pose(63u),76.0,80.0,3u,0.0);
    q=(double)(f-64u)/63.0;
    {
        double u=1.0-q;
        p.x=u*u*a.x+2.0*u*q*cx+q*q*b.x;
        p.y=u*u*a.y+2.0*u*q*cy+q*q*b.y;
        p.z=16.0;
    }
    {
        double dx=2.0*(1.0-q)*(cx-a.x)+2.0*q*(b.x-cx);
        double dy=2.0*(1.0-q)*(cy-a.y)+2.0*q*(b.y-cy);
        p.yaw=(uint8_t)(yaw_from_vec(dx,dy)+(uint8_t)lround(sin(q*PI)*24.0));
    }
    return p;
}

static Pose route_pose_portals(uint16_t f,uint8_t bundle,
                               uint8_t entry_portal,uint8_t exit_portal){
    if(bundle==0u||bundle==1u||bundle==4u||bundle==6u||bundle==7u){
        Pose p;
        if(entry_portal==0u&&exit_portal==1u)
            p=route_pose(f,bundle);
        else if(entry_portal==1u&&exit_portal==0u)
            p=route_pose((uint16_t)(ROUTE_FRAMES-1u-f),bundle);
        else{
            die("invalid straight room route pair");
            return route_pose(f,0u);
        }
        if(bundle==6u)p.z=16.0+step_room_floor_z(p.x);
        return p;
    }
    if(bundle==2u)return split_route_pose(f,entry_portal,exit_portal);
    if(bundle==3u){
        if(entry_portal==0u&&exit_portal==1u)return stair_forward_pose(f);
        if(entry_portal==1u&&exit_portal==0u)
            return stair_forward_pose((uint16_t)(ROUTE_FRAMES-1u-f));
        die("invalid stair route pair");
    }
    if(bundle==5u){
        if(entry_portal==0u&&exit_portal==1u)return turn_forward_pose(f);
        if(entry_portal==1u&&exit_portal==0u)
            return turn_forward_pose((uint16_t)(ROUTE_FRAMES-1u-f));
        die("invalid turn route pair");
    }
    die("invalid room bundle route");
    return route_pose(f,0u);
}

static int ray_seg(double ox,double oy,double dx,double dy,const Seg *s,double *t_out){
    double sx=s->b.x-s->a.x,sy=s->b.y-s->a.y;
    double den=dx*sy-dy*sx,qx,qy,t,u;
    if(fabs(den)<1e-10)return 0;
    qx=s->a.x-ox;qy=s->a.y-oy;
    t=(qx*sy-qy*sx)/den;
    u=(qx*dy-qy*dx)/den;
    if(t<=1e-6||u<-1e-8||u>1.0+1e-8)return 0;
    *t_out=t;return 1;
}
static uint8_t shade_for_inv(double inv,int8_t bias){
    int s=inv>=82.0?2:(inv>=46.0?1:0);
    s+=bias;if(s<0)s=0;if(s>2)s=2;return (uint8_t)s;
}
static int iround(double v){return (int)floor(v+0.5);}

static void render_pose(const World *w,const Pose *p,uint16_t out[TSP_MAP_CELLS]){
    int sx;
    TSPState cam;
    memset(&cam,0,sizeof(cam));
    cam.x_q4=(int16_t)lround(p->x*16.0);
    cam.y_q4=(int16_t)lround(p->y*16.0);
    cam.z_q4=(int16_t)lround(p->z*16.0);
    cam.yaw=p->yaw;

    tsp_host_composite_set_lighting(w->lighting_stage,&cam);
    tsp_host_composite_begin_frame();

    for(sx=0;sx<160;++sx){
        double rel=atan(((double)sx+0.5-80.0)/80.0);
        double ang=(double)p->yaw*(2.0*PI/256.0)+rel;
        double dx=cos(ang),dy=sin(ang),best=1e30;
        int best_sid=-1;
        uint8_t sid;
        for(sid=0u;sid<w->count;++sid){
            double t;
            if(ray_seg(p->x,p->y,dx,dy,&w->seg[sid],&t)&&t<best){
                best=t;best_sid=(int)sid;
            }
        }
        if(best_sid>=0){
            const Seg *s=&w->seg[best_sid];
            double depth=best*cos(rel);
            double top,bottom,inv;
            int it,ib;
            if(depth<0.01)depth=0.01;
            top=72.0-(s->z1-p->z)*80.0/depth;
            bottom=72.0-(s->z0-p->z)*80.0/depth;
            inv=2560.0/depth;if(inv>255.0)inv=255.0;
            it=iround(top);ib=iround(bottom);
            tsp_host_composite_surface((uint8_t)(sx>>3),(uint8_t)sx,(uint8_t)sx,
                                       (int16_t)it,(int16_t)it,
                                       (int16_t)ib,(int16_t)ib,
                                       (uint8_t)best_sid,
                                       shade_for_inv(inv,s->shade_bias),
                                       0u,0u,0u);
        }
    }
    tsp_host_composite_export(out);
}

static size_t build_patch(const uint16_t *a,const uint16_t *b,uint8_t *dst,
                          uint16_t *changed_out,uint16_t *runs_out){
    size_t p=2u;
    uint16_t changed=0u,runs=0u;
    uint8_t row;
    for(row=0u;row<TSP_ROWS;++row){
        uint8_t x=0u;
        uint16_t base=(uint16_t)row*TSP_COLS;
        while(x<TSP_COLS){
            uint8_t start,count,c;
            while(x<TSP_COLS&&a[base+x]==b[base+x])++x;
            if(x>=TSP_COLS)break;
            start=x;
            while(x<TSP_COLS&&a[base+x]!=b[base+x])++x;
            count=(uint8_t)(x-start);
            if(p+3u+(size_t)count*2u>PATCH_MAX)die("patch overflow");
            dst[p++]=row;dst[p++]=start;dst[p++]=count;
            for(c=0u;c<count;++c){
                uint16_t v=b[base+(uint16_t)start+c];
                dst[p++]=(uint8_t)v;dst[p++]=(uint8_t)(v>>8);
            }
            changed=(uint16_t)(changed+count);++runs;
        }
    }
    dst[0]=(uint8_t)runs;dst[1]=(uint8_t)(runs>>8);
    *changed_out=changed;*runs_out=runs;
    return p;
}
static int apply_patch(uint16_t *map,const uint8_t *src,size_t len){
    size_t p=2u;
    uint16_t n,i;
    if(len<2u)return 0;
    n=(uint16_t)src[0]|((uint16_t)src[1]<<8);
    for(i=0u;i<n;++i){
        uint8_t row,x,count,c;
        uint16_t base;
        if(p+3u>len)return 0;
        row=src[p++];x=src[p++];count=src[p++];
        if(row>=TSP_ROWS||!count||(uint16_t)x+count>TSP_COLS)return 0;
        if(p+(size_t)count*2u>len)return 0;
        base=(uint16_t)row*TSP_COLS+x;
        for(c=0u;c<count;++c){
            map[base+c]=(uint16_t)src[p]|((uint16_t)src[p+1u]<<8);
            p+=2u;
        }
    }
    return p==len;
}
static void capture_tiles(FramePack *fp){
    uint16_t n=tsp_host_composite_frame_load_count(),i;
    const TSPHostTileLoad *loads=tsp_host_composite_frame_loads();
    size_t p=2u;
    fp->tile_loads=n;
    fp->tile[p?0:0]=(uint8_t)n;
    fp->tile[1]=(uint8_t)(n>>8);
    for(i=0u;i<n;++i){
        if(p+2u+TSP_HOST_TILE_BYTES>TILEPATCH_MAX)die("tilepatch overflow");
        fp->tile[p++]=(uint8_t)loads[i].slot;
        fp->tile[p++]=(uint8_t)(loads[i].slot>>8);
        memcpy(fp->tile+p,loads[i].bytes,TSP_HOST_TILE_BYTES);
        p+=TSP_HOST_TILE_BYTES;
    }
    fp->tile_len=(uint16_t)p;
}

static uint16_t frame_tile_loads(const FramePack *fp){
    return (uint16_t)fp->tile[0]|((uint16_t)fp->tile[1]<<8);
}

/* Same release/deadline model as polar_demo_patch_gen.c, but frame zero is
 * deliberately excluded: it is the canonical seam bootstrap. Every normal
 * runtime frame receives the smallest steady-state tile-upload budget that
 * can satisfy all slot-use constraints. */
static uint16_t schedule_bundle_tiles(FramePack frames[ROUTE_FRAMES],
                                      const uint16_t *maps){
    uint32_t job_count=0u,j=0u;
    TileJob *jobs;
    int16_t last_use[512];
    uint16_t t,i;
    uint16_t chosen=0u,budget;

    for(t=1u;t<ROUTE_FRAMES;++t)job_count+=frame_tile_loads(&frames[t]);
    jobs=(TileJob *)malloc((job_count?job_count:1u)*sizeof(TileJob));
    if(!jobs)die("bundle tile scheduler allocation failed");
    for(i=0u;i<512u;++i)last_use[i]=-1;
    for(i=0u;i<TSP_MAP_CELLS;++i){
        uint16_t slot=maps[i]&TSP_TILE_ID_MASK;
        last_use[slot]=0;
    }

    for(t=1u;t<ROUTE_FRAMES;++t){
        const uint8_t *p=frames[t].tile+2u;
        uint16_t n=frame_tile_loads(&frames[t]),q;
        for(q=0u;q<n;++q){
            uint16_t slot=(uint16_t)p[0]|((uint16_t)p[1]<<8);
            p+=2u;
            jobs[j].release=(uint16_t)(last_use[slot]+1);
            jobs[j].deadline=t;
            jobs[j].slot=slot;
            jobs[j].assigned=0xffffu;
            memcpy(jobs[j].bytes,p,TSP_HOST_TILE_BYTES);
            p+=TSP_HOST_TILE_BYTES;
            ++j;
        }
        for(i=0u;i<TSP_MAP_CELLS;++i){
            uint16_t slot=maps[(size_t)t*TSP_MAP_CELLS+i]&TSP_TILE_ID_MASK;
            last_use[slot]=(int16_t)t;
        }
    }
    if(j!=job_count)die("bundle tile scheduler job count mismatch");

    for(budget=1u;budget<=48u&&!chosen;++budget){
        uint32_t done=0u;
        for(j=0u;j<job_count;++j)jobs[j].assigned=0xffffu;
        for(t=1u;t<ROUTE_FRAMES;++t){
            uint16_t k;
            for(k=0u;k<budget;++k){
                uint32_t best=UINT32_MAX,x;
                uint16_t best_deadline=0xffffu;
                for(x=0u;x<job_count;++x){
                    if(jobs[x].assigned==0xffffu &&
                       jobs[x].release<=t &&
                       jobs[x].deadline<best_deadline){
                        best=x;
                        best_deadline=jobs[x].deadline;
                    }
                }
                if(best==UINT32_MAX)break;
                jobs[best].assigned=t;
                ++done;
            }
            for(j=0u;j<job_count;++j){
                if(jobs[j].assigned==0xffffu&&jobs[j].deadline==t)break;
            }
            if(j<job_count)break;
        }
        if(done==job_count)chosen=budget;
    }
    if(!chosen)die("bundle tile scheduler needs more than 48 uploads/VBlank");

    for(t=1u;t<ROUTE_FRAMES;++t){
        uint16_t n=0u;
        size_t p=2u;
        for(j=0u;j<job_count;++j)if(jobs[j].assigned==t)++n;
        frames[t].tile[0]=(uint8_t)n;
        frames[t].tile[1]=(uint8_t)(n>>8);
        frames[t].tile_loads=n;
        for(j=0u;j<job_count;++j)if(jobs[j].assigned==t){
            if(p+2u+TSP_HOST_TILE_BYTES>TILEPATCH_MAX)
                die("scheduled bundle tilepatch overflow");
            frames[t].tile[p++]=(uint8_t)jobs[j].slot;
            frames[t].tile[p++]=(uint8_t)(jobs[j].slot>>8);
            memcpy(frames[t].tile+p,jobs[j].bytes,TSP_HOST_TILE_BYTES);
            p+=TSP_HOST_TILE_BYTES;
        }
        frames[t].tile_len=(uint16_t)p;
    }

    free(jobs);
    return chosen;
}

static uint64_t fnv64(const void *data,size_t n){
    const uint8_t *p=(const uint8_t *)data;
    uint64_t h=UINT64_C(1469598103934665603);
    size_t i;
    for(i=0u;i<n;++i){h^=p[i];h*=UINT64_C(1099511628211);}
    return h;
}

static void write_u16(FILE *f,uint16_t v){fputc((int)(v&255u),f);fputc((int)(v>>8),f);}
static void write_u32(FILE *f,uint32_t v){write_u16(f,(uint16_t)v);write_u16(f,(uint16_t)(v>>16));}

static void emit_route(FILE *pack,FILE *manifest,uint8_t bundle,
                       uint8_t entry_portal,uint8_t exit_portal,
                       FramePack frames[ROUTE_FRAMES],
                       BundleStats *stats){
    uint16_t i;
    fprintf(manifest,
            "bundle=%u route=%u->%u frames=%u patch_bytes=%lu tile_bytes=%lu tile_loads=%lu raw_peak_tile_loads=%u scheduled_peak=%u scheduled_budget=%u changed_words=%u\n",
            (unsigned)bundle,(unsigned)entry_portal,(unsigned)exit_portal,
            (unsigned)ROUTE_FRAMES,
            (unsigned long)stats->patch_bytes,(unsigned long)stats->tile_bytes,
            (unsigned long)stats->tile_loads,
            (unsigned)stats->raw_peak_tile_loads,
            (unsigned)stats->peak_tile_loads,
            (unsigned)stats->scheduled_budget,
            (unsigned)stats->changed_words);
    printf("ROOM_BUNDLE_STATS bundle=%u route=%u->%u frames=%u patch_bytes=%lu tile_bytes=%lu tile_loads=%lu raw_peak=%u scheduled_peak=%u scheduled_budget=%u changed_words=%u\n",
           (unsigned)bundle,(unsigned)entry_portal,(unsigned)exit_portal,
           (unsigned)ROUTE_FRAMES,
           (unsigned long)stats->patch_bytes,(unsigned long)stats->tile_bytes,
           (unsigned long)stats->tile_loads,
           (unsigned)stats->raw_peak_tile_loads,
           (unsigned)stats->peak_tile_loads,
           (unsigned)stats->scheduled_budget,
           (unsigned)stats->changed_words);

    fputc((int)entry_portal,pack);
    fputc((int)exit_portal,pack);
    write_u16(pack,ROUTE_FRAMES);
    write_u32(pack,stats->patch_bytes);
    write_u32(pack,stats->tile_bytes);
    for(i=0u;i<ROUTE_FRAMES;++i){
        write_u16(pack,frames[i].patch_len);
        write_u16(pack,frames[i].tile_len);
        fwrite(frames[i].patch,1,frames[i].patch_len,pack);
        fwrite(frames[i].tile,1,frames[i].tile_len,pack);
    }
}

static void bake_route(const char *outdir,FILE *pack,FILE *manifest,
                       uint8_t bundle,World *w,
                       uint8_t entry_portal,uint8_t exit_portal,
                       uint16_t canonical[TSP_MAP_CELLS],
                       uint64_t *canonical_hash,uint8_t *canonical_ready){
    char path[512];
    FramePack *frames=(FramePack *)calloc(ROUTE_FRAMES,sizeof(FramePack));
    uint16_t *maps=(uint16_t *)malloc((size_t)ROUTE_FRAMES*TSP_MAP_CELLS*sizeof(uint16_t));
    uint16_t prev[TSP_MAP_CELLS],cur[TSP_MAP_CELLS],replay[TSP_MAP_CELLS];
    BundleStats stats={0};
    uint16_t f;
    if(!frames||!maps)die("bundle route allocation failed");

    tsp_host_composite_set_scene(&w->scene);
    tsp_host_composite_reset_cache();

    for(f=0u;f<ROUTE_FRAMES;++f){
        Pose p=route_pose_portals(f,bundle,entry_portal,exit_portal);

        /* Both route directions terminate inside the canonical hidden leg. */
        if(f==176u)tsp_host_composite_reset_cache();

        render_pose(w,&p,cur);
        capture_tiles(&frames[f]);
        memcpy(maps+(size_t)f*TSP_MAP_CELLS,cur,sizeof(cur));

        if(f==0u){
            memcpy(prev,cur,sizeof(prev));
            memcpy(replay,cur,sizeof(replay));
            frames[f].patch_len=(uint16_t)build_patch(cur,cur,frames[f].patch,
                                                      &frames[f].changed,&frames[f].runs);
            if(!*canonical_ready){
                memcpy(canonical,cur,sizeof(cur));
                *canonical_hash=fnv64(canonical,sizeof(cur));
                *canonical_ready=1u;
            }else if(memcmp(canonical,cur,sizeof(cur))!=0){
                die("route initial seam name table != canonical seam");
            }
        }else{
            frames[f].patch_len=(uint16_t)build_patch(prev,cur,frames[f].patch,
                                                      &frames[f].changed,&frames[f].runs);
            memcpy(replay,prev,sizeof(replay));
            if(!apply_patch(replay,frames[f].patch,frames[f].patch_len)||
               memcmp(replay,cur,sizeof(cur))!=0)
                die("bundle route patch replay != oracle");
            memcpy(prev,cur,sizeof(prev));
        }

        stats.patch_bytes+=frames[f].patch_len;
        stats.tile_bytes+=frames[f].tile_len;
        stats.tile_loads+=frames[f].tile_loads;
        stats.changed_words=(uint16_t)(stats.changed_words+frames[f].changed);
        if(frames[f].tile_loads>stats.raw_peak_tile_loads)
            stats.raw_peak_tile_loads=frames[f].tile_loads;

        if(f==0u||f==64u||f==80u||f==96u||f==112u||f==176u||f==191u){
            snprintf(path,sizeof(path),"%s/bundle%u_route%u%u_frame%u.ppm",
                     outdir,(unsigned)bundle,(unsigned)entry_portal,
                     (unsigned)exit_portal,(unsigned)f);
            if(!tsp_host_composite_write_ppm(path))die("route screenshot write failed");
        }
    }

    if(memcmp(prev,canonical,sizeof(prev))!=0)
        die("route terminal seam name table != canonical seam");

    stats.scheduled_budget=schedule_bundle_tiles(frames,maps);
    stats.tile_bytes=0u;
    stats.tile_loads=0u;
    stats.peak_tile_loads=0u;
    for(f=0u;f<ROUTE_FRAMES;++f){
        stats.tile_bytes+=frames[f].tile_len;
        stats.tile_loads+=frames[f].tile_loads;
        if(frames[f].tile_loads>stats.peak_tile_loads)
            stats.peak_tile_loads=frames[f].tile_loads;
    }

    fprintf(manifest,
            "bundle=%u route=%u->%u canonical_begin=PASS canonical_end=PASS terminal_hash=%016llX\n",
            (unsigned)bundle,(unsigned)entry_portal,(unsigned)exit_portal,
            (unsigned long long)fnv64(prev,sizeof(prev)));
    emit_route(pack,manifest,bundle,entry_portal,exit_portal,frames,&stats);
    free(maps);
    free(frames);
}

int main(int argc,char **argv){
    const char *outdir;
    uint8_t wall_angle_enabled=1u;
    uint8_t quant_mode=TSP_HOST_LIGHT_QUANT_SOLID8;
    char path[512];
    FILE *pack,*manifest;
    uint16_t canonical[TSP_MAP_CELLS];
    uint64_t canonical_hash=0u;
    uint8_t canonical_ready=0u;
    uint8_t bundle;

    if(argc<2||argc>3){
        fprintf(stderr,
                "usage: %s OUTPUT_DIR [--legacy-binary-light|--wall-angle-no-view|--dither16-angle]\n",
                argv[0]);
        return 2;
    }
    if(argc==3){
        if(strcmp(argv[2],"--legacy-binary-light")==0){
            wall_angle_enabled=0u;
            quant_mode=TSP_HOST_LIGHT_QUANT_DITHER16;
        }else if(strcmp(argv[2],"--wall-angle-no-view")==0){
            g_view_term_steps=0u;
        }else if(strcmp(argv[2],"--dither16-angle")==0){
            quant_mode=TSP_HOST_LIGHT_QUANT_DITHER16;
        }else{
            fprintf(stderr,"unknown option: %s\n",argv[2]);
            return 2;
        }
    }
    outdir=argv[1];
    tsp_host_composite_set_wall_angle_mode(wall_angle_enabled);
    tsp_host_composite_set_wall_quant_mode(quant_mode);

    snprintf(path,sizeof(path),"%s/room_bundle_poc.pack",outdir);
    pack=fopen(path,"wb");if(!pack)die("cannot create room bundle pack");
    fwrite("RBP2",1,4,pack);
    write_u16(pack,2u);
    fputc(BUNDLE_COUNT,pack);
    fputc(0,pack);

    snprintf(path,sizeof(path),"%s/room_bundle_poc_manifest.txt",outdir);
    manifest=fopen(path,"w");if(!manifest)die("cannot create room bundle manifest");
    fprintf(manifest,"Room bundle PoC pack v2 - independently scheduled portal routes\n");
    fprintf(manifest,"wall_angle_light=%s quant=%s view_term_max_steps=%u\n",
            wall_angle_enabled?"ON":"LEGACY_BINARY",
            quant_mode==TSP_HOST_LIGHT_QUANT_SOLID8?"SOLID8":"DITHER16",
            (unsigned)(wall_angle_enabled?g_view_term_steps:0u));

    for(bundle=0u;bundle<BUNDLE_COUNT;++bundle){
        World w;
        make_world(bundle,&w);
        fputc((int)bundle,pack);
        if(bundle==2u){
            static const uint8_t pairs[6][2]={{0,1},{1,0},{0,2},{2,0},{1,2},{2,1}};
            uint8_t r;
            fputc(6,pack);
            write_u16(pack,0u);
            for(r=0u;r<6u;++r)
                bake_route(outdir,pack,manifest,bundle,&w,pairs[r][0],pairs[r][1],
                           canonical,&canonical_hash,&canonical_ready);
        }else{
            fputc(2,pack);
            write_u16(pack,0u);
            bake_route(outdir,pack,manifest,bundle,&w,0u,1u,
                       canonical,&canonical_hash,&canonical_ready);
            bake_route(outdir,pack,manifest,bundle,&w,1u,0u,
                       canonical,&canonical_hash,&canonical_ready);
        }
    }

    if(!canonical_ready)die("canonical seam was never established");
    fprintf(manifest,"canonical_seam_fnv64=%016llX\n",(unsigned long long)canonical_hash);
    fprintf(manifest,"independent_bundle_replay=PASS\n");
    fprintf(manifest,"cross_bundle_canonical_handoff=PASS\n");
    fprintf(manifest,"bidirectional_portal_routes=PASS\n");
    fprintf(manifest,"three_portal_split_routes=PASS\n");
    fprintf(manifest,"quarter_stair_height_rebase_routes=PASS\n");
    fprintf(manifest,"eight_module_catalog=PASS\n");

    snprintf(path,sizeof(path),"%s/room_bundle_poc_canonical.bin",outdir);
    {
        FILE *cf=fopen(path,"wb");
        if(!cf)die("cannot create canonical seam map");
        if(fwrite(canonical,1,sizeof(canonical),cf)!=sizeof(canonical))
            die("canonical seam map write failed");
        fclose(cf);
    }

    fclose(manifest);fclose(pack);
    tsp_host_composite_set_scene((const TSPHostCompositeScene *)0);

    printf("ROOM_BUNDLE_POC_PASS bundles=%u ordinary_routes=2 split_routes=6 stair_routes=2 frames_per_route=%u canonical=%016llX wall_angle=%s quant=%s view_steps=%u\n",
           BUNDLE_COUNT,ROUTE_FRAMES,(unsigned long long)canonical_hash,
           wall_angle_enabled?"ON":"LEGACY_BINARY",
           quant_mode==TSP_HOST_LIGHT_QUANT_SOLID8?"SOLID8":"DITHER16",
           (unsigned)(wall_angle_enabled?g_view_term_steps:0u));
    return 0;
}
