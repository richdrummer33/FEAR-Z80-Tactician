/*
 * Host-side 8x8 semantic compositor + persistent 512-slot tile-cache model.
 *
 * Partial edge coverage is resolved on the PC. The exported name table uses
 * only resident Game Gear tile slots; newly required patterns are reported as
 * explicit 32-byte tile loads for the bake generator to schedule in VBlank.
 */
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "polar_baked_composite.h"
#include "polar_baked_lighting_data.h"

/* Polar explicitly moves the GG pattern name table to 0x3800.
 * That restores Sega's standard maximum background-pattern region:
 *   0x0000..0x37FF = 448 * 32-byte tiles (IDs 0..447)
 *   0x3800..0x3EFF = 32x28 pattern name table
 *   0x3F00..       = sprite attribute table
 *
 * This also matches the live renderer's 423-entry generic tile vocabulary.
 * GBDK crt0 defaults R2 to 0x1800, so every Polar ROM must opt into 0x3800
 * before loading patterns or uploading its name table. */
#define HW_TILES 448u
#define PIXELS 64u
#define FRAME_HASH 1024u

enum {
    SEM_BLACK=0u,
    SEM_CEILING=1u,
    SEM_FLOOR=2u,
    SEM_FAR=3u,
    SEM_MID=4u,
    SEM_NEAR=5u
};

typedef struct FramePattern {
    uint64_t hash;
    uint8_t pix[PIXELS];
    uint16_t slot;
} FramePattern;

static const int8_t k_edge_lut[8][8] = {
    {0,0,0,0,0,0,0,0},{0,0,0,0,1,1,1,1},{0,0,1,1,1,1,2,2},{0,0,1,1,2,2,3,3},
    {0,1,1,2,2,3,3,4},{0,1,1,2,3,4,4,5},{0,1,2,3,3,4,5,6},{0,1,2,3,4,5,6,7}
};

static uint8_t g_cells[TSP_MAP_CELLS][PIXELS];
/* Final visible wall-owner ID for each semantic pixel. Background floor and
 * ceiling remain 0xff. This exists only in the host bake and lets the lighting
 * pass recover an exact world receiver point for wall pixels. */
static uint8_t g_owner[TSP_MAP_CELLS][PIXELS];
/* Binary world-space point-light coverage. Keep illumination separate from
 * ambient semantic colour so palette 1 can implement the common +1 shade
 * transform without creating a new 32-byte pattern for every fully-lit tile. */
static uint8_t g_lit[TSP_MAP_CELLS][PIXELS];
/* Final material veto mask. Keep it broad/cheap by default and clear only
 * authored one-sided wall/profile backfaces. Quantization and penumbra retain
 * their compact reusable vocabulary; this mask is an absolute final clamp. */
static uint8_t g_lightable[TSP_MAP_CELLS][PIXELS];
static uint8_t g_lighting_stage=TSP_HOST_LIGHT_BASELINE;
static int16_t g_camera_x_q4;
static int16_t g_camera_y_q4;
static int16_t g_camera_z_q4=TSP_EYE_HEIGHT_Q4;
static uint8_t g_camera_yaw;
static const TSPHostCompositeScene *g_scene_override;

static uint8_t g_cache_pix[HW_TILES][PIXELS];
static uint64_t g_cache_hash[HW_TILES];
static uint32_t g_cache_last[HW_TILES];
static uint8_t g_cache_valid[HW_TILES];
static uint32_t g_frame_no;
static uint8_t g_ready;

static TSPHostTileLoad g_loads[TSP_HOST_MAX_FRAME_LOADS];
static uint16_t g_load_count;
static uint16_t g_frame_unique;
static uint16_t g_peak_unique;
static uint16_t g_peak_loads;
static uint32_t g_total_loads;

static void die(const char *msg){fprintf(stderr,"fatal: %s\n",msg);exit(2);}

static uint64_t fnv64(const uint8_t *p){
    uint64_t h=UINT64_C(1469598103934665603);
    uint8_t i;
    for(i=0u;i<PIXELS;++i){h^=p[i];h*=UINT64_C(1099511628211);}
    return h;
}
static void tile_fill(uint8_t p[PIXELS],uint8_t c){memset(p,c,PIXELS);}
static void make_horizon(uint8_t p[PIXELS]){
    uint8_t x,y;
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x)
        p[(uint16_t)y*8u+x]=(y==0u)?SEM_BLACK:SEM_FLOOR;
}
static void cache_seed(uint16_t slot,const uint8_t p[PIXELS]){
    memcpy(g_cache_pix[slot],p,PIXELS);
    g_cache_hash[slot]=fnv64(p);
    g_cache_valid[slot]=1u;
    g_cache_last[slot]=0u;
}
static void ensure_init(void){
    uint8_t p[PIXELS];
    if(g_ready)return;
    memset(g_cache_valid,0,sizeof(g_cache_valid));
    tile_fill(p,SEM_CEILING);cache_seed(0u,p);
    tile_fill(p,SEM_FLOOR);cache_seed(1u,p);
    make_horizon(p);cache_seed(2u,p);
    g_frame_no=0u;g_peak_unique=0u;g_peak_loads=0u;g_total_loads=0u;
    g_ready=1u;
}

static uint8_t shade_sem(uint8_t shade){return (uint8_t)(SEM_FAR+(shade>2u?2u:shade));}

void tsp_host_composite_set_lighting(uint8_t stage,const TSPState *camera){
    g_lighting_stage=stage>TSP_HOST_LIGHT_POINT?TSP_HOST_LIGHT_POINT:stage;
    if(camera){
        g_camera_x_q4=camera->x_q4;
        g_camera_y_q4=camera->y_q4;
        g_camera_z_q4=camera->z_q4;
        g_camera_yaw=camera->yaw;
    }
}

void tsp_host_composite_set_scene(const TSPHostCompositeScene *scene){
    g_scene_override=scene;
}

static uint8_t scene_vertex_count(void){
    return g_scene_override?g_scene_override->vertex_count:TSP_HOST_WORLD_VERTEX_COUNT;
}
static uint8_t scene_segment_count(void){
    return g_scene_override?g_scene_override->segment_count:TSP_HOST_WORLD_SEGMENT_COUNT;
}
static uint8_t scene_light_count(void){
    return g_scene_override?g_scene_override->light_count:TSP_HOST_STATIC_LIGHT_COUNT;
}
static int scene_vertex(uint8_t id,TSPHostSceneVertex *out){
    if(!out||id>=scene_vertex_count())return 0;
    if(g_scene_override){
        *out=g_scene_override->vertices[id];
    }else{
        out->x=k_tsp_host_world_vertices[id].x;
        out->y=k_tsp_host_world_vertices[id].y;
        out->x_q4=0;
        out->y_q4=0;
        out->has_exact_q4=0u;
    }
    return 1;
}

static int scene_vertex_world(uint8_t id,double *x,double *y){
    TSPHostSceneVertex v;
    if(!x||!y||!scene_vertex(id,&v))return 0;
    if(v.has_exact_q4){
        *x=(double)v.x_q4/16.0;
        *y=(double)v.y_q4/16.0;
    }else{
        *x=(double)v.x;
        *y=(double)v.y;
    }
    return 1;
}
static int scene_segment(uint8_t id,TSPHostSceneSegment *out){
    if(!out||id>=scene_segment_count())return 0;
    if(g_scene_override){
        *out=g_scene_override->segments[id];
    }else{
        const TSPHostWorldSegment *s=&k_tsp_host_world_segments[id];
        out->v0=s->v0;out->v1=s->v1;out->profile=s->profile;
        out->blocks_light=s->blocks_light;
        out->light_front_sign=s->light_front_sign;
        out->visual_front_sign=s->visual_front_sign;
        out->z0_q4=0;
        out->z1_q4=0;
        out->has_exact_z=0u;
    }
    return 1;
}
static int scene_light(uint8_t id,TSPHostSceneLight *out){
    if(!out||id>=scene_light_count())return 0;
    if(g_scene_override){
        *out=g_scene_override->lights[id];
    }else{
        const TSPHostStaticLight *l=&k_tsp_host_static_lights[id];
        out->x_q4=l->x_q4;out->y_q4=l->y_q4;
        out->height_q4=l->height_q4;
        out->radius_world=l->radius_world;
        out->intensity=l->intensity;
    }
    return 1;
}

static uint8_t ao_pixel(uint8_t color,uint8_t strength,uint8_t sx,uint8_t sy){
    if(!strength||color<=SEM_FAR)return color;
    /* Stable ordered coverage rather than a temporal shimmer.  Stronger
     * authored corners darken more of the sub-tile footprint. */
    if(strength>=2u||(((uint8_t)(sx+sy)&1u)==0u))return (uint8_t)(color-1u);
    return color;
}

#define TSP_CEILING_Z 32.0
#define TSP_ROOM_B_FLOOR_Z 4.0
#define TSP_FOCAL_PX 80.0
#define TSP_HORIZON_PX 72.0

static int ray_segment_params(double ox,double oy,double dx,double dy,
                              double ax,double ay,double bx,double by,
                              double *t_out,double *u_out){
    double sx=bx-ax,sy=by-ay;
    double den=dx*sy-dy*sx;
    double qx,qy,t,u;
    if(fabs(den)<1e-10)return 0;
    qx=ax-ox;qy=ay-oy;
    t=(qx*sy-qy*sx)/den;
    u=(qx*dy-qy*dx)/den;
    if(t_out)*t_out=t;
    if(u_out)*u_out=u;
    return 1;
}

static void profile_z_range(uint8_t profile,double *z0,double *z1){
    switch(profile){
        case TSP_HOST_PROFILE_RAISED:*z0=4.0;*z1=32.0;break;
        case TSP_HOST_PROFILE_LINTEL:*z0=24.0;*z1=32.0;break;
        case TSP_HOST_PROFILE_RISER:*z0=0.0;*z1=4.0;break;
        default:*z0=0.0;*z1=32.0;break;
    }
}

static void segment_z_range(const TSPHostSceneSegment *s,double *z0,double *z1){
    if(s&&s->has_exact_z){
        *z0=(double)s->z0_q4/16.0;
        *z1=(double)s->z1_q4/16.0;
    }else{
        profile_z_range(s?s->profile:TSP_PROFILE_FULL,z0,z1);
    }
}

/* Signed position against the segment's directed right-hand normal (dy,-dx). */
static double segment_right_side(const TSPHostSceneSegment *s,double x,double y){
    double ax,ay,bx,by,dx,dy;
    if(!scene_vertex_world(s->v0,&ax,&ay)||!scene_vertex_world(s->v1,&bx,&by))
        return 0.0;
    dx=bx-ax;
    dy=by-ay;
    return (x-ax)*dy-(y-ay)*dx;
}
static int point_on_signed_front(const TSPHostSceneSegment *s,int8_t sign,double x,double y){
    double q;
    if(!sign)return 1;
    q=segment_right_side(s,x,y);
    return sign>0?q>=-1e-7:q<=1e-7;
}
static int surface_visible_from_camera(uint8_t sid){
    TSPHostSceneSegment s;
    double cx,cy;
    if(!scene_segment(sid,&s))return 1;
    if(!s.visual_front_sign)return 1;
    cx=(double)g_camera_x_q4/16.0;cy=(double)g_camera_y_q4/16.0;
    return point_on_signed_front(&s,s.visual_front_sign,cx,cy);
}
static int receiver_accepts_light(uint8_t sid){
    TSPHostSceneSegment s;
    TSPHostSceneLight light;
    double cx,cy,lx,ly;
    if(!scene_segment(sid,&s)||!scene_light(0u,&light))return 1;
    if(!s.light_front_sign)return 1;
    cx=(double)g_camera_x_q4/16.0;cy=(double)g_camera_y_q4/16.0;
    lx=(double)light.x_q4/16.0;ly=(double)light.y_q4/16.0;
    return point_on_signed_front(&s,s.light_front_sign,cx,cy) &&
           point_on_signed_front(&s,s.light_front_sign,lx,ly);
}

/*
 * 2.5D ray test: XY finds the crossing with each wall/profile segment, then
 * the same parametric t gives the ray's Z at that crossing. A segment blocks
 * only if z_hit lies inside its profile-derived vertical interval.
 */
static int world_point_lit(double wx,double wy,double wz,int receiver_sid){
    TSPHostSceneLight light;
    double lx,ly,lz,dx,dy;
    uint8_t sid;
    if(!scene_light(0u,&light))return 0;
    lx=(double)light.x_q4/16.0;ly=(double)light.y_q4/16.0;
    lz=(double)light.height_q4/16.0;
    dx=wx-lx;dy=wy-ly;
    if(dx*dx+dy*dy<1e-10)return 1;
    for(sid=0u;sid<scene_segment_count();++sid){
        TSPHostSceneSegment s;
        double ax,ay,bx,by,t,u,z0,z1,zhit;
        if(!scene_segment(sid,&s)||!s.blocks_light||(int)sid==receiver_sid)continue;
        if(!scene_vertex_world(s.v0,&ax,&ay)||!scene_vertex_world(s.v1,&bx,&by))
            continue;
        if(!ray_segment_params(lx,ly,dx,dy,ax,ay,bx,by,&t,&u))continue;
        if(t<=1e-7||t>=1.0-1e-7||u<-1e-7||u>1.0+1e-7)continue;
        zhit=lz+t*(wz-lz);
        segment_z_range(&s,&z0,&z1);
        if(zhit>=z0-1e-7&&zhit<=z1+1e-7)return 0;
    }
    return 1;
}

static void camera_basis(double *fx,double *fy,double *rx,double *ry){
    const double pi=3.14159265358979323846;
    double yaw=(double)g_camera_yaw*(2.0*pi/256.0);
    *fx=cos(yaw);*fy=sin(yaw);
    *rx=-*fy;*ry=*fx;
}

static int point_on_world_edge(double x,double y,
                               const TSPHostWorldVertex *a,
                               const TSPHostWorldVertex *b){
    double dx=(double)b->x-(double)a->x,dy=(double)b->y-(double)a->y;
    double px=x-(double)a->x,py=y-(double)a->y;
    double cross=px*dy-py*dx;
    double dot=px*dx+py*dy,len2=dx*dx+dy*dy;
    return fabs(cross)<1e-7&&dot>=-1e-7&&dot<=len2+1e-7;
}
static int point_in_world_poly(double x,double y,const uint8_t *vid,uint8_t n){
    uint8_t i,j;int inside=0;
    for(i=0u,j=(uint8_t)(n-1u);i<n;j=i++){
        const TSPHostWorldVertex *a=&k_tsp_host_world_vertices[vid[j]];
        const TSPHostWorldVertex *b=&k_tsp_host_world_vertices[vid[i]];
        double ay=(double)a->y,by=(double)b->y;
        if(point_on_world_edge(x,y,a,b))return 1;
        if(((ay>y)!=(by>y))&&
           x<((double)(b->x-a->x)*(y-ay)/(by-ay)+(double)a->x))
            inside=!inside;
    }
    return inside;
}
static int point_in_room_a(double x,double y){
    static const uint8_t p[]={0u,1u,2u,3u,4u,5u};
    return point_in_world_poly(x,y,p,(uint8_t)(sizeof(p)/sizeof(p[0])));
}
static int point_in_connector(double x,double y){
    static const uint8_t p[]={2u,6u,7u,3u};
    return point_in_world_poly(x,y,p,(uint8_t)(sizeof(p)/sizeof(p[0])));
}
static int point_in_room_b(double x,double y){
    static const uint8_t p[]={8u,10u,11u,12u,13u,9u,7u,6u};
    return point_in_world_poly(x,y,p,(uint8_t)(sizeof(p)/sizeof(p[0])));
}
static int point_in_any_horizontal(double x,double y){
    return point_in_room_a(x,y)||point_in_connector(x,y)||point_in_room_b(x,y);
}

/* Intersect the camera ray through one screen pixel with a horizontal plane. */
static double camera_z_world(void){return (double)g_camera_z_q4/16.0;}

static int screen_plane_world(int sx,int sy,double zplane,
                              double *wx,double *wy,double *depth_out){
    double px=(double)sx+0.5,py=(double)sy+0.5;
    double vz=-(py-TSP_HORIZON_PX)/TSP_FOCAL_PX;
    double depth,lateral,fx,fy,rx,ry;
    double cx=(double)g_camera_x_q4/16.0,cy=(double)g_camera_y_q4/16.0;
    if(fabs(vz)<1e-10)return 0;
    depth=(zplane-camera_z_world())/vz;
    if(depth<=1e-6)return 0;
    lateral=depth*((px-80.0)/TSP_FOCAL_PX);
    camera_basis(&fx,&fy,&rx,&ry);
    *wx=cx+fx*depth+rx*lateral;
    *wy=cy+fy*depth+ry*lateral;
    if(depth_out)*depth_out=depth;
    return 1;
}

/*
 * Background receiver selection now respects the virtual raised Room-B floor.
 * Room A + connector use z=0; Room B uses z=4. Because a screen ray can in
 * principle hit more than one candidate plane footprint, choose the nearest
 * valid authored receiver.
 */
static int background_world_receiver(int sx,int sy,double *wx,double *wy,double *wz){
    double x,y,d,best=1e30,bx=0.0,by=0.0,bz=0.0;
    if(g_scene_override&&g_scene_override->rects&&g_scene_override->rect_count){
        uint8_t i;
        if(sy==72)return 0;
        for(i=0u;i<g_scene_override->rect_count;++i){
            const TSPHostSceneRect *r=&g_scene_override->rects[i];
            double plane=(double)(sy<72?r->ceiling_z:r->floor_z);
            if(screen_plane_world(sx,sy,plane,&x,&y,&d)&&
               x>=(double)r->x0-1e-7&&x<=(double)r->x1+1e-7&&
               y>=(double)r->y0-1e-7&&y<=(double)r->y1+1e-7&&d<best){
                best=d;bx=x;by=y;bz=plane;
            }
        }
        if(best>=1e29)return 0;
        *wx=bx;*wy=by;*wz=bz;return 1;
    }
    if(sy<72){
        if(screen_plane_world(sx,sy,TSP_CEILING_Z,&x,&y,&d)&&point_in_any_horizontal(x,y)){
            *wx=x;*wy=y;*wz=TSP_CEILING_Z;return 1;
        }
        return 0;
    }
    if(sy<=72)return 0;

    if(screen_plane_world(sx,sy,TSP_ROOM_B_FLOOR_Z,&x,&y,&d)&&point_in_room_b(x,y)){
        best=d;bx=x;by=y;bz=TSP_ROOM_B_FLOOR_Z;
    }
    if(screen_plane_world(sx,sy,0.0,&x,&y,&d)&&
       (point_in_room_a(x,y)||point_in_connector(x,y))&&d<best){
        best=d;bx=x;by=y;bz=0.0;
    }
    if(best>=1e29)return 0;
    *wx=bx;*wy=by;*wz=bz;return 1;
}

/* Recover full XYZ of a visible wall/profile pixel. */
static int wall_world_point(uint8_t sid,int sx,int sy,double *wx,double *wy,double *wz){
    TSPHostSceneSegment s;
    double ax,ay,bx,by,fx,fy,rx,ry,lateral,dx,dy,t,u,z0,z1;
    double cx=(double)g_camera_x_q4/16.0,cy=(double)g_camera_y_q4/16.0;
    double py=(double)sy+0.5;
    if(!scene_segment(sid,&s)||
       !scene_vertex_world(s.v0,&ax,&ay)||!scene_vertex_world(s.v1,&bx,&by))
        return 0;
    camera_basis(&fx,&fy,&rx,&ry);
    lateral=(((double)sx+0.5)-80.0)/TSP_FOCAL_PX;
    dx=fx+rx*lateral;dy=fy+ry*lateral;
    if(!ray_segment_params(cx,cy,dx,dy,ax,ay,bx,by,&t,&u))return 0;
    if(t<=1e-7||u<-1e-5||u>1.0+1e-5)return 0;
    *wx=cx+dx*t;*wy=cy+dy*t;
    *wz=camera_z_world()-((py-TSP_HORIZON_PX)/TSP_FOCAL_PX)*t;
    segment_z_range(&s,&z0,&z1);
    if(*wz<z0)*wz=z0;
    if(*wz>z1)*wz=z1;
    return 1;
}

static uint8_t lit_semantic(uint8_t v){
    /* First geometric test deliberately has only two states: ambient and
     * one shade brighter. No radius, attenuation band or penumbra yet. */
    if(v==SEM_BLACK||v>=SEM_NEAR)return v;
    return (uint8_t)(v+1u);
}

/*
 * Snap a mixed 8x8 light/shadow classification to the renderer's reusable
 * straight-edge vocabulary. The exact world visibility solution is sampled
 * first; this is only the final sub-tile rasterization step.
 *
 * k_edge_lut supplies shallow 0..1 slopes. Mirroring the independent axis
 * gives negative slopes, and transposing X/Y supplies the steep family. We
 * search both lit sides and enough offsets to slide the line completely
 * through the tile. Full-lit/full-shadow cells are left exact and therefore
 * continue to collapse to the ambient tile pattern via palette 1.
 */
static void quantize_light_cell(uint16_t cell){
    uint64_t eligible=0u,target=0u,best=0u;
    unsigned eligible_count,target_count,best_cost=65u;
    int orient,si,mirror,side,off,x,y;
    uint8_t i;

    for(i=0u;i<PIXELS;++i){
        uint8_t v=g_cells[cell][i];
        uint64_t bit=UINT64_C(1)<<i;
        if(v>SEM_BLACK&&v<SEM_NEAR){
            eligible|=bit;
            if(g_lit[cell][i])target|=bit;
        }
    }
    eligible_count=(unsigned)__builtin_popcountll(eligible);
    target_count=(unsigned)__builtin_popcountll(target);
    if(!eligible_count||!target_count||target_count==eligible_count)return;

    for(orient=0;orient<2;++orient)
    for(si=0;si<8;++si)
    for(mirror=0;mirror<2;++mirror)
    for(side=0;side<2;++side)
    for(off=-8;off<=15;++off){
        uint64_t cand=0u;
        unsigned cand_count,cost;
        for(y=0;y<8;++y)for(x=0;x<8;++x){
            int a=orient?y:x;
            int b=orient?x:y;
            int sample=mirror?(7-a):a;
            int line=off+(int)k_edge_lut[si][sample];
            int lit=side?(b>=line):(b<line);
            if(lit)cand|=UINT64_C(1)<<((unsigned)y*8u+(unsigned)x);
        }
        cand_count=(unsigned)__builtin_popcountll(cand&eligible);
        /* Preserve the fact that this was a boundary cell; do not erase a
         * narrow cast edge merely because all-on/all-off is one pixel closer. */
        if(!cand_count||cand_count==eligible_count)continue;
        cost=(unsigned)__builtin_popcountll((cand^target)&eligible);
        if(cost<best_cost){best_cost=cost;best=cand;if(!cost)goto found_exact;}
    }
found_exact:
    if(best_cost>64u)return;
    for(i=0u;i<PIXELS;++i){
        uint8_t v=g_cells[cell][i];
        if(v>SEM_BLACK&&v<SEM_NEAR)
            g_lit[cell][i]=(uint8_t)((best>>i)&UINT64_C(1));
    }
}

static void quantize_point_light_edges(void){
    uint16_t cell;
    for(cell=0u;cell<TSP_MAP_CELLS;++cell)quantize_light_cell(cell);
}

/* Quantization approximates only the SHAPE of a hard shadow. Material
 * sidedness is non-negotiable: clear any approximated light pixel that the
 * exact receiver test marked as illegal. Keeping this as a post-clamp avoids
 * fragmenting the reusable straight-edge vocabulary. */
static void enforce_lightable_mask(void){
    uint16_t cell;uint8_t i;
    for(cell=0u;cell<TSP_MAP_CELLS;++cell)
        for(i=0u;i<PIXELS;++i)
            if(!g_lightable[cell][i])g_lit[cell][i]=0u;
}

static void apply_one_sided_penumbra(void){
    uint8_t hard[TSP_MAP_CELLS][PIXELS];
    uint16_t cell;
    memcpy(hard,g_lit,sizeof(hard));

    /*
     * Cartridge-aware soft edge: only feather cells that were ALREADY mixed
     * by the quantized hard boundary. This prevents the 1px penumbra from
     * turning an adjacent all-shadow tile into a brand-new dynamic pattern.
     * The hard boundary remains authoritative; softness is one-sided outward
     * into shadow and uses stable ordered 50% coverage.
     */
    for(cell=0u;cell<TSP_MAP_CELLS;++cell){
        uint8_t eligible=0u,lit=0u,i;
        int x,y,k;
        for(i=0u;i<PIXELS;++i){
            uint8_t v=g_cells[cell][i];
            if(v>SEM_BLACK&&v<SEM_NEAR){
                ++eligible;
                if(hard[cell][i])++lit;
            }
        }
        if(!eligible||!lit||lit==eligible)continue;

        for(y=0;y<8;++y)for(x=0;x<8;++x){
            uint16_t pi=(uint16_t)y*8u+(uint16_t)x;
            uint8_t v=g_cells[cell][pi],touch=0u;
            if(hard[cell][pi]||v==SEM_BLACK||v>=SEM_NEAR)continue;

            for(k=0;k<8;++k){
                static const int8_t nx[8]={-1,0,1,-1,1,-1,0,1};
                static const int8_t ny[8]={-1,-1,-1,0,0,1,1,1};
                int xx=x+nx[k],yy=y+ny[k];
                uint16_t np;
                if(xx<0||xx>=8||yy<0||yy>=8)continue;
                np=(uint16_t)yy*8u+(uint16_t)xx;
                /* Owner must match so softness never leaks across a surface
                 * silhouette within a tile. Background owner 0xff naturally
                 * remains background-only. */
                if(g_owner[cell][np]!=g_owner[cell][pi])continue;
                if(hard[cell][np]){touch=1u;break;}
            }
            if(touch&&((((cell%TSP_COLS)*8u+x)+
                         ((cell/TSP_COLS)*8u+y))&1u)==0u)
                g_lit[cell][pi]=1u;
        }
    }
}

static void apply_point_light(void){
    int x,y;
    if(!scene_light_count())return;
    for(y=0;y<144;++y)for(x=0;x<160;++x){
        uint16_t cell=(uint16_t)((y>>3)*TSP_COLS+(x>>3));
        uint16_t pi=(uint16_t)(y&7)*8u+(uint16_t)(x&7);
        uint8_t v=g_cells[cell][pi],owner=g_owner[cell][pi];
        double wx,wy,wz;
        int ok=0,receiver=-1;
        if(v==SEM_BLACK)continue;
        if(owner!=0xffu){
            receiver=(int)owner;
            if(receiver_accepts_light(owner))
                ok=wall_world_point(owner,x,y,&wx,&wy,&wz);
            else
                g_lightable[cell][pi]=0u;
        }else if((y<72&&v==SEM_CEILING)||(y>72&&v==SEM_FLOOR)){
            ok=background_world_receiver(x,y,&wx,&wy,&wz);
        }
        if(ok&&v>SEM_BLACK&&v<SEM_NEAR&&world_point_lit(wx,wy,wz,receiver))
            g_lit[cell][pi]=1u;
    }
    quantize_point_light_edges();
    if(g_lighting_stage>=TSP_HOST_LIGHT_POINT)apply_one_sided_penumbra();
    enforce_lightable_mask();
}

static void generic_unflipped_indices(uint16_t id,uint8_t out[PIXELS]){
    uint8_t x,y;
    if(id==TSP_TILE_CEILING){tile_fill(out,1u);return;}
    if(id==TSP_TILE_FLOOR){tile_fill(out,2u);return;}
    if(id==TSP_TILE_HORIZON){make_horizon(out);return;}

    if(id>=TSP_TILE_FULL_BASE && id<TSP_TILE_EDGE_BASE){
        uint16_t rel=(uint16_t)(id-TSP_TILE_FULL_BASE);
        uint8_t border=(uint8_t)(rel%TSP_BORDER_COUNT);
        uint8_t q=(uint8_t)(rel/TSP_BORDER_COUNT);
        uint8_t cap=(uint8_t)(q%TSP_CAP_COUNT);
        uint8_t shade=(uint8_t)(q/TSP_CAP_COUNT);
        uint8_t color=shade_sem(shade);
        for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){
            uint8_t black=(uint8_t)(((border&1u)&&x==0u)||((border&2u)&&x==7u));
            if(cap==TSP_CAP_TOP&&y==0u)black=1u;
            if(cap==TSP_CAP_BOTTOM&&y==7u)black=1u;
            out[(uint16_t)y*8u+x]=black?SEM_BLACK:color;
        }
        return;
    }

    if(id>=TSP_TILE_EDGE_BASE && id<TSP_GENERATED_TILE_COUNT){
        uint16_t rel=(uint16_t)(id-TSP_TILE_EDGE_BASE);
        uint8_t si=(uint8_t)(rel%TSP_EDGE_SLOPE_COUNT);
        uint8_t q=(uint8_t)(rel/TSP_EDGE_SLOPE_COUNT);
        uint8_t oi=(uint8_t)(q%TSP_EDGE_OFF_COUNT);
        uint8_t shade=(uint8_t)(q/TSP_EDGE_OFF_COUNT);
        int8_t off=(int8_t)TSP_EDGE_OFF_MIN+(int8_t)oi;
        uint8_t color=shade_sem(shade);
        for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){
            int8_t line=(int8_t)(off+k_edge_lut[si][x]);
            out[(uint16_t)y*8u+x]=(int8_t)y<line?1u:((int8_t)y==line?0u:color);
        }
        return;
    }
    die("renderer emitted invalid generic tile");
}

static void decode_word(uint16_t word,uint8_t sem[PIXELS],uint8_t mask[PIXELS]){
    uint16_t id=(uint16_t)(word&TSP_TILE_ID_MASK);
    uint8_t raw[PIXELS];
    uint8_t x,y,fx=(uint8_t)((word&TSP_ATTR_FLIPX)!=0u);
    uint8_t fy=(uint8_t)((word&TSP_ATTR_FLIPY)!=0u);
    uint8_t pal=(uint8_t)((word&TSP_ATTR_PALETTE)!=0u);
    uint8_t is_edge=(uint8_t)(id>=TSP_TILE_EDGE_BASE&&id<TSP_GENERATED_TILE_COUNT);
    generic_unflipped_indices(id,raw);
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){
        uint8_t sx=fx?(uint8_t)(7u-x):x;
        uint8_t sy=fy?(uint8_t)(7u-y):y;
        uint8_t v=raw[(uint16_t)sy*8u+sx];
        uint16_t i=(uint16_t)y*8u+x;
        mask[i]=(uint8_t)(!is_edge||v!=1u);
        sem[i]=(uint8_t)((pal&&v==1u)?SEM_FLOOR:v);
    }
}

void tsp_host_composite_reset_cache(void){
    g_ready=0u;
    ensure_init();
}

void tsp_host_composite_begin_frame(void){
    uint8_t row,col;
    ensure_init();
    memset(g_owner,0xff,sizeof(g_owner));
    memset(g_lit,0,sizeof(g_lit));
    memset(g_lightable,1,sizeof(g_lightable));
    for(row=0u;row<TSP_ROWS;++row)for(col=0u;col<TSP_COLS;++col){
        uint8_t *p=g_cells[(uint16_t)row*TSP_COLS+col];
        if(row<9u)tile_fill(p,SEM_CEILING);
        else if(row==9u)make_horizon(p);
        else tile_fill(p,SEM_FLOOR);
    }
}
void tsp_host_composite_write(uint8_t row,uint8_t col,uint16_t word){
    uint8_t sem[PIXELS],mask[PIXELS],i;
    uint8_t *dst;
    if(row>=TSP_ROWS||col>=TSP_COLS)die("composite cell out of range");
    decode_word(word,sem,mask);
    dst=g_cells[(uint16_t)row*TSP_COLS+col];
    for(i=0u;i<PIXELS;++i)if(mask[i])dst[i]=sem[i];
}

static int16_t lerp_edge7(int16_t a,int16_t b,uint8_t x){
    int16_t d=(int16_t)(b-a),n=(int16_t)(d*(int16_t)x);
    int16_t q=n>=0?(int16_t)((n+3)/7):(int16_t)-(((-n)+3)/7);
    return (int16_t)(a+q);
}

void tsp_host_composite_surface(uint8_t col,uint8_t clip_x0,uint8_t clip_x1,
                                int16_t tl,int16_t tr,int16_t bl,int16_t br,
                                uint8_t sid,uint8_t shade,uint8_t border,
                                uint8_t ao_left,uint8_t ao_right){
    uint8_t sx,color=shade_sem(shade);
    uint16_t coarse_x=(uint16_t)col*8u;
    if(col>=TSP_COLS||clip_x0>clip_x1||clip_x1>159u)die("surface raster bounds invalid");
    if(!surface_visible_from_camera(sid))return;

    for(sx=clip_x0;sx<=clip_x1;++sx){
        uint8_t local=(uint8_t)((uint16_t)sx-coarse_x);
        int16_t top=lerp_edge7(tl,tr,local);
        int16_t bot=lerp_edge7(bl,br,local);
        int16_t y0=top<0?0:top;
        int16_t y1=bot>143?143:bot;
        int16_t y;

        if(y0>y1)continue;
        for(y=y0;y<=y1;++y){
            uint8_t row=(uint8_t)((uint16_t)y>>3);
            uint8_t py=(uint8_t)((uint16_t)y&7u);
            uint8_t px=(uint8_t)((uint16_t)sx&7u);
            uint16_t cell=(uint16_t)row*TSP_COLS+col;
            uint16_t pi=(uint16_t)py*8u+px;
            uint8_t *dst=g_cells[cell];
            uint8_t *own=g_owner[cell];
            uint8_t black=(uint8_t)(y==top||y==bot),pixel=color;
            if((border&1u)&&sx==clip_x0)black=1u;
            if((border&2u)&&sx==clip_x1)black=1u;
            if(!black&&g_lighting_stage>=TSP_HOST_LIGHT_AO){
                uint8_t strength=0u;
                if(ao_left){
                    uint8_t d=(uint8_t)(sx-clip_x0);
                    if(d<=ao_left){uint8_t q=(uint8_t)(ao_left+1u-d);if(q>strength)strength=q;}
                }
                if(ao_right){
                    uint8_t d=(uint8_t)(clip_x1-sx);
                    if(d<=ao_right){uint8_t q=(uint8_t)(ao_right+1u-d);if(q>strength)strength=q;}
                }
                pixel=ao_pixel(pixel,strength,sx,(uint8_t)y);
            }
            dst[pi]=black?SEM_BLACK:pixel;
            own[pi]=sid;
        }
    }
}

static void flip_pattern(const uint8_t src[PIXELS],uint8_t dst[PIXELS],uint8_t fx,uint8_t fy){
    uint8_t x,y;
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){
        uint8_t sx=fx?(uint8_t)(7u-x):x;
        uint8_t sy=fy?(uint8_t)(7u-y):y;
        dst[(uint16_t)y*8u+x]=src[(uint16_t)sy*8u+sx];
    }
}
static void canonicalize(const uint8_t orig[PIXELS],uint8_t canon[PIXELS],uint16_t *attr){
    uint8_t tmp[PIXELS],best[PIXELS],k,bestk=0u;
    flip_pattern(orig,best,0u,0u);
    for(k=1u;k<4u;++k){
        flip_pattern(orig,tmp,(uint8_t)(k&1u),(uint8_t)((k>>1)&1u));
        if(memcmp(tmp,best,PIXELS)<0){memcpy(best,tmp,PIXELS);bestk=k;}
    }
    memcpy(canon,best,PIXELS);
    *attr=(uint16_t)(((bestk&1u)?TSP_ATTR_FLIPX:0u)|((bestk&2u)?TSP_ATTR_FLIPY:0u));
}
static void encode_4bpp(const uint8_t sem[PIXELS],uint8_t out[TSP_HOST_TILE_BYTES]){
    uint8_t x,y,p;
    memset(out,0,TSP_HOST_TILE_BYTES);
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){
        uint8_t c=sem[(uint16_t)y*8u+x];
        uint8_t bit=(uint8_t)(0x80u>>x);
        for(p=0u;p<4u;++p)if(c&(uint8_t)(1u<<p))
            out[(uint16_t)y*4u+p]|=bit;
    }
}
static int cache_find(uint64_t h,const uint8_t p[PIXELS]){
    uint16_t i;
    for(i=0u;i<HW_TILES;++i)
        if(g_cache_valid[i]&&g_cache_hash[i]==h&&memcmp(g_cache_pix[i],p,PIXELS)==0)return (int)i;
    return -1;
}

/*
 * Palette 1 is a hardware +1-shade transform for indices 1..4.  A tile that
 * contains any light uses palette 1. Lit pixels keep their ordinary semantic
 * index; unlit pixels in the same mixed boundary tile use reserved indices
 * 8..11, whose palette-1 colours duplicate the original ambient shades.
 *
 * Result: a completely-lit tile has byte-for-byte the SAME pattern as its
 * ambient form and only toggles one name-table palette bit. Only tiles actually
 * crossed by a hard light/shadow boundary require a distinct mixed pattern.
 */
static uint8_t point_tile_encode(const uint8_t ambient[PIXELS],
                                 const uint8_t lit[PIXELS],
                                 uint8_t encoded[PIXELS]){
    uint8_t i,any=0u;
    for(i=0u;i<PIXELS;++i){
        uint8_t v=ambient[i];
        if(lit[i]&&v>SEM_BLACK&&v<SEM_NEAR){any=1u;break;}
    }
    if(!any){memcpy(encoded,ambient,PIXELS);return 0u;}
    for(i=0u;i<PIXELS;++i){
        uint8_t v=ambient[i];
        if(v==SEM_BLACK||v>=SEM_NEAR)encoded[i]=v;
        else if(lit[i])encoded[i]=v;
        else encoded[i]=(uint8_t)(v+7u);
    }
    return 1u;
}

void tsp_host_composite_export(uint16_t out[TSP_MAP_CELLS]){
    FramePattern req[TSP_MAP_CELLS];
    int16_t htab[FRAME_HASH];
    uint16_t cell_req[TSP_MAP_CELLS],cell_attr[TSP_MAP_CELLS];
    uint8_t needed[HW_TILES];
    uint16_t req_count=0u,i;
    ensure_init();
    if(g_lighting_stage>=TSP_HOST_LIGHT_HARD&&scene_light_count())
        apply_point_light();
    ++g_frame_no;
    memset(htab,0xff,sizeof(htab));
    memset(needed,0,sizeof(needed));
    g_load_count=0u;

    for(i=0u;i<TSP_MAP_CELLS;++i){
        uint8_t canon[PIXELS],encoded[PIXELS],use_lit_palette=0u;
        uint16_t attr,pos;
        uint64_t h;
        if(g_lighting_stage>=TSP_HOST_LIGHT_HARD)
            use_lit_palette=point_tile_encode(g_cells[i],g_lit[i],encoded);
        else memcpy(encoded,g_cells[i],PIXELS);
        canonicalize(encoded,canon,&attr);
        if(use_lit_palette)attr|=TSP_ATTR_PALETTE;
        h=fnv64(canon);pos=(uint16_t)h&(FRAME_HASH-1u);
        for(;;){
            int16_t q=htab[pos];
            if(q<0){
                if(req_count>=TSP_MAP_CELLS)die("frame pattern capacity exceeded");
                req[req_count].hash=h;memcpy(req[req_count].pix,canon,PIXELS);req[req_count].slot=0xffffu;
                htab[pos]=(int16_t)req_count;cell_req[i]=req_count++;break;
            }
            if(req[(uint16_t)q].hash==h&&memcmp(req[(uint16_t)q].pix,canon,PIXELS)==0){
                cell_req[i]=(uint16_t)q;break;
            }
            pos=(uint16_t)((pos+1u)&(FRAME_HASH-1u));
        }
        cell_attr[i]=attr;
    }

    g_frame_unique=req_count;
    if(req_count>g_peak_unique)g_peak_unique=req_count;
    if(req_count>HW_TILES)die("single frame needs more than 512 unique tile patterns");

    /* First retain every pattern already resident. */
    for(i=0u;i<req_count;++i){
        int s=cache_find(req[i].hash,req[i].pix);
        if(s>=0){req[i].slot=(uint16_t)s;needed[(uint16_t)s]=1u;g_cache_last[(uint16_t)s]=g_frame_no;}
    }

    /* Load misses into free slots, otherwise evict the least-recent resident
     * pattern not required by this frame. Slots 0..2 are permanent base tiles. */
    for(i=0u;i<req_count;++i)if(req[i].slot==0xffffu){
        uint16_t s,chosen=0xffffu;
        uint32_t oldest=UINT32_MAX;
        for(s=3u;s<HW_TILES;++s)if(!g_cache_valid[s]){chosen=s;break;}
        if(chosen==0xffffu){
            for(s=3u;s<HW_TILES;++s)if(!needed[s]&&g_cache_last[s]<=oldest){
                oldest=g_cache_last[s];chosen=s;
            }
        }
        if(chosen==0xffffu)die("no evictable hardware tile slot");
        memcpy(g_cache_pix[chosen],req[i].pix,PIXELS);
        g_cache_hash[chosen]=req[i].hash;g_cache_valid[chosen]=1u;g_cache_last[chosen]=g_frame_no;
        req[i].slot=chosen;needed[chosen]=1u;
        if(g_load_count>=TSP_HOST_MAX_FRAME_LOADS)die("frame tile-load capacity exceeded");
        g_loads[g_load_count].slot=chosen;
        encode_4bpp(req[i].pix,g_loads[g_load_count].bytes);
        ++g_load_count;++g_total_loads;
    }

    if(g_load_count>g_peak_loads)g_peak_loads=g_load_count;
    for(i=0u;i<TSP_MAP_CELLS;++i)
        out[i]=(uint16_t)(req[cell_req[i]].slot|cell_attr[i]);
}

uint16_t tsp_host_composite_frame_load_count(void){return g_load_count;}
const TSPHostTileLoad *tsp_host_composite_frame_loads(void){return g_loads;}
uint16_t tsp_host_composite_frame_unique_count(void){return g_frame_unique;}
uint16_t tsp_host_composite_peak_unique_count(void){return g_peak_unique;}
uint16_t tsp_host_composite_peak_load_count(void){return g_peak_loads;}
uint32_t tsp_host_composite_total_load_count(void){return g_total_loads;}

uint16_t tsp_host_composite_owner_pixel_count(uint8_t sid){
    uint16_t cell,n=0u;uint8_t i;
    for(cell=0u;cell<TSP_MAP_CELLS;++cell)
        for(i=0u;i<PIXELS;++i)if(g_owner[cell][i]==sid)++n;
    return n;
}
uint16_t tsp_host_composite_lit_owner_pixel_count(uint8_t sid){
    uint16_t cell,n=0u;uint8_t i;
    for(cell=0u;cell<TSP_MAP_CELLS;++cell)
        for(i=0u;i<PIXELS;++i)if(g_owner[cell][i]==sid&&g_lit[cell][i])++n;
    return n;
}

int tsp_host_composite_write_ppm(const char *path){
    static const uint8_t rgb[6][3]={
        {0,0,0},{16,16,48},{64,64,96},{96,112,144},{144,160,192},{208,224,240}
    };
    FILE *f=fopen(path,"wb");
    uint16_t y,x;
    if(!f)return 0;
    fprintf(f,"P6\n160 144\n255\n");
    for(y=0u;y<144u;++y)for(x=0u;x<160u;++x){
        uint8_t row=(uint8_t)(y>>3),col=(uint8_t)(x>>3);
        uint8_t py=(uint8_t)(y&7u),px=(uint8_t)(x&7u);
        uint16_t cell=(uint16_t)row*TSP_COLS+col;
        uint16_t pi=(uint16_t)py*8u+px;
        uint8_t v=g_cells[cell][pi];
        if(g_lighting_stage>=TSP_HOST_LIGHT_HARD&&g_lit[cell][pi])
            v=lit_semantic(v);
        if(v>5u)v=0u;
        fwrite(rgb[v],1,3,f);
    }
    fclose(f);return 1;
}
