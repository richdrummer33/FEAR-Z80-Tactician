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
#include "room_mesh_bake.h"

#ifdef ROOM_BUNDLE_DOOMGUY_GENERATED
#include "generated/doomguy_mesh.inc"
#ifdef ROOM_BUNDLE_DOOMGUY_SEAMS
#include "generated/doomguy_seams.inc"
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_SHADE_LEVELS
#define ROOM_BUNDLE_DOOMGUY_SHADE_LEVELS 1
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_LIGHTING_PROXY
#define ROOM_BUNDLE_DOOMGUY_LIGHTING_PROXY 0
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_CREASE_COVERAGE
#define ROOM_BUNDLE_DOOMGUY_CREASE_COVERAGE 0.30
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_CREASE_DEPTH
#define ROOM_BUNDLE_DOOMGUY_CREASE_DEPTH 0.55
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_DITHER
#define ROOM_BUNDLE_DOOMGUY_DITHER 1
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_CREASE
#define ROOM_BUNDLE_DOOMGUY_CREASE 1
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_STATIC_LIGHT
#define ROOM_BUNDLE_DOOMGUY_STATIC_LIGHT 1
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_INCIDENT
#define ROOM_BUNDLE_DOOMGUY_INCIDENT 0.60
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_AO_RADIUS
#define ROOM_BUNDLE_DOOMGUY_AO_RADIUS 2.5
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_AO_STRENGTH
#define ROOM_BUNDLE_DOOMGUY_AO_STRENGTH 0.65
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_LIGHT_RADIUS
#define ROOM_BUNDLE_DOOMGUY_LIGHT_RADIUS 0.0
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_SHADOW_FLOOR
#define ROOM_BUNDLE_DOOMGUY_SHADOW_FLOOR 0.45
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_RAMP
#define ROOM_BUNDLE_DOOMGUY_RAMP 5
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_SMOOTH
#define ROOM_BUNDLE_DOOMGUY_SMOOTH 1
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_CONSOLIDATE
#define ROOM_BUNDLE_DOOMGUY_CONSOLIDATE 0
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_CONSOLIDATE_PASSES
#define ROOM_BUNDLE_DOOMGUY_CONSOLIDATE_PASSES 1
#endif
#ifndef ROOM_BUNDLE_DOOMGUY_SEAM_COMPONENTS
#define ROOM_BUNDLE_DOOMGUY_SEAM_COMPONENTS 0
#endif
#endif

#ifdef ROOM_BUNDLE_BONSAI_GENERATED
#include "generated/bonsai_mesh.inc"
#ifdef ROOM_BUNDLE_BONSAI_SEAMS
#include "generated/bonsai_seams.inc"
#endif
#ifndef ROOM_BUNDLE_BONSAI_RAMP
#define ROOM_BUNDLE_BONSAI_RAMP 5
#endif
#ifndef ROOM_BUNDLE_BONSAI_SMOOTH
#define ROOM_BUNDLE_BONSAI_SMOOTH 1
#endif
#ifndef ROOM_BUNDLE_BONSAI_LIGHTING_PROXY
#define ROOM_BUNDLE_BONSAI_LIGHTING_PROXY 0
#endif
#ifndef ROOM_BUNDLE_BONSAI_STATIC_LIGHT
#define ROOM_BUNDLE_BONSAI_STATIC_LIGHT 1
#endif
/* Canopy scale, not seam scale: the occlusion probe has to reach across the
 * gap between a leaf mass and the branches under it, otherwise the underside
 * of the canopy never registers as enclosed. */
#ifndef ROOM_BUNDLE_BONSAI_AO_RADIUS
#define ROOM_BUNDLE_BONSAI_AO_RADIUS 8.0
#endif
#ifndef ROOM_BUNDLE_BONSAI_AO_STRENGTH
#define ROOM_BUNDLE_BONSAI_AO_STRENGTH 0.40
#endif
#ifndef ROOM_BUNDLE_BONSAI_LIGHT_RADIUS
#define ROOM_BUNDLE_BONSAI_LIGHT_RADIUS 0.0
#endif
#ifndef ROOM_BUNDLE_BONSAI_SHADOW_FLOOR
#define ROOM_BUNDLE_BONSAI_SHADOW_FLOOR 0.55
#endif
/*
 * Equalization spreads the ramp evenly over the surface, which is right for a
 * figure lit from the side but wrong for a top-lit tree: it forces a fifth of
 * the canopy into each band and so cancels the very contrast that makes an
 * underside look like an underside. Off keeps the measured brightness.
 */
#ifndef ROOM_BUNDLE_BONSAI_EQUALIZE
#define ROOM_BUNDLE_BONSAI_EQUALIZE 0
#endif
#ifndef ROOM_BUNDLE_BONSAI_CONSOLIDATE
#define ROOM_BUNDLE_BONSAI_CONSOLIDATE 4
#endif
#ifndef ROOM_BUNDLE_BONSAI_CONSOLIDATE_PASSES
#define ROOM_BUNDLE_BONSAI_CONSOLIDATE_PASSES 3
#endif
#ifndef ROOM_BUNDLE_BONSAI_SEAM_COMPONENTS
#define ROOM_BUNDLE_BONSAI_SEAM_COMPONENTS 0
#endif
#endif

#define BUNDLE_COUNT 13u
#define ROUTE_FRAMES 192u
#define MAX_SEGMENTS 64u
#define MAX_SCENE_VERTICES (MAX_SEGMENTS*2u)
#define MAX_SCENE_RECTS 12u
#define MAX_HSURFS 32u
#define PATCH_MAX (2u + TSP_MAP_CELLS * 5u)
#define TILEPATCH_MAX (2u + TSP_MAP_CELLS * (2u + TSP_HOST_TILE_BYTES))
#define PI 3.14159265358979323846

typedef struct V2 { double x,y; } V2;
typedef struct Seg {
    V2 a,b;
    double z0,z1;
    int8_t shade_bias;
} Seg;
typedef struct HSurf {
    V2 p[4];
    double z;
    int8_t shade_bias;
} HSurf;
typedef struct World {
    Seg seg[MAX_SEGMENTS];
    uint8_t count;
    HSurf hsurf[MAX_HSURFS];
    uint8_t hsurf_count;
    TSPHostSceneVertex scene_vertices[MAX_SCENE_VERTICES];
    TSPHostSceneSegment scene_segments[MAX_SEGMENTS];
    TSPHostSceneLight scene_lights[1];
    TSPHostSceneRect scene_rects[MAX_SCENE_RECTS];
    TSPHostCompositeScene scene;
    RMBScene mesh;
    uint8_t scene_vertex_count;
    uint8_t scene_rect_count;
    uint8_t lighting_stage;
} World;
typedef struct Pose {
    double x,y,z;
    uint8_t yaw;
} Pose;
typedef struct RenderHit {
    double t;
    uint8_t sid;
} RenderHit;
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
static void add_seg(World *w,double ax,double ay,double bx,double by,
                    double z0,double z1,int8_t bias){
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
    w->scene_vertices[v0].x_q4=(int16_t)lround(ax*16.0);
    w->scene_vertices[v0].y_q4=(int16_t)lround(ay*16.0);
    w->scene_vertices[v0].has_exact_q4=1u;
    w->scene_vertices[v1].x=(int16_t)lround(bx);
    w->scene_vertices[v1].y=(int16_t)lround(by);
    w->scene_vertices[v1].x_q4=(int16_t)lround(bx*16.0);
    w->scene_vertices[v1].y_q4=(int16_t)lround(by*16.0);
    w->scene_vertices[v1].has_exact_q4=1u;
    w->scene_vertex_count=(uint8_t)(v1+1u);

    ls=&w->scene_segments[sid];
    ls->v0=v0;ls->v1=v1;
    ls->profile=TSP_PROFILE_FULL;
    ls->blocks_light=1u;
    ls->light_front_sign=0;
    ls->visual_front_sign=0;
    ls->z0_q4=(int16_t)lround(z0*16.0);
    ls->z1_q4=(int16_t)lround(z1*16.0);
    ls->has_exact_z=1u;
}

static void add_hsurf_quad(World *w,V2 a,V2 b,V2 c,V2 d,
                           double z,int8_t bias){
    HSurf *s;
    if(w->hsurf_count>=MAX_HSURFS)die("too many horizontal room surfaces");
    s=&w->hsurf[w->hsurf_count++];
    s->p[0]=a;s->p[1]=b;s->p[2]=c;s->p[3]=d;
    s->z=z;s->shade_bias=bias;
}

/*
 * Host-only volumetric wall authoring.
 *
 * The legacy room PoC stores every vertical face as an infinitely thin Seg.
 * That is still the right representation for room/perimeter boundaries, but
 * it is a poor authoring primitive for free-standing interior walls: authors
 * otherwise have to draw both broad sides plus every exposed end/reveal by
 * hand. These helpers keep the runtime format unchanged and expand one
 * centerline into the exposed vertical faces of a rectangular wall prism.
 *
 * Full-height solids are therefore closed laterally by construction. Horizontal
 * cap/reveal planes (table tops, window sills/lintels) are deliberately not
 * emitted here because the current room baker is a vertical-wall ray caster.
 */
#define SOLID_WALL_DEFAULT_THICKNESS 1.0

static V2 v2_lerp(V2 a,V2 b,double q){
    V2 p;
    p.x=a.x+(b.x-a.x)*q;
    p.y=a.y+(b.y-a.y)*q;
    return p;
}

static void wall_offsets(double ax,double ay,double bx,double by,double thickness,
                         V2 *a_left,V2 *a_right,V2 *b_left,V2 *b_right,
                         double *length_out){
    double dx=bx-ax,dy=by-ay,len=sqrt(dx*dx+dy*dy);
    double nx,ny,h;
    if(len<=1e-9)die("solid wall line has zero length");
    if(thickness<=0.0)die("solid wall thickness must be positive");
    nx=-dy/len;
    ny= dx/len;
    h=thickness*0.5;
    a_left->x=ax+nx*h; a_left->y=ay+ny*h;
    a_right->x=ax-nx*h;a_right->y=ay-ny*h;
    b_left->x=bx+nx*h; b_left->y=by+ny*h;
    b_right->x=bx-nx*h;b_right->y=by-ny*h;
    if(length_out)*length_out=len;
}

static void add_solid_wall_line_caps(World *w,
                                     double ax,double ay,double bx,double by,
                                     double thickness,double z0,double z1,
                                     int8_t bias,uint8_t cap_a,uint8_t cap_b){
    V2 al,ar,bl,br;
    wall_offsets(ax,ay,bx,by,thickness,&al,&ar,&bl,&br,(double *)0);

    /* The two broad faces plus only the physically exposed end caps. */
    add_seg(w,al.x,al.y,bl.x,bl.y,z0,z1,bias);
    add_seg(w,br.x,br.y,ar.x,ar.y,z0,z1,bias);
    if(cap_a)add_seg(w,ar.x,ar.y,al.x,al.y,z0,z1,bias);
    if(cap_b)add_seg(w,bl.x,bl.y,br.x,br.y,z0,z1,bias);
}

static void add_solid_wall_line(World *w,
                                double ax,double ay,double bx,double by,
                                double thickness,double z0,double z1,
                                int8_t bias){
    add_solid_wall_line_caps(w,ax,ay,bx,by,thickness,z0,z1,bias,1u,1u);
}

/*
 * A rectangular window cut through a solid wall. open_from/open_to are world
 * distances measured along the authored centerline from A toward B.
 *
 * The broad front/back faces are split above, below and beside the opening,
 * and the two vertical jamb/reveal faces across wall thickness are generated
 * automatically. The sill and lintel underside are emitted as horizontal
 * quads too, completing the visible four-sided reveal of the opening.
 */
static void add_solid_window_line_caps(World *w,
                                       double ax,double ay,double bx,double by,
                                       double thickness,
                                       double open_from,double open_to,
                                       double open_z0,double open_z1,
                                       double z0,double z1,int8_t bias,
                                       uint8_t cap_a,uint8_t cap_b){
    V2 al,ar,bl,br,l0,l1,r0,r1;
    double len,u0,u1;
    wall_offsets(ax,ay,bx,by,thickness,&al,&ar,&bl,&br,&len);
    if(open_from<=0.0||open_to>=len||open_from>=open_to)
        die("window opening must lie strictly inside wall endpoints");
    if(open_z0<=z0||open_z1>=z1||open_z0>=open_z1)
        die("window vertical opening must lie strictly inside wall height");

    u0=open_from/len;
    u1=open_to/len;
    l0=v2_lerp(al,bl,u0);l1=v2_lerp(al,bl,u1);
    r0=v2_lerp(ar,br,u0);r1=v2_lerp(ar,br,u1);

    /* Left/broad side. */
    add_seg(w,al.x,al.y,l0.x,l0.y,z0,z1,bias);
    add_seg(w,l0.x,l0.y,l1.x,l1.y,z0,open_z0,bias);
    add_seg(w,l0.x,l0.y,l1.x,l1.y,open_z1,z1,bias);
    add_seg(w,l1.x,l1.y,bl.x,bl.y,z0,z1,bias);

    /* Right/broad side, reverse winding for outward consistency. */
    add_seg(w,br.x,br.y,r1.x,r1.y,z0,z1,bias);
    add_seg(w,r1.x,r1.y,r0.x,r0.y,z0,open_z0,bias);
    add_seg(w,r1.x,r1.y,r0.x,r0.y,open_z1,z1,bias);
    add_seg(w,r0.x,r0.y,ar.x,ar.y,z0,z1,bias);

    /* Physical wall ends. */
    if(cap_a)add_seg(w,ar.x,ar.y,al.x,al.y,z0,z1,bias);
    if(cap_b)add_seg(w,bl.x,bl.y,br.x,br.y,z0,z1,bias);

    /* The part the old thin-segment model could not infer: window jambs. */
    add_seg(w,r0.x,r0.y,l0.x,l0.y,open_z0,open_z1,bias);
    add_seg(w,l1.x,l1.y,r1.x,r1.y,open_z0,open_z1,bias);

    /* Horizontal reveal planes: sill top and lintel underside. They are
     * double-sided host surfaces; visibility is resolved solely by depth. */
    add_hsurf_quad(w,l0,l1,r1,r0,open_z0,bias);
    add_hsurf_quad(w,r0,r1,l1,l0,open_z1,bias);
}

static void self_test_solid_wall_geometry(void){
    World w;
    memset(&w,0,sizeof(w));
    add_solid_wall_line(&w,0.0,0.0,10.0,0.0,1.0,0.0,32.0,0);
    if(w.count!=4u)die("solid wall self-test: expected four vertical faces");

    memset(&w,0,sizeof(w));
    add_solid_window_line_caps(&w,0.0,0.0,10.0,0.0,1.0,
                               2.0,8.0,10.0,22.0,0.0,32.0,0,1u,1u);
    if(w.count!=12u)die("solid window self-test: expected 12 vertical faces");
    if(w.hsurf_count!=2u)die("solid window self-test: expected sill + lintel planes");
    if(fabs(w.seg[10].a.y-w.seg[10].b.y-1.0)>1e-8 &&
       fabs(w.seg[10].b.y-w.seg[10].a.y-1.0)>1e-8)
        die("solid window self-test: jamb does not span wall thickness");
}

static void add_rect(World *w,int16_t x0,int16_t y0,int16_t x1,int16_t y1,
                     int16_t floor_z,int16_t ceiling_z){
    TSPHostSceneRect *r;
    if(w->scene_rect_count>=MAX_SCENE_RECTS)die("too many room scene rectangles");
    r=&w->scene_rects[w->scene_rect_count++];
    r->x0=x0;r->y0=y0;r->x1=x1;r->y1=y1;
    r->floor_z=floor_z;r->ceiling_z=ceiling_z;
}

/* Derive vertical step/riser faces from adjacent axis-aligned floor regions.
 * Rect bounds are inclusive; neighboring regions therefore meet when one
 * maximum + 1 equals the other's minimum. Only floor-height differences emit
 * geometry. Ceiling differences can later use the same pattern for bulkheads. */
static uint8_t add_risers_from_rects(World *w,uint8_t first,uint8_t count,int8_t bias){
    uint8_t i,j,added=0u;
    if((uint16_t)first+count>w->scene_rect_count)
        die("riser derivation rectangle range invalid");
    for(i=first;i<(uint8_t)(first+count);++i){
        const TSPHostSceneRect *a=&w->scene_rects[i];
        for(j=(uint8_t)(i+1u);j<(uint8_t)(first+count);++j){
            const TSPHostSceneRect *b=&w->scene_rects[j];
            int16_t z0,z1;
            if(a->floor_z==b->floor_z)continue;
            z0=a->floor_z<b->floor_z?a->floor_z:b->floor_z;
            z1=a->floor_z>b->floor_z?a->floor_z:b->floor_z;

            if(a->x1+1==b->x0||b->x1+1==a->x0){
                int16_t x=(a->x1+1==b->x0)?b->x0:a->x0;
                int16_t y0=a->y0>b->y0?a->y0:b->y0;
                int16_t y1=a->y1<b->y1?a->y1:b->y1;
                if(y0<=y1){
                    add_seg(w,x,y0,x,y1,z0,z1,bias);
                    ++added;
                }
            }else if(a->y1+1==b->y0||b->y1+1==a->y0){
                int16_t y=(a->y1+1==b->y0)?b->y0:a->y0;
                int16_t x0=a->x0>b->x0?a->x0:b->x0;
                int16_t x1=a->x1<b->x1?a->x1:b->x1;
                if(x0<=x1){
                    add_seg(w,x0,y,x1,y,z0,z1,bias);
                    ++added;
                }
            }
        }
    }
    return added;
}

static void self_test_derived_riser(void){
    World w;
    memset(&w,0,sizeof(w));
    add_rect(&w,0,0,7,7,0,32);
    add_rect(&w,8,0,15,7,3,35);
    if(add_risers_from_rects(&w,0u,2u,1)!=1u)
        die("derived riser self-test: expected one shared height edge");
    if(w.count!=1u||w.seg[0].z0!=0.0||w.seg[0].z1!=3.0)
        die("derived riser self-test: incorrect vertical range");
}
static void init_world(World *w){
    memset(w,0,sizeof(*w));
    rmb_scene_init(&w->mesh);
}

static int room_mesh_light_occluder(const void *user,
                                    double lx,double ly,double lz,
                                    double wx,double wy,double wz){
    return rmb_segment_occluded((const RMBScene *)user,lx,ly,lz,wx,wy,wz);
}

static void finalize_scene(World *w){
    w->scene.vertices=w->scene_vertices;
    w->scene.vertex_count=w->scene_vertex_count;
    w->scene.segments=w->scene_segments;
    w->scene.segment_count=w->count;
    w->scene.lights=w->scene_lights;
    w->scene.light_count=(uint8_t)(w->lighting_stage>=TSP_HOST_LIGHT_HARD?1u:0u);
    w->scene.rects=w->scene_rects;
    w->scene.rect_count=w->scene_rect_count;
    w->scene.extra_occluder=w->mesh.triangle_count?room_mesh_light_occluder:(TSPHostExtraOccluderFn)0;
    w->scene.extra_occluder_user=w->mesh.triangle_count?(const void *)&w->mesh:(const void *)0;
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

    init_world(w);

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
        add_solid_wall_line(w,94,18,94,38,SOLID_WALL_DEFAULT_THICKNESS,0,32,1);
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
    init_world(w);

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
    add_solid_wall_line(w,94,10,94,38,SOLID_WALL_DEFAULT_THICKNESS,0,32,1);

    add_rect(w,36,-40,116,88,0,32);
    w->lighting_stage=TSP_HOST_LIGHT_BASELINE;
    finalize_scene(w);
}

static void make_stair_world(World *w){
    init_world(w);

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

    /* Horizontal receivers: non-overlapping bands with matched ceiling shift.
     * The vertical risers are derived from these floor discontinuities below. */
    {
        uint8_t floor_first=w->scene_rect_count;
        add_rect(w,36,12,55,36,0,32);
        add_rect(w,56,12,71,36,1,33);
        add_rect(w,72,12,96,36,2,34);
        add_rect(w,56,37,96,51,2,34);
        add_rect(w,56,52,96,63,3,35);
        add_rect(w,56,64,96,80,4,36);
        /* Five shared height discontinuities are implied. The old manual
         * geometry listed only four and omitted the z=1 -> z=2 turn/landing
         * face; deriving from the floor regions closes that hole. */
        if(add_risers_from_rects(w,floor_first,6u,1)!=5u)
            die("stair world did not derive five expected risers");
    }

    w->lighting_stage=TSP_HOST_LIGHT_BASELINE;
    finalize_scene(w);
}

static void make_gallery_world(World *w){
    init_world(w);
    add_two_portal_seams(w,8.0,40.0);

    add_seg(w,36,-48,36,8,0,32,0);
    add_seg(w,36,40,36,96,0,32,0);
    add_seg(w,116,-48,116,8,0,32,0);
    add_seg(w,116,40,116,96,0,32,0);
    add_seg(w,36,-48,116,-48,0,32,0);
    add_seg(w,116,96,36,96,0,32,0);

    /* Sparse architectural fins leave the centre broad and open. */
    /* Wall zero is attached to the north perimeter and carries a real
     * through-window. The baker now derives both thickness faces plus the two
     * vertical window reveals from this one centerline declaration. */
    add_solid_window_line_caps(w,68,-48,68,-18,SOLID_WALL_DEFAULT_THICKNESS,
                               8.0,20.0,10.0,22.0,0,32,1,0u,1u);
    /* Wall one terminates into the south perimeter, so its buried cap is
     * intentionally omitted. */
    add_solid_wall_line_caps(w,90,66,90,96,SOLID_WALL_DEFAULT_THICKNESS,
                             0,32,1,1u,0u);

    add_rect(w,36,-48,116,96,0,32);
    w->lighting_stage=TSP_HOST_LIGHT_BASELINE;
    finalize_scene(w);
}

static void make_turn_world(World *w){
    init_world(w);

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
    add_solid_wall_line(w,80,48,92,48,SOLID_WALL_DEFAULT_THICKNESS,0,32,1);

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
    init_world(w);
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
    init_world(w);
    add_two_portal_seams(w,8.0,40.0);

    add_seg(w,36,-40,36,8,0,32,0);
    add_seg(w,36,40,36,88,0,32,0);
    add_seg(w,116,-40,116,8,0,32,0);
    add_seg(w,116,40,116,88,0,32,0);
    add_seg(w,36,-40,116,-40,0,32,0);
    add_seg(w,116,88,36,88,0,32,0);

    /* Two compact full-height pillars. Each is now authored as ONE centerline
     * plus thickness; the baker derives the four enclosed vertical sides. */
    add_solid_wall_line(w,70,2,70,16,12.0,0,32,1);
    add_solid_wall_line(w,96,44,96,58,12.0,0,32,1);

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

static void add_showcase_shell(World *w){
    add_two_portal_seams(w,8.0,40.0);
    add_seg(w,36,-40,36,8,0,32,0);
    add_seg(w,36,40,36,88,0,32,0);
    add_seg(w,116,-40,116,8,0,32,0);
    add_seg(w,116,40,116,88,0,32,0);
    add_seg(w,36,-40,116,-40,0,32,0);
    add_seg(w,116,88,36,88,0,32,0);
    add_rect(w,36,-40,116,88,0,32);
}


static void add_doomguy_proxy_mesh(RMBScene *m){
#ifdef ROOM_BUNDLE_DOOMGUY_GENERATED
    /* Fixed art-direction scale: 135% of the original 19-unit normalization.
     * Plinth height is NOT part of this scale. */
    RMBTransform t=rmb_transform(78.0,24.0,3.0,0,0,0,1.35,1.35,1.35);
    uint8_t visual=rmb_new_object(m,RMB_OUTLINE_NONE);
    uint8_t lighting=rmb_new_object(m,RMB_OUTLINE_NONE);
    uint8_t shadow=rmb_new_object(m,RMB_OUTLINE_NONE);

    /* The high-detail hero is allowed to affect the picture but deliberately
     * does not participate in light visibility. A separately simplified,
     * invisible proxy owns cast shadows. Both came from the same normalized
     * GLB master, so their silhouettes remain registered. */
    rmb_set_object_flags(m,visual,1u,0u);
    rmb_set_object_shade_levels(m,visual,(uint8_t)ROOM_BUNDLE_DOOMGUY_SHADE_LEVELS);
    rmb_add_indexed_mesh_q8_ex(m,visual,&t,
                               doomguy_visual_xyz_q8,DOOMGUY_VISUAL_VERTEX_COUNT,
                               doomguy_visual_indices,
                               DOOMGUY_VISUAL_TRIANGLE_COUNT,0,
                               doomguy_visual_recess);
#if ROOM_BUNDLE_DOOMGUY_STATIC_LIGHT
    rmb_set_object_incident_weight(m,visual,
                                   (double)ROOM_BUNDLE_DOOMGUY_INCIDENT);
    rmb_set_object_static_light(m,visual,
                                (double)ROOM_BUNDLE_DOOMGUY_AO_RADIUS,
                                (double)ROOM_BUNDLE_DOOMGUY_AO_STRENGTH,
                                (double)ROOM_BUNDLE_DOOMGUY_LIGHT_RADIUS,
                                (double)ROOM_BUNDLE_DOOMGUY_SHADOW_FLOOR);
#endif
#if ROOM_BUNDLE_DOOMGUY_RAMP
    /* Incident angle straight onto the compositor brightness ramp. */
    rmb_set_object_ramp_shading(m,visual,(uint8_t)ROOM_BUNDLE_DOOMGUY_RAMP,
                                (uint8_t)ROOM_BUNDLE_DOOMGUY_SMOOTH);
    rmb_set_object_ramp_equalize(m,visual,1u);
#if ROOM_BUNDLE_DOOMGUY_DITHER
    rmb_set_object_ramp_dither(m,visual,1u);
#endif
#if ROOM_BUNDLE_DOOMGUY_CREASE
    rmb_set_object_crease(m,visual,(double)ROOM_BUNDLE_DOOMGUY_CREASE_COVERAGE,
                          (double)ROOM_BUNDLE_DOOMGUY_CREASE_DEPTH);
#endif
#endif
#if ROOM_BUNDLE_DOOMGUY_CONSOLIDATE
    /* Reduce tile vocabulary in screen space rather than by decimating the
     * mesh, so the lit/unlit boundary stays registered to the real anatomy. */
    rmb_set_object_shade_consolidate(m,visual,
                                     (uint8_t)ROOM_BUNDLE_DOOMGUY_CONSOLIDATE,
                                     (uint8_t)ROOM_BUNDLE_DOOMGUY_CONSOLIDATE_PASSES);
#endif

#if ROOM_BUNDLE_DOOMGUY_LIGHTING_PROXY
    rmb_set_object_flags(m,lighting,1u,0u);
    rmb_set_object_shade_levels(m,lighting,3u);
    rmb_set_object_overlay_target(m,lighting,visual);
    rmb_add_indexed_mesh_q8(m,lighting,&t,
                            doomguy_lighting_xyz_q8,DOOMGUY_LIGHTING_VERTEX_COUNT,
                            doomguy_lighting_indices,DOOMGUY_LIGHTING_TRIANGLE_COUNT,0);
#else
    rmb_set_object_flags(m,lighting,0u,0u);
#endif

    rmb_set_object_flags(m,shadow,0u,1u);
    rmb_add_indexed_mesh_q8(m,shadow,&t,
                            doomguy_shadow_xyz_q8,DOOMGUY_SHADOW_VERTEX_COUNT,
                            doomguy_shadow_indices,DOOMGUY_SHADOW_TRIANGLE_COUNT,0);
#if defined(ROOM_BUNDLE_DOOMGUY_SEAMS) && DOOMGUY_SEAM_LAYER_COUNT > 0
    {
        uint8_t si;
        for(si=0u;si<DOOMGUY_SEAM_LAYER_COUNT;++si){
            const RMBGeneratedSeamLayer *sl=&doomguy_seam_layers[si];
            uint8_t seam;
            if(sl->rank>(uint8_t)ROOM_BUNDLE_DOOMGUY_SEAM_COMPONENTS)continue;
            seam=rmb_new_object(m,RMB_OUTLINE_NONE);
            rmb_set_object_flags(m,seam,1u,0u);
            rmb_set_object_shade_levels(m,seam,3u);
            rmb_set_object_overlay_target(m,seam,visual);
            rmb_set_object_overlay_dither(m,seam,sl->quarters);
            /* -2 forces the existing darkest semantic endpoint regardless of
             * face/light angle; ordered coverage supplies 25/50/75% falloff. */
            rmb_add_indexed_mesh_q8(m,seam,&t,sl->xyz,sl->vertex_count,
                                    sl->idx,sl->triangle_count,-2);
        }
    }
#endif
#else

    RMBTransform t;
    uint8_t body=rmb_new_object(m,RMB_OUTLINE_NONE);
    uint8_t limb=rmb_new_object(m,RMB_OUTLINE_NONE);
    uint8_t gear=rmb_new_object(m,RMB_OUTLINE_NONE);

    /* Proxy bounds intentionally match the incoming real asset after height
     * normalization: about 13.3 x 12.1 x 19 world units above the plinth. */
    t=rmb_transform(78,24,6.5,0,0,-12,1,1,1);
    rmb_add_box(m,body,&t,3.1,2.8,3.5,0);
    t=rmb_transform(78,24,13.0,0,0,-12,1,1,1);
    rmb_add_box(m,body,&t,4.2,3.1,4.0,0);
    t=rmb_transform(78,24,20.0,0,0,-12,1,1,1);
    rmb_add_uv_sphere(m,body,&t,2.5,4u,8u,0);

    t=rmb_transform(76.0,20.5,14.5,58,10,-12,1,1,1);
    rmb_add_cylinder(m,limb,&t,1.2,8.0,6u,0,1u);
    t=rmb_transform(80.0,27.5,14.5,-50,-8,-12,1,1,1);
    rmb_add_cylinder(m,limb,&t,1.2,8.5,6u,0,1u);

    t=rmb_transform(75.3,21.0,7.0,0,0,-12,1,1,1);
    rmb_add_box(m,limb,&t,1.5,1.6,4.0,0);
    t=rmb_transform(80.7,27.0,7.0,0,0,-12,1,1,1);
    rmb_add_box(m,limb,&t,1.5,1.6,4.0,0);

    /* Raised weapon-shaped mass is important for the eventual shadow proxy. */
    t=rmb_transform(82.0,31.0,17.0,-24,8,-18,1,1,1);
    rmb_add_box(m,gear,&t,0.9,0.9,5.8,0);

#endif
}
static void make_doomguy_hero_chamber(World *w){
    RMBTransform t;
    uint8_t plinth;
    init_world(w);

    /* Canonical west/east traversal mouths remain unchanged. */
    add_two_portal_seams(w,8.0,40.0);
    add_seg(w,36,-40,36,8,0,32,0);
    add_seg(w,36,40,36,88,0,32,0);
    add_seg(w,116,-40,116,8,0,32,0);
    add_seg(w,116,40,116,88,0,32,0);

    /* NORTH perimeter remains infinitely thin. The porthole is a true hole:
     * a 12-unit horizontal aperture with lower/upper wall spans only. */
    add_seg(w,36,-40,66,-40,0,32,0);
    add_seg(w,66,-40,78,-40,0,8,0);
    add_seg(w,66,-40,78,-40,22,32,0);
    add_seg(w,78,-40,116,-40,0,32,0);
    add_seg(w,116,88,36,88,0,32,0);
    add_rect(w,36,-40,116,88,0,32);

    /* Four square eight-by-eight solid pillars, close to the room corners. */
    add_solid_wall_line(w,54,-20,54,-12,8.0,0,32,1);
    add_solid_wall_line(w,102,-20,102,-12,8.0,0,32,1);
    add_solid_wall_line(w,54,60,54,68,8.0,0,32,1);
    add_solid_wall_line(w,102,60,102,68,8.0,0,32,1);

    plinth=rmb_new_object(&w->mesh,RMB_OUTLINE_NONE);
    t=rmb_transform(78,24,1.5,0,0,0,1,1,1);
    rmb_add_box(&w->mesh,plinth,&t,10.0,9.0,1.5,0);
    add_doomguy_proxy_mesh(&w->mesh);

    /* Outside light chosen from aperture geometry:
     *  light (62,-96,18) -> porthole x 66..78, z 8..22
     * encloses the 13.3x12.1x19 normalized hero proxy and magnifies its
     * silhouette to nearly room height on the south wall. */
    w->scene_lights[0].x_q4=(int16_t)(62<<4);
    w->scene_lights[0].y_q4=(int16_t)(-96*16);
    w->scene_lights[0].height_q4=(int16_t)(18*16);
    w->scene_lights[0].radius_world=220u;
    w->scene_lights[0].intensity=255u;
    w->lighting_stage=TSP_HOST_LIGHT_HARD;
    finalize_scene(w);
}

static void add_bonsai_generated_mesh(RMBScene *m){
#ifdef ROOM_BUNDLE_BONSAI_GENERATED
    RMBTransform t=rmb_transform(78.0,24.0,0.0,0,0,0,1,1,1);
    uint8_t visual=rmb_new_object(m,RMB_OUTLINE_NONE);
    uint8_t lighting=rmb_new_object(m,RMB_OUTLINE_NONE);
    uint8_t shadow=rmb_new_object(m,RMB_OUTLINE_NONE);

    rmb_set_object_flags(m,visual,1u,0u);
    rmb_set_object_shade_levels(m,visual,1u);
    rmb_add_indexed_mesh_q8(m,visual,&t,
                            bonsai_visual_xyz_q8,BONSAI_VISUAL_VERTEX_COUNT,
                            bonsai_visual_indices,BONSAI_VISUAL_TRIANGLE_COUNT,0);

#if ROOM_BUNDLE_BONSAI_RAMP
    /*
     * Same treatment as the Doomguy hero: incident angle onto the compositor
     * brightness ramp, with self-shadow and cavity occlusion baked per vertex.
     *
     * It matters more here than it did on the statue. The light sits directly
     * overhead, so with flat shading a tree is a single silhouette blob -- the
     * canopy has no top or bottom. Self-shadowing is what separates the lit
     * upper leaf mass from the branches it hangs over, and the occlusion term
     * is what makes the underside read as an underside rather than as more
     * foliage.
     */
    rmb_set_object_ramp_shading(m,visual,(uint8_t)ROOM_BUNDLE_BONSAI_RAMP,
                                (uint8_t)ROOM_BUNDLE_BONSAI_SMOOTH);
    rmb_set_object_ramp_equalize(m,visual,
                                 (uint8_t)ROOM_BUNDLE_BONSAI_EQUALIZE);
#if ROOM_BUNDLE_BONSAI_STATIC_LIGHT
    rmb_set_object_static_light(m,visual,
                                (double)ROOM_BUNDLE_BONSAI_AO_RADIUS,
                                (double)ROOM_BUNDLE_BONSAI_AO_STRENGTH,
                                (double)ROOM_BUNDLE_BONSAI_LIGHT_RADIUS,
                                (double)ROOM_BUNDLE_BONSAI_SHADOW_FLOOR);
#endif
#if ROOM_BUNDLE_BONSAI_CONSOLIDATE
    rmb_set_object_shade_consolidate(m,visual,
                                     (uint8_t)ROOM_BUNDLE_BONSAI_CONSOLIDATE,
                                     (uint8_t)ROOM_BUNDLE_BONSAI_CONSOLIDATE_PASSES);
#endif
#endif

#if ROOM_BUNDLE_BONSAI_LIGHTING_PROXY
    rmb_set_object_flags(m,lighting,1u,0u);
    rmb_set_object_shade_levels(m,lighting,3u);
    rmb_set_object_overlay_target(m,lighting,visual);
    rmb_add_indexed_mesh_q8(m,lighting,&t,
                            bonsai_lighting_xyz_q8,BONSAI_LIGHTING_VERTEX_COUNT,
                            bonsai_lighting_indices,BONSAI_LIGHTING_TRIANGLE_COUNT,0);
#else
    /* Superseded by the ramp path above, exactly as on the hero statue. */
    rmb_set_object_flags(m,lighting,0u,0u);
#endif

    rmb_set_object_flags(m,shadow,0u,1u);
    rmb_add_indexed_mesh_q8(m,shadow,&t,
                            bonsai_shadow_xyz_q8,BONSAI_SHADOW_VERTEX_COUNT,
                            bonsai_shadow_indices,BONSAI_SHADOW_TRIANGLE_COUNT,0);
#if defined(ROOM_BUNDLE_BONSAI_SEAMS) && BONSAI_SEAM_LAYER_COUNT > 0
    {
        uint8_t si;
        for(si=0u;si<BONSAI_SEAM_LAYER_COUNT;++si){
            const RMBGeneratedSeamLayer *sl=&bonsai_seam_layers[si];
            uint8_t seam;
            if(sl->rank>(uint8_t)ROOM_BUNDLE_BONSAI_SEAM_COMPONENTS)continue;
            seam=rmb_new_object(m,RMB_OUTLINE_NONE);
            rmb_set_object_flags(m,seam,1u,0u);
            rmb_set_object_shade_levels(m,seam,3u);
            rmb_set_object_overlay_target(m,seam,visual);
            rmb_set_object_overlay_dither(m,seam,sl->quarters);
            rmb_add_indexed_mesh_q8(m,seam,&t,sl->xyz,sl->vertex_count,
                                    sl->idx,sl->triangle_count,-2);
        }
    }
#endif
#else
    (void)m;
#endif
}

static void make_bonsai_hero_chamber(World *w){
    init_world(w);
    add_showcase_shell(w);
    add_bonsai_generated_mesh(&w->mesh);

    /* Deliberately impossible-scale test: generated asset height is 64 world
     * units, exactly 2x the nominal 32-unit room ceiling. Ceiling is semantic
     * background, not an occluding mesh, so the canopy may extend through it. */
    w->scene_lights[0].x_q4=(int16_t)(78<<4);
    w->scene_lights[0].y_q4=(int16_t)(24<<4);
    w->scene_lights[0].height_q4=(int16_t)(78<<4);
    w->scene_lights[0].radius_world=180u;
    w->scene_lights[0].intensity=255u;
    w->lighting_stage=TSP_HOST_LIGHT_HARD;
    finalize_scene(w);
}

static void make_statue_showcase_world(World *w){
    RMBTransform t;
    uint8_t plinth,torso,head,limbs,sword;
    init_world(w);
    add_showcase_shell(w);

    /* Four old-fashioned solid architectural pillars remain scene segments,
     * so the existing baked point-light pass can cast long room shadows. */
    add_solid_wall_line(w,58,-4,58,4,8.0,0,32,1);
    add_solid_wall_line(w,98,-4,98,4,8.0,0,32,1);
    add_solid_wall_line(w,58,48,58,56,8.0,0,32,1);
    add_solid_wall_line(w,98,48,98,56,8.0,0,32,1);

    plinth=rmb_new_object(&w->mesh,RMB_OUTLINE_NONE);
    torso=rmb_new_object(&w->mesh,RMB_OUTLINE_NONE);
    head=rmb_new_object(&w->mesh,RMB_OUTLINE_NONE);
    limbs=rmb_new_object(&w->mesh,RMB_OUTLINE_NONE);
    sword=rmb_new_object(&w->mesh,RMB_OUTLINE_NONE);

    t=rmb_transform(78,24,3,0,0,8,1,1,1);
    rmb_add_box(&w->mesh,plinth,&t,8.5,7.0,3.0,0);

    t=rmb_transform(78,24,14,0,0,8,1,1,1);
    rmb_add_box(&w->mesh,torso,&t,3.5,5.0,6.0,0);
    t=rmb_transform(78,24,23.5,0,0,8,1,1,1);
    rmb_add_uv_sphere(&w->mesh,head,&t,3.7,4u,8u,0);

    t=rmb_transform(78,20.8,8.5,0,0,8,1,1,1);
    rmb_add_box(&w->mesh,limbs,&t,2.0,1.7,5.5,0);
    t=rmb_transform(78,27.2,8.5,0,0,8,1,1,1);
    rmb_add_box(&w->mesh,limbs,&t,2.0,1.7,5.5,0);

    t=rmb_transform(78,17.6,16.0,58,0,8,1,1,1);
    rmb_add_cylinder(&w->mesh,limbs,&t,1.5,9.0,6u,0,1u);
    t=rmb_transform(78,30.4,16.0,-58,0,8,1,1,1);
    rmb_add_cylinder(&w->mesh,limbs,&t,1.5,9.0,6u,0,1u);

    /* Ridiculous ceremonial sword, because this is a research branch. */
    t=rmb_transform(79.5,33.5,13.5,-18,8,10,1,1,1);
    rmb_add_box(&w->mesh,sword,&t,0.65,0.65,9.5,1);

    w->scene_lights[0].x_q4=(int16_t)(48<<4);
    w->scene_lights[0].y_q4=(int16_t)(70<<4);
    w->scene_lights[0].height_q4=(uint8_t)(11<<4);
    w->scene_lights[0].radius_world=124u;
    w->scene_lights[0].intensity=255u;
    /* Keep dramatic pillar-cast shadows but avoid the one-pixel penumbra
     * pattern explosion on this deliberately busy hero composition. */
    w->lighting_stage=TSP_HOST_LIGHT_HARD;
    finalize_scene(w);
}

static void make_curved_showcase_world(World *w){
    RMBTransform t;
    uint8_t cyl,sphere,platform,dome;
    init_world(w);
    add_showcase_shell(w);

    cyl=rmb_new_object(&w->mesh,RMB_OUTLINE_NONE);
    sphere=rmb_new_object(&w->mesh,RMB_OUTLINE_NONE);
    platform=rmb_new_object(&w->mesh,RMB_OUTLINE_NONE);
    dome=rmb_new_object(&w->mesh,RMB_OUTLINE_NONE);

    /* Intentionally no polygon-edge ink on any curved primitive. */
    t=rmb_transform(60,8,16,0,0,0,1,1,1);
    rmb_add_cylinder(&w->mesh,cyl,&t,7.0,32.0,20u,0,1u);

    t=rmb_transform(80,48,6.5,0,0,0,1,1,1);
    rmb_add_uv_sphere(&w->mesh,sphere,&t,6.5,8u,18u,1);

    t=rmb_transform(98,18,3.0,0,0,0,1,1,1);
    rmb_add_cylinder(&w->mesh,platform,&t,11.0,6.0,24u,0,1u);

    t=rmb_transform(99,56,0.0,0,0,0,1,1,1);
    rmb_add_dome(&w->mesh,dome,&t,8.5,6u,20u,0,1u);

    w->scene_lights[0].x_q4=(int16_t)(74<<4);
    w->scene_lights[0].y_q4=(int16_t)(76<<4);
    w->scene_lights[0].height_q4=(uint8_t)(15<<4);
    w->scene_lights[0].radius_world=120u;
    w->scene_lights[0].intensity=255u;
    w->lighting_stage=TSP_HOST_LIGHT_POINT;
    finalize_scene(w);
}

static void add_table_mesh(RMBScene *m){
    RMBTransform parent,child,t;
    uint8_t obj=rmb_new_object(m,RMB_OUTLINE_NONE);
    static const double lx[4]={-7.0,7.0,-7.0,7.0};
    static const double ly[4]={-4.0,-4.0,4.0,4.0};
    uint8_t i;

    parent=rmb_transform(78,23,9.0,68,17,27,1,1,1);
    child=rmb_transform(0,0,0,0,0,0,1,1,1);
    t=rmb_compose(&parent,&child);
    rmb_add_box(m,obj,&t,9.0,6.0,1.0,0);

    for(i=0u;i<4u;++i){
        child=rmb_transform(lx[i],ly[i],-4.5,0,0,0,1,1,1);
        t=rmb_compose(&parent,&child);
        rmb_add_box(m,obj,&t,0.85,0.85,3.8,0);
    }
}

static void add_shelf_mesh(RMBScene *m){
    RMBTransform parent,child,t;
    uint8_t obj=rmb_new_object(m,RMB_OUTLINE_NONE);
    uint8_t i;
    parent=rmb_transform(97,57,10,0,0,-24,1,1,1);

    child=rmb_transform(0,0,0,0,0,0,1,1,1);
    t=rmb_compose(&parent,&child);
    rmb_add_box(m,obj,&t,0.7,6.5,10.0,0);

    child=rmb_transform(0,6.0,0,0,0,0,1,1,1);
    t=rmb_compose(&parent,&child);
    rmb_add_box(m,obj,&t,4.5,0.7,10.0,0);
    child=rmb_transform(0,-6.0,0,0,0,0,1,1,1);
    t=rmb_compose(&parent,&child);
    rmb_add_box(m,obj,&t,4.5,0.7,10.0,0);

    /* Three shelves are enough to read instantly as a bookcase at GG
     * resolution; the fourth plane was mostly tile-pattern entropy. */
    for(i=0u;i<3u;++i){
        child=rmb_transform(0,0,-7.0+7.0*(double)i,0,0,0,1,1,1);
        t=rmb_compose(&parent,&child);
        rmb_add_box(m,obj,&t,4.5,6.0,0.55,0);
    }
}

static void make_prop_showcase_world(World *w){
    init_world(w);
    add_showcase_shell(w);
    add_table_mesh(&w->mesh);
    add_shelf_mesh(&w->mesh);

    w->scene_lights[0].x_q4=(int16_t)(50<<4);
    w->scene_lights[0].y_q4=(int16_t)(64<<4);
    w->scene_lights[0].height_q4=(uint8_t)(10<<4);
    w->scene_lights[0].radius_world=120u;
    w->scene_lights[0].intensity=255u;
    w->lighting_stage=TSP_HOST_LIGHT_HARD;
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
    else if(bundle==8u)make_statue_showcase_world(w);
    else if(bundle==9u)make_curved_showcase_world(w);
    else if(bundle==10u)make_prop_showcase_world(w);
    else if(bundle==11u)make_doomguy_hero_chamber(w);
    else if(bundle==12u)make_bonsai_hero_chamber(w);
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

static Pose showcase_detail_pose(uint8_t bundle,uint16_t f){
    Pose p;
    double tx=78.0,ty=24.0,tz=16.0,rx=20.0,ry=27.0;
    double q=(double)f/119.0;
    double a=(110.0-220.0*q)*(PI/180.0);
    if(bundle==9u){tx=80.0;ty=30.0;rx=24.0;ry=27.0;}
    else if(bundle==10u){tx=80.0;ty=28.0;tz=14.0;rx=21.0;ry=26.0;}
    else if(bundle==11u){tx=78.0;ty=24.0;tz=15.0;rx=22.0;ry=27.0;}
    else if(bundle==12u){tx=78.0;ty=24.0;tz=16.0;rx=34.0;ry=52.0;}
    /* A deliberately interior ellipse: all review frames remain inside the
     * showcase room instead of occasionally filming the outside of a wall. */
    p.x=tx+rx*cos(a);p.y=ty+ry*sin(a);p.z=tz;
    p.yaw=yaw_from_vec(tx-p.x,ty-p.y);
    return p;
}

/*
 * Two deliberately different passes over a 64-unit tree in a 32-unit room.
 *
 * The camera cannot pitch -- project() puts the horizon on a fixed screen row
 * -- so the only way to fit a tall object is horizontal distance plus eye
 * height. Screen row is 72 - (worldZ - camZ) * 80 / distance, so holding the
 * canopy top (z=64) on screen needs (64 - camZ) * 80 / d <= 72.
 *
 * Pass one stays close and low for the root flare and trunk. Pass two backs
 * off to the room walls and raises the eye to 27, which satisfies that
 * inequality across the wide part of the orbit and puts the whole tree in
 * frame; the very tip still grazes the top edge at the narrow end, which is
 * the room's x extent talking, not the framing.
 */
static Pose bonsai_detail_pose(uint16_t f){
    Pose p;
    const double tx=78.0,ty=24.0;
    if(f<60u){
        double q=(double)f/59.0;
        double a=(125.0-250.0*q)*(PI/180.0);
        p.x=tx+20.0*cos(a);p.y=ty+26.0*sin(a);p.z=15.0;
    }else{
        double q=(double)(f-60u)/59.0;
        double a=(180.0-360.0*q)*(PI/180.0);
        p.x=tx+36.0*cos(a);p.y=ty+52.0*sin(a);p.z=27.0;
    }
    p.yaw=yaw_from_vec(tx-p.x,ty-p.y);
    return p;
}

static Pose window_detail_pose(uint16_t f){
    Pose p;
    double q=(double)f/95.0;
    double deg=50.0-100.0*q;
    double a=deg*(PI/180.0);
    const double tx=68.0,ty=-34.0,r=16.0;
    p.x=tx+r*cos(a);
    p.y=ty+r*sin(a);
    p.z=16.0;
    p.yaw=yaw_from_vec(tx-p.x,ty-p.y);
    return p;
}

static Pose route_pose(uint16_t f,uint8_t bundle){
    Pose p;
    double q,inspect_y=30.0;

    if(bundle==0u)inspect_y=52.0;      /* wide portal-shadow room */
    else if(bundle==1u)inspect_y=46.0; /* clear the inset baffle */
    else if(bundle==4u)inspect_y=54.0; /* broad gallery */
    else if(bundle==6u)inspect_y=44.0; /* stepped room */
    else if(bundle==7u)inspect_y=70.0; /* sweep around both pillars */
    else if(bundle==8u)inspect_y=68.0; /* statue + four shadow pillars */
    else if(bundle==9u)inspect_y=72.0; /* curved primitive gallery */
    else if(bundle==10u)inspect_y=70.0; /* toppled furniture */
    else if(bundle==11u)inspect_y=70.0; /* Doomguy hero chamber */
    else if(bundle==12u)inspect_y=70.0; /* impossible-scale bonsai */

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
    if(bundle==0u||bundle==1u||bundle==4u||bundle==6u||bundle==7u||
       bundle==8u||bundle==9u||bundle==10u||bundle==11u||bundle==12u){
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

static int point_in_hsurf(const HSurf *s,double x,double y){
    uint8_t i,j;
    int inside=0;
    for(i=0u,j=3u;i<4u;j=i++){
        double ax=s->p[j].x,ay=s->p[j].y;
        double bx=s->p[i].x,by=s->p[i].y;
        double dx=bx-ax,dy=by-ay,px=x-ax,py=y-ay;
        double cross=px*dy-py*dx;
        double dot=px*dx+py*dy,len2=dx*dx+dy*dy;
        if(fabs(cross)<1e-8&&dot>=-1e-8&&dot<=len2+1e-8)return 1;
        if(((ay>y)!=(by>y))&&x<(bx-ax)*(y-ay)/(by-ay)+ax)inside=!inside;
    }
    return inside;
}

static void render_horizontal_column(const World *w,const Pose *p,int sx){
    double yaw=(double)p->yaw*(2.0*PI/256.0);
    double fx=cos(yaw),fy=sin(yaw),rx=-fy,ry=fx;
    double lateral=((double)sx+0.5-80.0)/80.0;
    double dx=fx+rx*lateral,dy=fy+ry*lateral;
    int sy;
    for(sy=0;sy<144;++sy){
        double vz=-(((double)sy+0.5)-72.0)/80.0;
        uint8_t hi;
        if(fabs(vz)<1e-12)continue;
        for(hi=0u;hi<w->hsurf_count;++hi){
            const HSurf *s=&w->hsurf[hi];
            double depth=(s->z-p->z)/vz;
            double wx,wy;
            int shade;
            if(depth<=1e-6)continue;
            wx=p->x+dx*depth;
            wy=p->y+dy*depth;
            if(!point_in_hsurf(s,wx,wy))continue;
            shade=depth<=31.0?2:(depth<=55.0?1:0);
            shade+=s->shade_bias;
            if(shade<0)shade=0;
            if(shade>2)shade=2;
            /* 0xfe marks a host-only horizontal receiver. Current point-light
             * pass simply leaves such local caps ambient; geometry is exact. */
            tsp_host_composite_pixel_depth((uint8_t)sx,(uint8_t)sy,0xfeu,
                                           (uint8_t)shade,0u,depth);
        }
    }
}

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
        double dx=cos(ang),dy=sin(ang);
        RenderHit hit[MAX_SEGMENTS];
        uint8_t hit_count=0u,sid,i;

        /*
         * The old PoC kept only the nearest XY segment. That is sufficient for
         * a single floor-to-ceiling wall, but it cannot represent a window:
         * upper and lower wall bands can share the same XY line, and farther
         * geometry must remain visible through the opening.
         *
         * Collect every crossing, sort far -> near, then let the semantic
         * compositor perform ordinary painter overdraw. Near spans replace
         * only their own screen pixels, so gaps genuinely reveal farther
         * geometry without adding a runtime Z buffer.
         */
        for(sid=0u;sid<w->count;++sid){
            double t;
            if(ray_seg(p->x,p->y,dx,dy,&w->seg[sid],&t)){
                uint8_t pos=hit_count;
                if(hit_count>=MAX_SEGMENTS)die("room render hit capacity exceeded");
                while(pos>0u&&hit[pos-1u].t<t){
                    hit[pos]=hit[pos-1u];
                    --pos;
                }
                hit[pos].t=t;
                hit[pos].sid=sid;
                ++hit_count;
            }
        }

        for(i=0u;i<hit_count;++i){
            const Seg *s=&w->seg[hit[i].sid];
            double depth=hit[i].t*cos(rel);
            double top,bottom,inv;
            int it,ib;
            if(depth<0.01)depth=0.01;
            top=72.0-(s->z1-p->z)*80.0/depth;
            bottom=72.0-(s->z0-p->z)*80.0/depth;
            inv=2560.0/depth;if(inv>255.0)inv=255.0;
            it=iround(top);ib=iround(bottom);
            tsp_host_composite_surface_depth((uint8_t)(sx>>3),(uint8_t)sx,(uint8_t)sx,
                                             (int16_t)it,(int16_t)it,
                                             (int16_t)ib,(int16_t)ib,
                                             hit[i].sid,
                                             shade_for_inv(inv,s->shade_bias),
                                             0u,0u,0u,depth);
        }
        render_horizontal_column(w,p,sx);
    }
    if(w->mesh.triangle_count){
        RMBLight ml;
        memset(&ml,0,sizeof(ml));
        if(w->lighting_stage>=TSP_HOST_LIGHT_HARD&&w->scene.light_count){
            ml.x=(double)w->scene_lights[0].x_q4/16.0;
            ml.y=(double)w->scene_lights[0].y_q4/16.0;
            ml.z=(double)w->scene_lights[0].height_q4/16.0;
            ml.enabled=1u;
        }
        rmb_render(&w->mesh,p->x,p->y,p->z,p->yaw,&ml);
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
    uint16_t chosen=0u,budget,max_budget=48u;
    const char *max_env=getenv("ROOM_BUNDLE_SCHEDULER_MAX_UPLOADS");

    if(max_env&&*max_env){
        char *end=(char *)0;
        long v=strtol(max_env,&end,10);
        if(!end||*end||v<1||v>512)
            die("ROOM_BUNDLE_SCHEDULER_MAX_UPLOADS must be 1..512");
        max_budget=(uint16_t)v;
    }

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

    for(budget=1u;budget<=max_budget&&!chosen;++budget){
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
    if(!chosen)die("bundle tile scheduler exceeds configured upload search limit");

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

        /* Review mode emits complete frame sequences for the two geometry
         * stress rooms. CI turns these into downloadable MP4 + PNG proof
         * artifacts without changing the normal room-bundle bake output. */
        if(getenv("ROOM_BUNDLE_CAPTURE_REVIEW") &&
           entry_portal==0u&&exit_portal==1u&&
           (bundle==4u||bundle==7u||bundle==8u||bundle==9u||bundle==10u||bundle==11u||bundle==12u)){
            const char *tag=bundle==4u?"gallery-window":
                            (bundle==7u?"solid-pillars":
                            (bundle==8u?"statue-showcase":
                            (bundle==9u?"curved-showcase":
                            (bundle==10u?"prop-showcase":
                            (bundle==11u?"doomguy-proxy":"bonsai-giant")))));
            snprintf(path,sizeof(path),"%s/review-%s-%03u.ppm",
                     outdir,tag,(unsigned)f);
            if(!tsp_host_composite_write_ppm(path))
                die("review frame write failed");
        }
    }

    if(memcmp(prev,canonical,sizeof(prev))!=0)
        die("route terminal seam name table != canonical seam");

    /* Dedicated visual microscope for the new solid window. This is outside
     * the serialized route and therefore cannot affect runtime bundle data. */
    if(getenv("ROOM_BUNDLE_CAPTURE_REVIEW") &&
       bundle==4u&&entry_portal==0u&&exit_portal==1u){
        uint16_t rf;
        for(rf=0u;rf<96u;++rf){
            Pose rp=window_detail_pose(rf);
            render_pose(w,&rp,cur);
            snprintf(path,sizeof(path),"%s/review-window-detail-%03u.ppm",
                     outdir,(unsigned)rf);
            if(!tsp_host_composite_write_ppm(path))
                die("window detail review frame write failed");
        }
    }

    if(getenv("ROOM_BUNDLE_CAPTURE_REVIEW") &&
       entry_portal==0u&&exit_portal==1u&&bundle>=8u&&bundle<=12u){
        uint16_t rf;
        const char *tag=bundle==8u?"statue-detail":
                        (bundle==9u?"curved-detail":
                        (bundle==10u?"prop-detail":
                        (bundle==11u?"doomguy-detail":"bonsai-detail")));
        for(rf=0u;rf<120u;++rf){
            Pose rp=bundle==12u?bonsai_detail_pose(rf):showcase_detail_pose(bundle,rf);
            render_pose(w,&rp,cur);
            snprintf(path,sizeof(path),"%s/review-%s-%03u.ppm",
                     outdir,tag,(unsigned)rf);
            if(!tsp_host_composite_write_ppm(path))
                die("showcase detail review frame write failed");
            /* Optional owner-masked companion frame for shade diagnostics. */
            if(getenv("ROOM_BUNDLE_CAPTURE_OWNER")){
                uint8_t sid=(uint8_t)(0x80u+
                    (uint8_t)atoi(getenv("ROOM_BUNDLE_CAPTURE_OWNER")));
                snprintf(path,sizeof(path),"%s/owner-%s-%03u.ppm",
                         outdir,tag,(unsigned)rf);
                if(!tsp_host_composite_write_owner_ppm(path,sid))
                    die("owner-masked review frame write failed");
                snprintf(path,sizeof(path),"%s/recess-%s-%03u.ppm",
                         outdir,tag,(unsigned)rf);
                if(!tsp_host_composite_write_recess_ppm(path,sid))
                    die("recess diagnostic frame write failed");
            }
        }
    }

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
    char path[512];
    FILE *pack,*manifest;
    uint16_t canonical[TSP_MAP_CELLS];
    uint64_t canonical_hash=0u;
    uint8_t canonical_ready=0u;
    uint8_t bundle;
    int only_bundle=-1;
    uint8_t output_bundle_count=BUNDLE_COUNT;

    if(argc!=2){fprintf(stderr,"usage: %s OUTPUT_DIR\n",argv[0]);return 2;}
    outdir=argv[1];
    if(getenv("ROOM_BUNDLE_ONLY")){
        only_bundle=atoi(getenv("ROOM_BUNDLE_ONLY"));
        if(only_bundle<0||only_bundle>=(int)BUNDLE_COUNT)
            die("ROOM_BUNDLE_ONLY outside catalog");
        output_bundle_count=1u;
    }

    /* Fail before a multi-thousand-frame bake if the authoring expansion ever
     * stops producing the promised closed vertical wall/window topology. */
    self_test_solid_wall_geometry();
    self_test_derived_riser();
    {
        RMBScene m;
        RMBTransform t=rmb_transform(0,0,0,0,0,37,1,1,1);
        uint8_t o;
        rmb_scene_init(&m);
        o=rmb_new_object(&m,RMB_OUTLINE_SILHOUETTE);
        rmb_add_box(&m,o,&t,1,2,3,0);
        if(m.vertex_count!=8u||m.triangle_count!=12u||m.edge_count!=18u)
            die("mesh authoring self-test failed");
        if(!rmb_segment_occluded(&m,-4,0,0,4,0,0))
            die("mesh shadow self-test: through-box ray should block");
        if(rmb_segment_occluded(&m,-4,6,0,4,6,0))
            die("mesh shadow self-test: outside-box ray should pass");
        rmb_set_object_flags(&m,o,1u,0u);
        if(rmb_segment_occluded(&m,-4,0,0,4,0,0))
            die("mesh flags self-test: non-caster should not block");
        rmb_set_object_flags(&m,o,0u,1u);
        if(!rmb_segment_occluded(&m,-4,0,0,4,0,0))
            die("mesh flags self-test: shadow-only caster should block");
    }
    {
        static const int16_t q8_xyz[12]={
            -256,-256,0, 256,-256,0, 0,256,0, 0,0,512
        };
        static const uint16_t idx[12]={0,2,1,0,1,3,1,2,3,2,0,3};
        RMBScene m;
        RMBTransform t=rmb_transform(0,0,0,0,0,0,1,1,1);
        uint8_t o;
        rmb_scene_init(&m);
        o=rmb_new_object(&m,RMB_OUTLINE_NONE);
        rmb_add_indexed_mesh_q8(&m,o,&t,q8_xyz,4u,idx,4u,0);
        if(m.vertex_count!=4u||m.triangle_count!=4u||m.edge_count!=0u)
            die("indexed mesh ingestion self-test failed");
    }

    snprintf(path,sizeof(path),"%s/room_bundle_poc.pack",outdir);
    pack=fopen(path,"wb");if(!pack)die("cannot create room bundle pack");
    fwrite("RBP2",1,4,pack);
    write_u16(pack,2u);
    fputc(output_bundle_count,pack);
    fputc(0,pack);

    snprintf(path,sizeof(path),"%s/room_bundle_poc_manifest.txt",outdir);
    manifest=fopen(path,"w");if(!manifest)die("cannot create room bundle manifest");
    fprintf(manifest,"Room bundle PoC pack v2 - independently scheduled portal routes\n");

    for(bundle=0u;bundle<BUNDLE_COUNT;++bundle){
        World w;
        if(only_bundle>=0&&bundle!=(uint8_t)only_bundle)continue;
        make_world(bundle,&w);
        /* The GG dispatcher requires dense bundle ids from zero, so a subset
         * pack must renumber. Without this a ROOM_BUNDLE_ONLY pack carries its
         * original catalogue id and room_bundle_pack_to_c.py rejects it, which
         * made subset packs unbuildable into a ROM. */
        fputc((int)(only_bundle>=0?0:bundle),pack);
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
    /*
     * Traversal cost probe. Answers the question a route bake cannot: how much
     * of the screen actually changes for ONE step of a walking player, as
     * opposed to one frame of a fast authored camera. The route moves about
     * 2.75 world units per frame; a player on a 1-unit grid moves far less, and
     * delta cost is driven by how much changed, not by how far the world is.
     */
    if(getenv("ROOM_BUNDLE_STEP_PROBE")){
        static const double steps[]={0.25,0.5,1.0,2.0,4.0,8.0};
        static const uint8_t yaws[]={1u,2u,4u,8u,16u};
        World w;
        Pose base,q;
        uint16_t a_map[TSP_MAP_CELLS],b_map[TSP_MAP_CELLS];
        uint8_t buf[PATCH_MAX];
        uint16_t changed,runs;
        size_t n;
        unsigned i;

        {   int pb=atoi(getenv("ROOM_BUNDLE_STEP_PROBE"));
            if(pb<0||pb>=(int)BUNDLE_COUNT)pb=11;
            make_world((uint8_t)pb,&w);
            fprintf(stderr,"\n[probe bundle %d]\n",pb);
        }
        base.x=78.0;base.y=60.0;base.z=16.0;base.yaw=192u;
        render_pose(&w,&base,a_map);

        fprintf(stderr,"\nSTEP PROBE (bundle 11, one step from x=78 y=60 yaw=192)\n");
        fprintf(stderr,"  translation:\n");
        for(i=0u;i<sizeof(steps)/sizeof(steps[0]);++i){
            q=base;q.y=base.y-steps[i];
            render_pose(&w,&q,b_map);
            n=build_patch(a_map,b_map,buf,&changed,&runs);
            fprintf(stderr,"    %5.2f units forward -> %4u/%u words changed, "
                    "%4u patch bytes\n",steps[i],(unsigned)changed,
                    (unsigned)TSP_MAP_CELLS,(unsigned)n);
        }
        fprintf(stderr,"  rotation:\n");
        for(i=0u;i<sizeof(yaws)/sizeof(yaws[0]);++i){
            q=base;q.yaw=(uint8_t)(base.yaw+yaws[i]);
            render_pose(&w,&q,b_map);
            n=build_patch(a_map,b_map,buf,&changed,&runs);
            fprintf(stderr,"    %2u/256 turn      -> %4u/%u words changed, "
                    "%4u patch bytes\n",(unsigned)yaws[i],(unsigned)changed,
                    (unsigned)TSP_MAP_CELLS,(unsigned)n);
        }
        fprintf(stderr,"\n");
    }

    if(only_bundle<0)fprintf(manifest,"thirteen_module_catalog=PASS\n");
    if(only_bundle==11||only_bundle<0)fprintf(manifest,"doomguy_hero_chamber=PASS\n");
    if(only_bundle==12||only_bundle<0)fprintf(manifest,"bonsai_giant_chamber=PASS\n");
    fprintf(manifest,"mesh_shadow_occlusion=PASS\n");
    fprintf(manifest,"host_mesh_raster=PASS\n");
    fprintf(manifest,"silent_internal_mesh_edges=PASS\n");
    fprintf(manifest,"arbitrary_mesh_transform=PASS\n");
    fprintf(manifest,"solid_interior_wall_expansion=PASS\n");
    fprintf(manifest,"window_vertical_reveal_generation=PASS\n");
    fprintf(manifest,"window_horizontal_reveal_generation=PASS\n");
    fprintf(manifest,"derived_stair_risers=PASS\n");

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

    printf("ROOM_BUNDLE_POC_PASS bundles=%u ordinary_routes=2 split_routes=6 stair_routes=2 frames_per_route=%u canonical=%016llX\n",
           (unsigned)output_bundle_count,ROUTE_FRAMES,(unsigned long long)canonical_hash);
    return 0;
}
