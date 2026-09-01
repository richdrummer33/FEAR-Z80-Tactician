#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "polar_baked_composite.h"
#include "room_mesh_bake.h"

#define RMB_PI 3.14159265358979323846
#define RMB_FOCAL 80.0
#define RMB_CX 80.0
#define RMB_CY 72.0

static void rmb_fail(const char *msg){
    (void)msg;
    abort();
}

static RMBVec3 vsub(RMBVec3 a,RMBVec3 b){
    RMBVec3 r={a.x-b.x,a.y-b.y,a.z-b.z};return r;
}
static RMBVec3 vcross(RMBVec3 a,RMBVec3 b){
    RMBVec3 r={a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};return r;
}
static double vdot(RMBVec3 a,RMBVec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static double vlen(RMBVec3 a){return sqrt(vdot(a,a));}
static RMBVec3 vnorm(RMBVec3 a){
    double l=vlen(a);
    if(l<1e-12){RMBVec3 z={0,0,0};return z;}
    a.x/=l;a.y/=l;a.z/=l;return a;
}

void rmb_scene_init(RMBScene *s){memset(s,0,sizeof(*s));}

uint8_t rmb_new_object(RMBScene *s,uint8_t outline_mode){
    uint8_t id=s->object_count;
    if(id>=RMB_MAX_OBJECTS)rmb_fail("too many mesh objects");
    s->objects[id].outline_mode=outline_mode;
    s->objects[id].visible=1u;
    s->objects[id].casts_shadow=1u;
    s->objects[id].shade_levels=3u;
    s->objects[id].overlay_target_object=0xffu;
    s->objects[id].consolidate_support=0u;
    s->objects[id].consolidate_passes=0u;
    s->objects[id].smooth_shading=0u;
    s->objects[id].ramp_levels=0u;
    s->objects[id].static_light=0u;
    s->objects[id].incident_weight=1.0;
    s->objects[id].ao_radius=0.0;
    s->objects[id].ao_strength=0.0;
    s->objects[id].light_radius=0.0;
    s->objects[id].shadow_floor=1.0;
    s->objects[id].equalize=0u;
    s->objects[id].crease_coverage=0.0;
    s->objects[id].crease_depth=0.0;
    s->objects[id].ramp_dither=0u;
    s->objects[id].overlay_dither_quarters=4u;
    s->object_count=(uint8_t)(id+1u);
    return id;
}

void rmb_set_object_flags(RMBScene *s,uint8_t object_id,
                          uint8_t visible,uint8_t casts_shadow){
    if(object_id>=s->object_count)rmb_fail("invalid mesh object id");
    s->objects[object_id].visible=(uint8_t)(visible?1u:0u);
    s->objects[object_id].casts_shadow=(uint8_t)(casts_shadow?1u:0u);
}

void rmb_set_object_shade_levels(RMBScene *s,uint8_t object_id,uint8_t levels){
    if(object_id>=s->object_count)rmb_fail("invalid mesh object id");
    if(levels<1u||levels>3u)rmb_fail("mesh shade levels must be 1..3");
    s->objects[object_id].shade_levels=levels;
}

void rmb_set_object_overlay_target(RMBScene *s,uint8_t object_id,
                                   uint8_t target_object_id){
    if(object_id>=s->object_count||target_object_id>=s->object_count)
        rmb_fail("invalid mesh overlay object id");
    if(object_id==target_object_id)rmb_fail("mesh overlay cannot target itself");
    s->objects[object_id].overlay_target_object=target_object_id;
}

void rmb_set_object_overlay_dither(RMBScene *s,uint8_t object_id,
                                   uint8_t quarters){
    if(object_id>=s->object_count)rmb_fail("invalid mesh dither object id");
    if(quarters<1u||quarters>4u)rmb_fail("mesh overlay dither must be 1..4");
    s->objects[object_id].overlay_dither_quarters=quarters;
}

void rmb_set_object_crease(RMBScene *s,uint8_t object_id,double coverage,
                           double depth){
    if(object_id>=s->object_count)rmb_fail("invalid crease object id");
    if(coverage<0.0||coverage>1.0)rmb_fail("crease coverage must be 0..1");
    if(depth<0.0||depth>1.0)rmb_fail("crease depth must be 0..1");
    s->objects[object_id].crease_coverage=coverage;
    s->objects[object_id].crease_depth=depth;
}

void rmb_set_object_ramp_dither(RMBScene *s,uint8_t object_id,uint8_t on){
    if(object_id>=s->object_count)rmb_fail("invalid ramp-dither object id");
    s->objects[object_id].ramp_dither=(uint8_t)(on?1u:0u);
}

void rmb_set_object_ramp_equalize(RMBScene *s,uint8_t object_id,uint8_t on){
    if(object_id>=s->object_count)rmb_fail("invalid ramp-equalize object id");
    s->objects[object_id].equalize=(uint8_t)(on?1u:0u);
}

void rmb_set_object_incident_weight(RMBScene *s,uint8_t object_id,double w){
    if(object_id>=s->object_count)rmb_fail("invalid incident-weight object id");
    if(w<0.0||w>1.0)rmb_fail("incident weight must be 0..1");
    s->objects[object_id].incident_weight=w;
}

void rmb_set_object_static_light(RMBScene *s,uint8_t object_id,
                                 double ao_radius,double ao_strength,
                                 double light_radius,double shadow_floor){
    if(object_id>=s->object_count)rmb_fail("invalid static-light object id");
    if(ao_strength<0.0||ao_strength>1.0)rmb_fail("ao_strength must be 0..1");
    if(shadow_floor<0.0||shadow_floor>1.0)rmb_fail("shadow_floor must be 0..1");
    s->objects[object_id].static_light=1u;
    s->objects[object_id].ao_radius=ao_radius;
    s->objects[object_id].ao_strength=ao_strength;
    s->objects[object_id].light_radius=light_radius;
    s->objects[object_id].shadow_floor=shadow_floor;
}

void rmb_set_object_ramp_shading(RMBScene *s,uint8_t object_id,
                                 uint8_t levels,uint8_t smooth){
    if(object_id>=s->object_count)rmb_fail("invalid mesh ramp object id");
    if(levels<2u||levels>RMB_SHADE_RAMP_LEN)
        rmb_fail("mesh ramp levels must be 2..RMB_SHADE_RAMP_LEN");
    s->objects[object_id].ramp_levels=levels;
    s->objects[object_id].smooth_shading=(uint8_t)(smooth?1u:0u);
}

void rmb_set_object_smooth_shading(RMBScene *s,uint8_t object_id,uint8_t on){
    if(object_id>=s->object_count)rmb_fail("invalid mesh smooth-shading object id");
    s->objects[object_id].smooth_shading=(uint8_t)(on?1u:0u);
}

void rmb_set_object_shade_consolidate(RMBScene *s,uint8_t object_id,
                                      uint8_t support,uint8_t passes){
    if(object_id>=s->object_count)rmb_fail("invalid mesh consolidate object id");
    if(support>8u)rmb_fail("mesh consolidate support must be 0..8");
    s->objects[object_id].consolidate_support=support;
    s->objects[object_id].consolidate_passes=passes;
}

static RMBVec3 rotate_xyz(RMBVec3 p,double rx,double ry,double rz){
    double c,s,x,y,z;
    c=cos(rx);s=sin(rx);y=p.y*c-p.z*s;z=p.y*s+p.z*c;p.y=y;p.z=z;
    c=cos(ry);s=sin(ry);x=p.x*c+p.z*s;z=-p.x*s+p.z*c;p.x=x;p.z=z;
    c=cos(rz);s=sin(rz);x=p.x*c-p.y*s;y=p.x*s+p.y*c;p.x=x;p.y=y;
    return p;
}

RMBTransform rmb_transform(double tx,double ty,double tz,
                           double rx_deg,double ry_deg,double rz_deg,
                           double sx,double sy,double sz){
    RMBTransform t;
    t.tx=tx;t.ty=ty;t.tz=tz;
    t.rx=rx_deg*(RMB_PI/180.0);
    t.ry=ry_deg*(RMB_PI/180.0);
    t.rz=rz_deg*(RMB_PI/180.0);
    t.sx=sx;t.sy=sy;t.sz=sz;
    return t;
}

static RMBVec3 apply_xf(const RMBTransform *t,RMBVec3 p){
    p.x*=t->sx;p.y*=t->sy;p.z*=t->sz;
    p=rotate_xyz(p,t->rx,t->ry,t->rz);
    p.x+=t->tx;p.y+=t->ty;p.z+=t->tz;
    return p;
}

RMBTransform rmb_compose(const RMBTransform *parent,const RMBTransform *child){
    RMBTransform out=*child;
    RMBVec3 tr={child->tx*parent->sx,child->ty*parent->sy,child->tz*parent->sz};
    tr=rotate_xyz(tr,parent->rx,parent->ry,parent->rz);
    out.tx=parent->tx+tr.x;out.ty=parent->ty+tr.y;out.tz=parent->tz+tr.z;
    out.rx=parent->rx+child->rx;
    out.ry=parent->ry+child->ry;
    out.rz=parent->rz+child->rz;
    out.sx=parent->sx*child->sx;
    out.sy=parent->sy*child->sy;
    out.sz=parent->sz*child->sz;
    return out;
}

static uint16_t add_vertex(RMBScene *s,RMBVec3 p){
    uint16_t id=s->vertex_count;
    if(id>=RMB_MAX_VERTICES)rmb_fail("too many mesh vertices");
    s->vertices[id]=p;
    if(!s->bounds_valid){
        s->bounds_min=s->bounds_max=p;
        s->bounds_valid=1u;
    }else{
        if(p.x<s->bounds_min.x)s->bounds_min.x=p.x;
        if(p.y<s->bounds_min.y)s->bounds_min.y=p.y;
        if(p.z<s->bounds_min.z)s->bounds_min.z=p.z;
        if(p.x>s->bounds_max.x)s->bounds_max.x=p.x;
        if(p.y>s->bounds_max.y)s->bounds_max.y=p.y;
        if(p.z>s->bounds_max.z)s->bounds_max.z=p.z;
    }
    s->vertex_count=(uint16_t)(id+1u);
    return id;
}

static void add_edge_ref(RMBScene *s,uint16_t a,uint16_t b,int16_t tri,uint8_t obj){
    uint16_t i,lo=a<b?a:b,hi=a<b?b:a;
    for(i=0u;i<s->edge_count;++i){
        RMBEdge *e=&s->edges[i];
        if(e->a==lo&&e->b==hi&&e->object_id==obj){
            if(e->tri1<0)e->tri1=tri;
            return;
        }
    }
    if(s->edge_count>=RMB_MAX_EDGES)rmb_fail("too many mesh edges");
    s->edges[s->edge_count].a=lo;
    s->edges[s->edge_count].b=hi;
    s->edges[s->edge_count].tri0=tri;
    s->edges[s->edge_count].tri1=-1;
    s->edges[s->edge_count].object_id=obj;
    ++s->edge_count;
}

static void add_triangle(RMBScene *s,uint8_t obj,uint16_t a,uint16_t b,uint16_t c,
                         int8_t bias){
    uint16_t id=s->triangle_count;
    RMBTriangle *t;
    if(id>=RMB_MAX_TRIANGLES)rmb_fail("too many mesh triangles");
    t=&s->triangles[id];
    t->v[0]=a;t->v[1]=b;t->v[2]=c;t->object_id=obj;t->shade_bias=bias;
    s->triangle_count=(uint16_t)(id+1u);
    /* Edge adjacency exists only for objects that actually request outline
     * rendering. Imported hero meshes intentionally use silent polygon edges;
     * skipping adjacency here avoids both O(E^2) edge insertion and a large
     * useless edge vocabulary. */
    if(s->objects[obj].outline_mode!=RMB_OUTLINE_NONE){
        add_edge_ref(s,a,b,(int16_t)id,obj);
        add_edge_ref(s,b,c,(int16_t)id,obj);
        add_edge_ref(s,c,a,(int16_t)id,obj);
    }
}

void rmb_add_box(RMBScene *s,uint8_t obj,const RMBTransform *xf,
                 double hx,double hy,double hz,int8_t bias){
    static const int8_t q[8][3]={
        {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}
    };
    static const uint8_t f[12][3]={
        {0,3,2},{0,2,1},{4,5,6},{4,6,7},
        {0,1,5},{0,5,4},{3,7,6},{3,6,2},
        {0,4,7},{0,7,3},{1,2,6},{1,6,5}
    };
    uint16_t v[8];uint8_t i;
    for(i=0u;i<8u;++i){
        RMBVec3 p={(double)q[i][0]*hx,(double)q[i][1]*hy,(double)q[i][2]*hz};
        v[i]=add_vertex(s,apply_xf(xf,p));
    }
    for(i=0u;i<12u;++i)add_triangle(s,obj,v[f[i][0]],v[f[i][1]],v[f[i][2]],bias);
}

void rmb_add_indexed_mesh_q8(RMBScene *s,uint8_t obj,const RMBTransform *xf,
                             const int16_t *xyz_q8,uint16_t vertex_count,
                             const uint16_t *indices,uint16_t triangle_count,
                             int8_t bias){
    rmb_add_indexed_mesh_q8_ex(s,obj,xf,xyz_q8,vertex_count,indices,
                               triangle_count,bias,NULL);
}

void rmb_add_indexed_mesh_q8_ex(RMBScene *s,uint8_t obj,const RMBTransform *xf,
                                const int16_t *xyz_q8,uint16_t vertex_count,
                                const uint16_t *indices,uint16_t triangle_count,
                                int8_t bias,const uint8_t *vertex_recess){
    uint16_t base,i;
    uint32_t t;
    if(obj>=s->object_count)rmb_fail("invalid indexed-mesh object id");
    if(!xyz_q8||!indices||!vertex_count||!triangle_count)
        rmb_fail("invalid indexed-mesh buffers");
    if((uint32_t)s->vertex_count+vertex_count>RMB_MAX_VERTICES)
        rmb_fail("indexed mesh exceeds vertex capacity");
    if((uint32_t)s->triangle_count+triangle_count>RMB_MAX_TRIANGLES)
        rmb_fail("indexed mesh exceeds triangle capacity");

    base=s->vertex_count;
    for(i=0u;i<vertex_count;++i){
        RMBVec3 p={
            (double)xyz_q8[(uint32_t)i*3u+0u]/256.0,
            (double)xyz_q8[(uint32_t)i*3u+1u]/256.0,
            (double)xyz_q8[(uint32_t)i*3u+2u]/256.0
        };
        uint16_t v=add_vertex(s,apply_xf(xf,p));
        s->vertex_recess[v]=vertex_recess?vertex_recess[i]:0u;
    }
    if(vertex_recess)s->objects[obj].recess_supplied=1u;
    for(t=0u;t<(uint32_t)triangle_count;++t){
        uint16_t a=indices[t*3u+0u],b=indices[t*3u+1u],d=indices[t*3u+2u];
        if(a>=vertex_count||b>=vertex_count||d>=vertex_count)
            rmb_fail("indexed mesh index out of range");
        add_triangle(s,obj,(uint16_t)(base+a),(uint16_t)(base+b),
                     (uint16_t)(base+d),bias);
    }
}

void rmb_add_cylinder(RMBScene *s,uint8_t obj,const RMBTransform *xf,
                      double radius,double height,uint8_t sides,
                      int8_t bias,uint8_t caps){
    uint16_t bot[64],top[64],cb=0u,ct=0u;
    uint8_t i,n;
    if(sides<3u||sides>64u)rmb_fail("invalid cylinder sides");
    n=sides;
    for(i=0u;i<n;++i){
        double a=(2.0*RMB_PI*(double)i)/(double)n;
        RMBVec3 b={radius*cos(a),radius*sin(a),-height*0.5};
        RMBVec3 t={b.x,b.y,height*0.5};
        bot[i]=add_vertex(s,apply_xf(xf,b));
        top[i]=add_vertex(s,apply_xf(xf,t));
    }
    if(caps){
        RMBVec3 b={0,0,-height*0.5},t={0,0,height*0.5};
        cb=add_vertex(s,apply_xf(xf,b));ct=add_vertex(s,apply_xf(xf,t));
    }
    for(i=0u;i<n;++i){
        uint8_t j=(uint8_t)((i+1u)%n);
        add_triangle(s,obj,bot[i],bot[j],top[j],bias);
        add_triangle(s,obj,bot[i],top[j],top[i],bias);
        if(caps){
            add_triangle(s,obj,cb,bot[j],bot[i],bias);
            add_triangle(s,obj,ct,top[i],top[j],bias);
        }
    }
}

void rmb_add_uv_sphere(RMBScene *s,uint8_t obj,const RMBTransform *xf,
                       double radius,uint8_t rings,uint8_t slices,int8_t bias){
    uint16_t ring[32][64],top,bottom;
    uint8_t r,i;
    RMBVec3 p={0,0,radius};
    if(rings<3u||rings>31u||slices<4u||slices>64u)
        rmb_fail("invalid sphere tessellation");
    top=add_vertex(s,apply_xf(xf,p));
    p.z=-radius;bottom=add_vertex(s,apply_xf(xf,p));
    for(r=1u;r<rings;++r){
        double th=RMB_PI*(double)r/(double)rings;
        double rr=radius*sin(th),zz=radius*cos(th);
        for(i=0u;i<slices;++i){
            double a=2.0*RMB_PI*(double)i/(double)slices;
            RMBVec3 q={rr*cos(a),rr*sin(a),zz};
            ring[r][i]=add_vertex(s,apply_xf(xf,q));
        }
    }
    for(i=0u;i<slices;++i){
        uint8_t j=(uint8_t)((i+1u)%slices);
        add_triangle(s,obj,top,ring[1][i],ring[1][j],bias);
        add_triangle(s,obj,ring[rings-1u][i],bottom,ring[rings-1u][j],bias);
    }
    for(r=1u;r<rings-1u;++r)for(i=0u;i<slices;++i){
        uint8_t j=(uint8_t)((i+1u)%slices);
        add_triangle(s,obj,ring[r][i],ring[r+1u][i],ring[r+1u][j],bias);
        add_triangle(s,obj,ring[r][i],ring[r+1u][j],ring[r][j],bias);
    }
}

void rmb_add_dome(RMBScene *s,uint8_t obj,const RMBTransform *xf,
                  double radius,uint8_t rings,uint8_t slices,
                  int8_t bias,uint8_t base_cap){
    uint16_t ring[32][64],top,center=0u;
    uint8_t r,i;
    RMBVec3 p={0,0,radius};
    if(rings<2u||rings>31u||slices<4u||slices>64u)
        rmb_fail("invalid dome tessellation");
    top=add_vertex(s,apply_xf(xf,p));
    for(r=1u;r<=rings;++r){
        double th=(RMB_PI*0.5)*(double)r/(double)rings;
        double rr=radius*sin(th),zz=radius*cos(th);
        for(i=0u;i<slices;++i){
            double a=2.0*RMB_PI*(double)i/(double)slices;
            RMBVec3 q={rr*cos(a),rr*sin(a),zz};
            ring[r][i]=add_vertex(s,apply_xf(xf,q));
        }
    }
    if(base_cap){RMBVec3 z={0,0,0};center=add_vertex(s,apply_xf(xf,z));}
    for(i=0u;i<slices;++i){
        uint8_t j=(uint8_t)((i+1u)%slices);
        add_triangle(s,obj,top,ring[1][i],ring[1][j],bias);
    }
    for(r=1u;r<rings;++r)for(i=0u;i<slices;++i){
        uint8_t j=(uint8_t)((i+1u)%slices);
        add_triangle(s,obj,ring[r][i],ring[r+1u][i],ring[r+1u][j],bias);
        add_triangle(s,obj,ring[r][i],ring[r+1u][j],ring[r][j],bias);
    }
    if(base_cap)for(i=0u;i<slices;++i){
        uint8_t j=(uint8_t)((i+1u)%slices);
        add_triangle(s,obj,center,ring[rings][j],ring[rings][i],bias);
    }
}

typedef struct Proj {
    double x,y,d,inv;
    uint8_t ok;
} Proj;

static Proj project(RMBVec3 p,double cx,double cy,double cz,double yaw){
    Proj o;
    double fx=cos(yaw),fy=sin(yaw),rx=-fy,ry=fx;
    double dx=p.x-cx,dy=p.y-cy;
    double d=dx*fx+dy*fy,l=dx*rx+dy*ry;
    o.ok=(uint8_t)(d>0.5);
    o.d=d;o.inv=d>1e-12?1.0/d:0.0;
    o.x=RMB_CX+l*RMB_FOCAL/d;
    o.y=RMB_CY-(p.z-cz)*RMB_FOCAL/d;
    return o;
}

static double edge2(double ax,double ay,double bx,double by,double px,double py){
    return (px-ax)*(by-ay)-(py-ay)*(bx-ax);
}

/* Area-weighted per-vertex normals, rebuilt when the scene geometry changes.
 * Baking is offline, so this is deliberately recomputed rather than packed. */
static RMBVec3 g_vnormal[RMB_MAX_VERTICES];
static const RMBScene *g_vnormal_scene=NULL;
static uint16_t g_vnormal_vertices=0u,g_vnormal_triangles=0u;

static void ensure_vertex_normals(const RMBScene *s){
    uint16_t i;
    if(g_vnormal_scene==s&&g_vnormal_vertices==s->vertex_count&&
       g_vnormal_triangles==s->triangle_count)return;
    for(i=0u;i<s->vertex_count;++i){
        g_vnormal[i].x=0.0;g_vnormal[i].y=0.0;g_vnormal[i].z=0.0;
    }
    for(i=0u;i<s->triangle_count;++i){
        const RMBTriangle *t=&s->triangles[i];
        RMBVec3 a=s->vertices[t->v[0]],b=s->vertices[t->v[1]],c=s->vertices[t->v[2]];
        /* Unnormalized cross product is already area-weighted. */
        RMBVec3 n=vcross(vsub(b,a),vsub(c,a));
        uint8_t k;
        for(k=0u;k<3u;++k){
            g_vnormal[t->v[k]].x+=n.x;
            g_vnormal[t->v[k]].y+=n.y;
            g_vnormal[t->v[k]].z+=n.z;
        }
    }
    g_vnormal_scene=s;
    g_vnormal_vertices=s->vertex_count;
    g_vnormal_triangles=s->triangle_count;
}

static uint8_t face_shade(RMBVec3 n,RMBVec3 c,const RMBLight *light,int8_t bias){
    RMBVec3 l;
    double nd;
    int s;
    n=vnorm(n);
    if(light&&light->enabled){
        RMBVec3 lp={light->x,light->y,light->z};
        l=vnorm(vsub(lp,c));
    }else{
        RMBVec3 dl={-0.45,-0.55,0.72};
        l=vnorm(dl);
    }
    nd=vdot(n,l);
    s=nd>0.62?2:(nd>0.12?1:0);
    s+=bias;if(s<0)s=0;if(s>2)s=2;
    return (uint8_t)s;
}


/* ------------------------------------------------------------------------
 * Static per-vertex lighting bake.
 *
 * The hero mesh and its light are both fixed in world space, so "is this
 * surface point in shadow?" and "how enclosed is this surface point?" are
 * properties of the geometry alone. Solving them per pixel per frame would be
 * billions of ray tests; solving them once per vertex is a few seconds of
 * offline work and then costs one barycentric interpolation per pixel.
 *
 * Both terms are evaluated against the object's OWN full-resolution triangles,
 * not the decimated shadow proxy, because the features that make a figure
 * read -- the gap under a raised arm, the seam beside a chest plate -- are
 * exactly the ones decimation removes first.
 * ------------------------------------------------------------------------ */
static int cmp_double(const void *a,const void *b){
    double x=*(const double *)a,y=*(const double *)b;
    return x<y?-1:(x>y?1:0);
}
static int segment_triangle_hit(RMBVec3 o,RMBVec3 d,
                                RMBVec3 a,RMBVec3 b,RMBVec3 c);
static double surface_brightness(RMBVec3 n,RMBVec3 p,const RMBLight *light,
                                 double vis,double open,double recess,
                                 const RMBObject *ob);

#define RMB_AO_RAYS 24u
#define RMB_LIGHT_SAMPLES 8u

static double g_ramp_thresh[RMB_MAX_OBJECTS][RMB_SHADE_RAMP_LEN];
static float g_vcrease[RMB_MAX_VERTICES];
static float g_vlight[RMB_MAX_VERTICES];
static float g_vao[RMB_MAX_VERTICES];
static const RMBScene *g_vstatic_scene=NULL;
static uint16_t g_vstatic_vertices=0u,g_vstatic_triangles=0u;
static double g_vstatic_lx,g_vstatic_ly,g_vstatic_lz;
static uint8_t g_vstatic_light_on;

/* Compact triangle list for the object being baked, so each probe walks its
 * own geometry instead of rescanning the whole scene. */
static uint16_t g_bake_tris[RMB_MAX_TRIANGLES];
static uint16_t g_bake_tri_count;

static int bake_segment_hit(const RMBScene *s,RMBVec3 o,RMBVec3 d){
    uint16_t i;
    for(i=0u;i<g_bake_tri_count;++i){
        const RMBTriangle *t=&s->triangles[g_bake_tris[i]];
        if(segment_triangle_hit(o,d,s->vertices[t->v[0]],
                                s->vertices[t->v[1]],s->vertices[t->v[2]]))
            return 1;
    }
    return 0;
}

/* Orthonormal basis around n, chosen without a branch-dependent degeneracy. */
static void basis_from_normal(RMBVec3 n,RMBVec3 *tx,RMBVec3 *ty){
    RMBVec3 up;
    up.x=fabs(n.z)<0.9?0.0:1.0;
    up.y=0.0;
    up.z=fabs(n.z)<0.9?1.0:0.0;
    *tx=vnorm(vcross(up,n));
    *ty=vcross(n,*tx);
}

/* Deterministic cosine-weighted hemisphere directions (Fibonacci spiral).
 * Fixed rather than random so two bakes of the same asset are identical. */
static RMBVec3 hemisphere_dir(uint8_t i,uint8_t count,RMBVec3 n,
                              RMBVec3 tx,RMBVec3 ty){
    double u=((double)i+0.5)/(double)count;
    double phi=(double)i*2.399963229728653; /* golden angle */
    double r=sqrt(u),z=sqrt(1.0-u);
    double a=r*cos(phi),b=r*sin(phi);
    RMBVec3 d;
    d.x=tx.x*a+ty.x*b+n.x*z;
    d.y=tx.y*a+ty.y*b+n.y*z;
    d.z=tx.z*a+ty.z*b+n.z*z;
    return vnorm(d);
}

/*
 * Per-vertex surface concavity (a discrete mean-curvature / umbrella operator).
 *
 *   delta     = centroid(one-ring) - vertex
 *   concavity = dot(delta, outward normal) / mean ring edge length
 *
 * Positive is a concave crevice or seam, negative a convex ridge. Dividing by
 * the local edge length makes it scale-free, so the same threshold works on a
 * dense chest plate and a coarse rock base. This deliberately matches the
 * convention already pinned by tools/glb_rmb/analyze_seams.mjs, so a crease
 * measured here means the same thing as a seam extracted there.
 *
 * Ambient occlusion answers "how enclosed is this point"; concavity answers
 * "is this point in a fold". They are different questions and the crease cue
 * needs both: occlusion alone marks the whole underside of an arm, curvature
 * alone marks every tessellation wrinkle. Their product isolates the actual
 * recessed folds -- the back of a knee, an armpit, the helmet/neck junction.
 *
 * The raw field is noisy on a decimated shell, so it is Laplacian-smoothed
 * over the same one-ring. That turns scattered high-curvature vertices into
 * continuous valleys, which is what makes the result read as a line following
 * the fold rather than as speckle sitting near it.
 */
static void compute_concavity(const RMBScene *s,uint8_t object_id,
                              uint8_t smooth_passes){
    static double acc[RMB_MAX_VERTICES];
    static double wsum[RMB_MAX_VERTICES];
    static double scale[RMB_MAX_VERTICES];
    static RMBVec3 ring[RMB_MAX_VERTICES];
    static double tmp[RMB_MAX_VERTICES];
    uint16_t i,vi;
    uint8_t pass,k;

    for(vi=0u;vi<s->vertex_count;++vi){
        ring[vi].x=ring[vi].y=ring[vi].z=0.0;
        wsum[vi]=0.0;scale[vi]=0.0;acc[vi]=0.0;
    }
    for(i=0u;i<s->triangle_count;++i){
        const RMBTriangle *t=&s->triangles[i];
        if(t->object_id!=object_id)continue;
        for(k=0u;k<3u;++k){
            uint16_t v=t->v[k],a2=t->v[(k+1u)%3u],b2=t->v[(k+2u)%3u];
            RMBVec3 pv=s->vertices[v];
            uint8_t m;
            uint16_t nb[2];
            nb[0]=a2;nb[1]=b2;
            for(m=0u;m<2u;++m){
                RMBVec3 pn=s->vertices[nb[m]];
                ring[v].x+=pn.x;ring[v].y+=pn.y;ring[v].z+=pn.z;
                scale[v]+=vlen(vsub(pn,pv));
                wsum[v]+=1.0;
            }
        }
    }
    for(vi=0u;vi<s->vertex_count;++vi){
        RMBVec3 n=g_vnormal[vi],d;
        double sc;
        if(wsum[vi]<1.0||vdot(n,n)<1e-18){acc[vi]=0.0;continue;}
        n=vnorm(n);
        d.x=ring[vi].x/wsum[vi]-s->vertices[vi].x;
        d.y=ring[vi].y/wsum[vi]-s->vertices[vi].y;
        d.z=ring[vi].z/wsum[vi]-s->vertices[vi].z;
        sc=scale[vi]/wsum[vi];
        acc[vi]=sc>1e-9?vdot(d,n)/sc:0.0;
    }

    for(pass=0u;pass<smooth_passes;++pass){
        for(vi=0u;vi<s->vertex_count;++vi){tmp[vi]=0.0;wsum[vi]=0.0;}
        for(i=0u;i<s->triangle_count;++i){
            const RMBTriangle *t=&s->triangles[i];
            if(t->object_id!=object_id)continue;
            for(k=0u;k<3u;++k){
                uint16_t v=t->v[k];
                tmp[v]+=acc[t->v[(k+1u)%3u]]+acc[t->v[(k+2u)%3u]];
                wsum[v]+=2.0;
            }
        }
        for(vi=0u;vi<s->vertex_count;++vi)
            if(wsum[vi]>0.0)acc[vi]=0.5*acc[vi]+0.5*(tmp[vi]/wsum[vi]);
    }

    for(vi=0u;vi<s->vertex_count;++vi)g_vcrease[vi]=(float)acc[vi];
}

static void ensure_static_lighting(const RMBScene *s,const RMBLight *light){
    uint8_t light_on=(uint8_t)(light&&light->enabled);
    double lx=light_on?light->x:0.0;
    double ly=light_on?light->y:0.0;
    double lz=light_on?light->z:0.0;
    uint16_t vi,ti;
    uint8_t oid;

    if(g_vstatic_scene==s&&g_vstatic_vertices==s->vertex_count&&
       g_vstatic_triangles==s->triangle_count&&
       g_vstatic_light_on==light_on&&g_vstatic_lx==lx&&
       g_vstatic_ly==ly&&g_vstatic_lz==lz)return;

    for(vi=0u;vi<s->vertex_count;++vi){
        g_vlight[vi]=1.0f;g_vao[vi]=1.0f;g_vcrease[vi]=0.0f;
    }
    for(oid=0u;oid<RMB_MAX_OBJECTS;++oid){
        uint8_t k;
        for(k=0u;k<RMB_SHADE_RAMP_LEN;++k)g_ramp_thresh[oid][k]=1.0;
    }

    for(oid=0u;oid<s->object_count;++oid){
        const RMBObject *ob=&s->objects[oid];
        RMBVec3 ldir={0.0,0.0,1.0},lu,lv;
        double eps;
        if(!ob->static_light)continue;

        g_bake_tri_count=0u;
        for(ti=0u;ti<s->triangle_count;++ti)
            if(s->triangles[ti].object_id==oid)
                g_bake_tris[g_bake_tri_count++]=ti;
        if(!g_bake_tri_count)continue;

        if(ob->crease_coverage>0.0){
            if(ob->recess_supplied){
                /* Source-measured field: decimation removes folds before it
                 * removes anything else, so the shell cannot measure its own. */
                for(vi=0u;vi<s->vertex_count;++vi)
                    g_vcrease[vi]=(float)s->vertex_recess[vi]/255.0f;
            }else compute_concavity(s,oid,2u);
        }

        /* Offset probe origins off the surface so a ray does not immediately
         * re-hit the triangle that spawned it. */
        eps=ob->ao_radius>0.0?ob->ao_radius*0.02:0.02;
        if(eps<0.01)eps=0.01;

        for(vi=0u;vi<s->vertex_count;++vi){
            RMBVec3 n,p,o,tx,ty;
            uint8_t k,blocked;
            /* Only vertices belonging to this object have accumulated normals
             * from its own triangles; others keep the defaults set above. */
            n=g_vnormal[vi];
            if(vdot(n,n)<1e-18)continue;
            n=vnorm(n);
            p=s->vertices[vi];
            o.x=p.x+n.x*eps;o.y=p.y+n.y*eps;o.z=p.z+n.z*eps;
            basis_from_normal(n,&tx,&ty);

            if(ob->ao_radius>0.0){
                uint8_t open=0u;
                for(k=0u;k<RMB_AO_RAYS;++k){
                    RMBVec3 d=hemisphere_dir(k,RMB_AO_RAYS,n,tx,ty);
                    d.x*=ob->ao_radius;d.y*=ob->ao_radius;d.z*=ob->ao_radius;
                    if(!bake_segment_hit(s,o,d))++open;
                }
                g_vao[vi]=(float)((double)open/(double)RMB_AO_RAYS);
            }

            if(light_on){
                RMBVec3 lp={0,0,0};
                lp.x=lx;lp.y=ly;lp.z=lz;
                ldir=vnorm(vsub(lp,p));
                if(vdot(n,ldir)<=0.0){
                    /* Facing away: the incident-angle term already handles it,
                     * and a grazing probe here only produces shadow acne. */
                    g_vlight[vi]=0.0f;
                }else if(ob->light_radius>0.0){
                    basis_from_normal(ldir,&lu,&lv);
                    blocked=0u;
                    for(k=0u;k<RMB_LIGHT_SAMPLES;++k){
                        double a=((double)k+0.5)*2.0*RMB_PI/(double)RMB_LIGHT_SAMPLES;
                        double rr=ob->light_radius*((k&1u)?1.0:0.5);
                        RMBVec3 sp,d;
                        sp.x=lx+lu.x*cos(a)*rr+lv.x*sin(a)*rr;
                        sp.y=ly+lu.y*cos(a)*rr+lv.y*sin(a)*rr;
                        sp.z=lz+lu.z*cos(a)*rr+lv.z*sin(a)*rr;
                        d=vsub(o,sp);
                        if(bake_segment_hit(s,sp,d))++blocked;
                    }
                    g_vlight[vi]=(float)(1.0-(double)blocked/
                                              (double)RMB_LIGHT_SAMPLES);
                }else{
                    RMBVec3 sp,d;
                    sp.x=lx;sp.y=ly;sp.z=lz;
                    d=vsub(o,sp);
                    g_vlight[vi]=bake_segment_hit(s,sp,d)?0.0f:1.0f;
                }
            }
        }

        /*
         * Turn the two raw geometric measurements into a single normalized
         * recess field.
         *
         * Measured on this asset, concavity is NEGATIVE for about 89% of the
         * surface -- a human figure is overwhelmingly convex, and only genuine
         * folds come out positive -- while openness sits at 0.92 at the median.
         * Multiplying the two raw terms therefore produced a field that was
         * numerically almost everywhere zero: the first attempt darkened 482
         * pixels across 2,500 frames, which is invisible.
         *
         * So the two cues are ADDED, not multiplied, and the result is cut at
         * a percentile. Curvature says "this is a fold", enclosure says "this
         * is buried"; either one contributes, and the places that read as real
         * recesses score on both. Cutting by percentile means the control is
         * "what fraction of the surface reads as folded", which transfers
         * across assets and light rigs instead of needing a magnitude retuned
         * for every mesh.
         */
        if(ob->crease_coverage>0.0){
            static double rsamp[RMB_MAX_VERTICES];
            static double raw[RMB_MAX_VERTICES];
            uint32_t rn=0u,idx;
            double cmax=0.0,cut,top;
            for(vi=0u;vi<s->vertex_count;++vi)
                if(g_vcrease[vi]>cmax)cmax=g_vcrease[vi];
            if(cmax<1e-9)cmax=1.0;
            for(vi=0u;vi<s->vertex_count;++vi){
                double c=(double)g_vcrease[vi];
                double fold=c>0.0?c/cmax:0.0;
                raw[vi]=fold+(1.0-(double)g_vao[vi]);
                if(vdot(g_vnormal[vi],g_vnormal[vi])>1e-18)
                    rsamp[rn++]=raw[vi];
            }
            if(rn){
                qsort(rsamp,rn,sizeof(double),cmp_double);
                idx=(uint32_t)((1.0-ob->crease_coverage)*(double)(rn-1u));
                cut=rsamp[idx];
                top=rsamp[rn-1u];
                (void)top;
                /*
                 * Normalize by RANK, not by value. The recess field has a long
                 * tail -- on this asset the 90th percentile is 0.026 while the
                 * maximum is 0.125 -- so a linear value remap left almost the
                 * whole drawn set bunched against zero and the crease read as
                 * a faint blue haze in the diagnostic rather than as lines.
                 * Ranking spreads the drawn fraction evenly over the dither's
                 * full coverage range, which is the same equalization argument
                 * used for the brightness ramp.
                 */
                for(vi=0u;vi<s->vertex_count;++vi){
                    uint32_t lo=idx,hi=rn,mid;
                    double v;
                    if(raw[vi]<=cut){g_vcrease[vi]=0.0f;continue;}
                    while(lo<hi){
                        mid=lo+(hi-lo)/2u;
                        if(rsamp[mid]<raw[vi])lo=mid+1u;else hi=mid;
                    }
                    v=(rn>idx+1u)?(double)(lo-idx)/(double)(rn-idx-1u):1.0;
                    if(v<0.0)v=0.0;
                    if(v>1.0)v=1.0;
                    g_vcrease[vi]=(float)v;
                }
            }
        }

        /*
         * Choose the ramp thresholds from this object's own brightness
         * distribution instead of assuming an exposure.
         *
         * The three shading terms multiply, so their product is naturally
         * bunched: with a plausible AO strength and shadow floor, ~88% of the
         * figure measured onto the darkest stop and the statue went back to
         * being a silhouette. Rather than hand-tuning constants per light
         * rig, sort the per-vertex brightnesses and cut at equal quantiles.
         * Every stop then carries a similar share of the surface for any
         * light setup, and the thresholds are computed once from static
         * geometry so they cannot shimmer between frames.
         */
        if(ob->equalize&&ob->ramp_levels>=2u){
            static double bsamp[RMB_MAX_VERTICES];
            uint32_t bn=0u;
            uint8_t k;
            for(vi=0u;vi<s->vertex_count;++vi){
                RMBVec3 n=g_vnormal[vi];
                if(vdot(n,n)<1e-18)continue;
                bsamp[bn++]=surface_brightness(n,s->vertices[vi],light,
                                               (double)g_vlight[vi],
                                               (double)g_vao[vi],
                                               (double)g_vcrease[vi],ob);
            }
            if(bn){
                /* Insertion-free ordering: a simple comparison sort is ample
                 * for a few thousand offline samples. */
                qsort(bsamp,bn,sizeof(double),cmp_double);
                for(k=0u;k<ob->ramp_levels;++k){
                    uint32_t idx=(uint32_t)(((uint64_t)(k+1u)*bn)/ob->ramp_levels);
                    if(idx>=bn)idx=bn-1u;
                    g_ramp_thresh[oid][k]=bsamp[idx];
                }
                g_ramp_thresh[oid][ob->ramp_levels-1u]=1.0;
            }
        }
    }

    g_vstatic_scene=s;
    g_vstatic_vertices=s->vertex_count;
    g_vstatic_triangles=s->triangle_count;
    g_vstatic_light_on=light_on;
    g_vstatic_lx=lx;g_vstatic_ly=ly;g_vstatic_lz=lz;
}

/*
 * Surface brightness for one pixel, as a 0..1 scalar before quantization.
 *
 * Three terms, deliberately kept separate because they answer different
 * questions and fail in different ways:
 *
 *   incident  -- which way the surface turns relative to the light. This is
 *                the term the old three-stop wall shade could not resolve:
 *                with only "facing / oblique / away" every turned surface
 *                collapsed onto one value and the figure read as a silhouette.
 *                Wrapped half-Lambert, because measured over this chamber the
 *                incident angle is near-uniform over [-1,+1]; a plain Lambert
 *                or a gamma curve was measured first and put roughly 45% of
 *                the figure on the darkest stop, which is the flatness itself.
 *
 *   visibility -- whether the light actually reaches the point, baked per
 *                vertex. This is what puts the raised arm's shadow on the
 *                chest. It is separate from incident angle on purpose: a
 *                surface can face the light and still be occluded.
 *
 *   openness  -- how enclosed the point is, baked per vertex. This darkens
 *                creases, seams and deep insets. Unlike the renderer's
 *                authored corner AO, which is a wall-corner cue, this is
 *                measured from the source geometry itself.
 *
 * Because visibility multiplies rather than replaces the incident term, a
 * steeply-turned surface stays darker than a facing one both in light and in
 * shadow: the shadow never flattens the angular information back out.
 */
static double surface_brightness(RMBVec3 n,RMBVec3 p,const RMBLight *light,
                                 double vis,double open,double recess,
                                 const RMBObject *ob){
    RMBVec3 l;
    double nd,b;
    n=vnorm(n);
    if(light&&light->enabled){
        RMBVec3 lp={light->x,light->y,light->z};
        l=vnorm(vsub(lp,p));
    }else{
        RMBVec3 dl={-0.45,-0.55,0.72};
        l=vnorm(dl);
    }
    nd=vdot(n,l);
    b=0.5+0.5*nd;                                  /* incident  */
    b=(1.0-ob->incident_weight)*0.5+ob->incident_weight*b;
    b*=ob->shadow_floor+(1.0-ob->shadow_floor)*vis; /* visibility */
    b*=1.0-ob->ao_strength*(1.0-open);              /* openness  */
    b*=1.0-ob->crease_depth*recess;                 /* crease    */
    if(b<0.0)b=0.0;
    if(b>1.0)b=1.0;
    return b;
}

/* Quantize 0..1 brightness onto the compositor ramp, spreading a reduced
 * level count across the full ramp so it still reaches both endpoints. */
static uint8_t ramp_quantize(double b,int8_t bias,uint8_t levels,
                             const RMBObject *ob,uint8_t object_id,
                             int px,int py){
    /* Ordered 4x4 Bayer, matching the coverage vocabulary the renderer already
     * uses for one-sided penumbra and cavity overlays. */
    static const uint8_t k_bayer4[16]={
        0u,8u,2u,10u,12u,4u,14u,6u,3u,11u,1u,9u,15u,7u,13u,5u
    };
    int q;
    if(ob->equalize){
        for(q=0;q<(int)levels-1;++q)if(b<=g_ramp_thresh[object_id][q])break;
        if(ob->ramp_dither&&q<(int)levels-1){
            /* Position within the band, dithered up toward the next stop, so
             * a gradient crossing a threshold feathers rather than steps. */
            double lo=q?g_ramp_thresh[object_id][q-1]:0.0;
            double hi=g_ramp_thresh[object_id][q];
            double f=hi>lo?(b-lo)/(hi-lo):0.0;
            if(f>((double)k_bayer4[(py&3)*4+(px&3)]+0.5)/16.0)++q;
        }
    }else{
        double t=b*(double)levels;
        q=(int)t;
        if(ob->ramp_dither&&
           (t-(double)q)>((double)k_bayer4[(py&3)*4+(px&3)]+0.5)/16.0)++q;
    }
    if(q>=(int)levels)q=(int)levels-1;
    if(q<0)q=0;
    q+=bias;
    if(q<0)q=0;
    if(q>(int)levels-1)q=(int)levels-1;
    if(levels>=RMB_SHADE_RAMP_LEN)return (uint8_t)q;
    return (uint8_t)((q*(RMB_SHADE_RAMP_LEN-1u))/(levels-1u));
}

static uint8_t tri_front(const RMBScene *s,const RMBTriangle *t,RMBVec3 cam){
    RMBVec3 a=s->vertices[t->v[0]],b=s->vertices[t->v[1]],c=s->vertices[t->v[2]];
    RMBVec3 n=vcross(vsub(b,a),vsub(c,a));
    RMBVec3 ctr={(a.x+b.x+c.x)/3.0,(a.y+b.y+c.y)/3.0,(a.z+b.z+c.z)/3.0};
    return (uint8_t)(vdot(n,vsub(cam,ctr))>0.0);
}

static void raster_triangle(const RMBScene *s,const RMBTriangle *t,
                            double cx,double cy,double cz,double yaw,
                            const RMBLight *light,uint8_t owner){
    RMBVec3 a=s->vertices[t->v[0]],b=s->vertices[t->v[1]],c=s->vertices[t->v[2]];
    RMBVec3 n=vcross(vsub(b,a),vsub(c,a));
    RMBVec3 ctr={(a.x+b.x+c.x)/3.0,(a.y+b.y+c.y)/3.0,(a.z+b.z+c.z)/3.0};
    Proj pa=project(a,cx,cy,cz,yaw),pb=project(b,cx,cy,cz,yaw),pc=project(c,cx,cy,cz,yaw);
    double area,minx,maxx,miny,maxy;
    int x0,x1,y0,y1,x,y;
    uint8_t shade;
    if(!pa.ok||!pb.ok||!pc.ok)return;
    if(vdot(n,(RMBVec3){cx-ctr.x,cy-ctr.y,cz-ctr.z})<=0.0)return;
    area=edge2(pa.x,pa.y,pb.x,pb.y,pc.x,pc.y);
    if(fabs(area)<1e-8)return;
    minx=fmin(pa.x,fmin(pb.x,pc.x));maxx=fmax(pa.x,fmax(pb.x,pc.x));
    miny=fmin(pa.y,fmin(pb.y,pc.y));maxy=fmax(pa.y,fmax(pb.y,pc.y));
    x0=(int)floor(minx);x1=(int)ceil(maxx);
    y0=(int)floor(miny);y1=(int)ceil(maxy);
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>159)x1=159;
    if(y1>143)y1=143;
    if(x0>x1||y0>y1)return;
    shade=face_shade(n,ctr,light,t->shade_bias);
    {
        uint8_t levels=s->objects[t->object_id].shade_levels;
        if(levels==1u)shade=1u;
        else if(levels==2u)shade=(uint8_t)(shade?2u:0u);
    }
    for(y=y0;y<=y1;++y)for(x=x0;x<=x1;++x){
        double px=(double)x+0.5,py=(double)y+0.5;
        double w0=edge2(pb.x,pb.y,pc.x,pc.y,px,py)/area;
        double w1=edge2(pc.x,pc.y,pa.x,pa.y,px,py)/area;
        double w2=1.0-w0-w1;
        double inv,d;
        if(w0<-1e-7||w1<-1e-7||w2<-1e-7)continue;
        inv=w0*pa.inv+w1*pb.inv+w2*pc.inv;
        if(inv<=1e-12)continue;
        d=1.0/inv;
        if(s->objects[t->object_id].ramp_levels){
            const RMBObject *ob=&s->objects[t->object_id];
            /* Perspective-correct barycentrics: screen-space weights divided
             * by each vertex depth, renormalized by their sum (which is inv). */
            double q0=w0*pa.inv/inv,q1=w1*pb.inv/inv,q2=w2*pc.inv/inv;
            RMBVec3 pn,pw;
            double vis,open,cr;
            uint8_t level,recess;
            pw.x=q0*a.x+q1*b.x+q2*c.x;
            pw.y=q0*a.y+q1*b.y+q2*c.y;
            pw.z=q0*a.z+q1*b.z+q2*c.z;
            if(ob->smooth_shading){
                RMBVec3 na=g_vnormal[t->v[0]],nb=g_vnormal[t->v[1]],
                        nc=g_vnormal[t->v[2]];
                pn.x=q0*na.x+q1*nb.x+q2*nc.x;
                pn.y=q0*na.y+q1*nb.y+q2*nc.y;
                pn.z=q0*na.z+q1*nb.z+q2*nc.z;
                /* A welded vertex on a hard crease can average to nearly zero;
                 * fall back to the face normal so the pixel is never random. */
                if(vdot(pn,pn)<1e-18)pn=n;
            }else pn=n;
            vis=1.0;open=1.0;
            if(ob->static_light){
                vis=q0*(double)g_vlight[t->v[0]]+q1*(double)g_vlight[t->v[1]]+
                    q2*(double)g_vlight[t->v[2]];
                open=q0*(double)g_vao[t->v[0]]+q1*(double)g_vao[t->v[1]]+
                     q2*(double)g_vao[t->v[2]];
            }
            cr=0.0;
            if(ob->crease_coverage>0.0){
                cr=q0*(double)g_vcrease[t->v[0]]+
                   q1*(double)g_vcrease[t->v[1]]+
                   q2*(double)g_vcrease[t->v[2]];
                if(cr<0.0)cr=0.0;
                if(cr>1.0)cr=1.0;
            }
            recess=(uint8_t)(cr*255.0);
            level=ramp_quantize(surface_brightness(pn,pw,light,vis,open,cr,ob),
                                t->shade_bias,ob->ramp_levels,ob,t->object_id,
                                x,y);
            /*
             * The incident angle already lives in the ambient ramp index, so
             * the lit bit carries ONLY cast-shadow visibility, which nothing
             * currently projects onto the hero. Marking every pixel lit would
             * add +2 ramp stops everywhere and flatten the angular information
             * straight back out -- that was measured, not assumed.
             */
            tsp_host_composite_pixel_ramp((uint8_t)x,(uint8_t)y,owner,level,0u,
                                          0u,recess,d);
        }else if(s->objects[t->object_id].overlay_target_object!=0xffu){
            static const uint8_t bayer2[4]={0u,2u,3u,1u};
            uint8_t q=s->objects[t->object_id].overlay_dither_quarters;
            uint8_t threshold=bayer2[((uint8_t)y&1u)*2u+((uint8_t)x&1u)];
            uint8_t target;
            if(q<4u&&threshold>=q)continue;
            target=(uint8_t)(0x80u+
                (s->objects[t->object_id].overlay_target_object&0x3fu));
            tsp_host_composite_pixel_overlay_depth((uint8_t)x,(uint8_t)y,
                                                    target,shade,0u,d);
        }else{
            tsp_host_composite_pixel_depth((uint8_t)x,(uint8_t)y,owner,shade,0u,d);
        }
    }
}

static void draw_silhouette_edge(const RMBScene *s,const RMBEdge *e,
                                 double cx,double cy,double cz,double yaw,
                                 uint8_t owner){
    RMBVec3 a=s->vertices[e->a],b=s->vertices[e->b];
    Proj pa=project(a,cx,cy,cz,yaw),pb=project(b,cx,cy,cz,yaw);
    double dx,dy,steps;
    int i,n;
    if(!pa.ok||!pb.ok)return;
    dx=pb.x-pa.x;dy=pb.y-pa.y;
    steps=fmax(fabs(dx),fabs(dy));n=(int)ceil(steps);
    if(n<1)n=1;
    for(i=0;i<=n;++i){
        double q=(double)i/(double)n;
        double sx=pa.x+dx*q,sy=pa.y+dy*q;
        double inv=pa.inv+(pb.inv-pa.inv)*q;
        double d;
        int ix=(int)floor(sx+0.5),iy=(int)floor(sy+0.5);
        if(ix<0||ix>159||iy<0||iy>143||inv<=1e-12)continue;
        d=1.0/inv-0.025;
        if(d<=0.0)d=0.001;
        tsp_host_composite_pixel_depth((uint8_t)ix,(uint8_t)iy,owner,0u,1u,d);
    }
}

static int segment_aabb_hit(const RMBScene *s,RMBVec3 o,RMBVec3 d){
    double t0=0.0,t1=1.0;
    int axis;
    const double omin[3]={s->bounds_min.x,s->bounds_min.y,s->bounds_min.z};
    const double omax[3]={s->bounds_max.x,s->bounds_max.y,s->bounds_max.z};
    const double ov[3]={o.x,o.y,o.z};
    const double dv[3]={d.x,d.y,d.z};
    if(!s->bounds_valid)return 0;
    for(axis=0;axis<3;++axis){
        if(fabs(dv[axis])<1e-12){
            if(ov[axis]<omin[axis]||ov[axis]>omax[axis])return 0;
        }else{
            double a=(omin[axis]-ov[axis])/dv[axis];
            double b=(omax[axis]-ov[axis])/dv[axis];
            if(a>b){double q=a;a=b;b=q;}
            if(a>t0)t0=a;
            if(b<t1)t1=b;
            if(t0>t1)return 0;
        }
    }
    return t1>1e-7&&t0<1.0-1e-7;
}

static int segment_triangle_hit(RMBVec3 o,RMBVec3 d,
                                RMBVec3 a,RMBVec3 b,RMBVec3 c){
    RMBVec3 e1=vsub(b,a),e2=vsub(c,a),p=vcross(d,e2);
    double det=vdot(e1,p),inv,u,v,t;
    RMBVec3 q,tv;
    if(fabs(det)<1e-12)return 0;
    inv=1.0/det;
    tv=vsub(o,a);
    u=vdot(tv,p)*inv;
    if(u<-1e-8||u>1.0+1e-8)return 0;
    q=vcross(tv,e1);
    v=vdot(d,q)*inv;
    if(v<-1e-8||u+v>1.0+1e-8)return 0;
    t=vdot(e2,q)*inv;
    return t>1e-6&&t<1.0-1e-6;
}

int rmb_segment_occluded(const RMBScene *s,
                         double lx,double ly,double lz,
                         double wx,double wy,double wz){
    RMBVec3 o={lx,ly,lz},d={wx-lx,wy-ly,wz-lz};
    uint16_t i;
    if(!s||!s->triangle_count||!segment_aabb_hit(s,o,d))return 0;
    for(i=0u;i<s->triangle_count;++i){
        const RMBTriangle *t=&s->triangles[i];
        if(!s->objects[t->object_id].casts_shadow)continue;
        if(segment_triangle_hit(o,d,
                                s->vertices[t->v[0]],
                                s->vertices[t->v[1]],
                                s->vertices[t->v[2]]))
            return 1;
    }
    return 0;
}

void rmb_render(const RMBScene *s,double cx,double cy,double cz,
                uint8_t yaw8,const RMBLight *light){
    uint16_t i;
    uint8_t o;
    double yaw=(double)yaw8*(2.0*RMB_PI/256.0);
    RMBVec3 cam={cx,cy,cz};
    uint8_t front[RMB_MAX_TRIANGLES];

    ensure_vertex_normals(s);
    ensure_static_lighting(s,light);

    for(i=0u;i<s->triangle_count;++i){
        const RMBTriangle *t=&s->triangles[i];
        uint8_t owner=(uint8_t)(0x80u+(t->object_id&0x3fu));
        front[i]=0u;
        if(!s->objects[t->object_id].visible)continue;
        front[i]=tri_front(s,t,cam);
        if(front[i])raster_triangle(s,t,cx,cy,cz,yaw,light,owner);
    }

    for(i=0u;i<s->edge_count;++i){
        const RMBEdge *e=&s->edges[i];
        uint8_t mode=s->objects[e->object_id].outline_mode;
        uint8_t silhouette=0u,crease=0u,owner;
        if(!s->objects[e->object_id].visible||mode==RMB_OUTLINE_NONE)continue;
        if(e->tri0>=0){
            uint8_t f0=front[(uint16_t)e->tri0];
            if(e->tri1<0)silhouette=f0;
            else{
                uint8_t f1=front[(uint16_t)e->tri1];
                silhouette=(uint8_t)(f0!=f1);
                if(mode==RMB_OUTLINE_SILHOUETTE_CREASE&&f0&&f1){
                    const RMBTriangle *a=&s->triangles[(uint16_t)e->tri0];
                    const RMBTriangle *b=&s->triangles[(uint16_t)e->tri1];
                    RMBVec3 an=vcross(vsub(s->vertices[a->v[1]],s->vertices[a->v[0]]),
                                      vsub(s->vertices[a->v[2]],s->vertices[a->v[0]]));
                    RMBVec3 bn=vcross(vsub(s->vertices[b->v[1]],s->vertices[b->v[0]]),
                                      vsub(s->vertices[b->v[2]],s->vertices[b->v[0]]));
                    double d=vdot(vnorm(an),vnorm(bn));
                    if(d<0.64)crease=1u;
                }
            }
        }
        if(!silhouette&&!crease)continue;
        owner=(uint8_t)(0x80u+(e->object_id&0x3fu));
        draw_silhouette_edge(s,e,cx,cy,cz,yaw,owner);
    }

    /* Consolidation runs last so it sees the object's final composited shade
     * image, including any clipped overlays drawn onto it. */
    for(o=0u;o<s->object_count;++o){
        if(!s->objects[o].visible)continue;
        if(!s->objects[o].consolidate_support)continue;
        tsp_host_composite_consolidate_owner((uint8_t)(0x80u+(o&0x3fu)),
                                             s->objects[o].consolidate_support,
                                             s->objects[o].consolidate_passes);
    }

}
