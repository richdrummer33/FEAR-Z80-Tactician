#include <math.h>
#include <stddef.h>
#include <stdio.h>
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
    s->objects[id].family_supplied=0u;
    s->objects[id].ground_contact=0u;
    s->objects[id].ground_contact_z=0.0;
    s->objects[id].ground_contact_radius=0.0;
    s->objects[id].static_light=0u;
    s->objects[id].incident_weight=1.0;
    s->objects[id].ao_radius=0.0;
    s->objects[id].ao_strength=0.0;
    s->objects[id].light_radius=0.0;
    s->objects[id].shadow_floor=1.0;
    s->objects[id].equalize=0u;
    s->objects[id].highlight_fraction=0.0;
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

void rmb_set_object_ramp_highlight_fraction(RMBScene *s,uint8_t object_id,
                                            double fraction){
    if(object_id>=s->object_count)rmb_fail("invalid highlight-fraction object id");
    if(fraction<0.0||fraction>=1.0)
        rmb_fail("highlight fraction must be in [0,1)");
    s->objects[object_id].highlight_fraction=fraction;
}

void rmb_set_object_incident_weight(RMBScene *s,uint8_t object_id,double w){
    if(object_id>=s->object_count)rmb_fail("invalid incident-weight object id");
    if(w<0.0||w>1.0)rmb_fail("incident weight must be 0..1");
    s->objects[object_id].incident_weight=w;
}

void rmb_set_object_ground_contact(RMBScene *s,uint8_t object_id,
                                   uint8_t enabled,double ground_z,
                                   double reach){
    if(object_id>=s->object_count)rmb_fail("invalid ground-contact object id");
    if(enabled&&!(reach>0.0))rmb_fail("ground-contact reach must be positive");
    s->objects[object_id].ground_contact=(uint8_t)(enabled?1u:0u);
    s->objects[object_id].ground_contact_z=ground_z;
    s->objects[object_id].ground_contact_radius=reach;
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
    rmb_add_indexed_mesh_q8_family(s,obj,xf,xyz_q8,vertex_count,indices,
                                   triangle_count,bias,vertex_recess,NULL);
}

void rmb_add_indexed_mesh_q8_family(RMBScene *s,uint8_t obj,
                                    const RMBTransform *xf,
                                    const int16_t *xyz_q8,uint16_t vertex_count,
                                    const uint16_t *indices,
                                    uint16_t triangle_count,int8_t bias,
                                    const uint8_t *vertex_recess,
                                    const uint8_t *vertex_family){
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
        s->vertex_family[v]=vertex_family?vertex_family[i]:0u;
    }
    if(vertex_recess)s->objects[obj].recess_supplied=1u;
    if(vertex_family)s->objects[obj].family_supplied=1u;
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
                    RMBVec3 u=hemisphere_dir(k,RMB_AO_RAYS,n,tx,ty);
                    RMBVec3 d;
                    d.x=u.x*ob->ao_radius;
                    d.y=u.y*ob->ao_radius;
                    d.z=u.z*ob->ao_radius;
                    if(bake_segment_hit(s,o,d))continue;
                    /*
                     * The ground counts as an occluder, and it is not in the
                     * mesh -- the room's floor is drawn by the segment
                     * renderer, so an AO probe against the triangle soup alone
                     * never sees it and the statue's lowest surfaces come out
                     * exactly as bright as its highest. That is the hard join
                     * where the figure meets the floor.
                     *
                     * d already carries the AO radius, so t in (0,1] is
                     * "within reach", and that finite reach is the whole
                     * effect: an infinite plane subtends the lower hemisphere
                     * from ANY height and would darken every vertex equally.
                     * What makes an object look planted is that the floor is
                     * only close enough to matter near the bottom.
                     */
                    if(ob->ground_contact&&u.z<-1e-12){
                        /* Distance to the ground along the UNIT direction, so
                         * the contact reach is independent of ao_radius. */
                        double t=(ob->ground_contact_z-o.z)/u.z;
                        if(t>1e-6&&t<=ob->ground_contact_radius)continue;
                    }
                    ++open;
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
                    uint32_t idx;
                    if(ob->highlight_fraction>0.0 &&
                       k+1u<ob->ramp_levels){
                        /* Reserve only highlight_fraction for the top stop.
                         * The remaining probability mass is spread evenly
                         * across the lower stops. This is deliberately a
                         * quantile policy rather than an exposure multiplier:
                         * it still works when equalization is enabled. */
                        double low_mass=1.0-ob->highlight_fraction;
                        double q=low_mass*(double)(k+1u)/
                                 (double)(ob->ramp_levels-1u);
                        idx=(uint32_t)floor(q*(double)bn);
                    }else{
                        idx=(uint32_t)(((uint64_t)(k+1u)*bn)/
                                       ob->ramp_levels);
                    }
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
            uint8_t level,recess,family=0u;
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
            /* Material family is CATEGORICAL: interpolating it would produce
             * indices for materials that are not on this triangle at all, and
             * on a boundary triangle every interior pixel would name a
             * material the surface does not have. Take the corner with the
             * largest perspective-correct weight -- nearest-vertex, which is
             * the only defensible resampling of a label. */
            if(ob->family_supplied){
                if(q0>=q1&&q0>=q2)family=s->vertex_family[t->v[0]];
                else if(q1>=q2)family=s->vertex_family[t->v[1]];
                else family=s->vertex_family[t->v[2]];
            }
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
            tsp_host_composite_pixel_ramp_family((uint8_t)x,(uint8_t)y,owner,
                                                 level,0u,0u,recess,family,d);
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

int rmb_segment_occluded_exact(const RMBScene *s,
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

/*
 * The same question, answered from the shadow map when one covers this light.
 * Kept as a wrapper rather than replacing the exact test outright: the exact
 * one is what the map is validated against, and it is still the answer for any
 * light the map was not built for.
 */
int rmb_segment_occluded(const RMBScene *s,
                         double lx,double ly,double lz,
                         double wx,double wy,double wz){
    if(rmb_shadow_map_ready(s,lx,ly,lz))
        return rmb_shadow_coverage(s,lx,ly,lz,wx,wy,wz,0.0)<128u;
    return rmb_segment_occluded_exact(s,lx,ly,lz,wx,wy,wz);
}


/* ---------------------------------------------------------------------------
 * Silhouette shadow map. See room_mesh_bake.h for why this exists.
 * ------------------------------------------------------------------------ */
#define RMB_SM_MAX_DIM 2048u
/* Rasterisation over-coverage, in map pixels. See sm_raster_triangle. */
/*
 * Conservative-rasterisation over-coverage, in map pixels.
 *
 * Half a pixel diagonal (0.7072) is the value that PROVABLY covers the centre
 * of every pixel a triangle overlaps. Measured against the exact ray cast at
 * 1024, the smaller 0.50 already reaches zero missed receivers on both casters
 * and over-covers noticeably less:
 *
 *   dilation  proxy missed/extra   visual-mesh missed/extra
 *   0.00           7 / 2                  10 / 13
 *   0.15           1 / 9                   5 / 25
 *   0.35           1 / 16                  2 / 50
 *   0.50           0 / 21                  0 / 91
 *   0.7072         0 / 32                  0 / 146
 *
 * Zero missed is the property worth buying: a hole inside a shadow is a lit
 * speckle and reads as noise, while over-covering by well under one texel
 * widens the silhouette by around 0.03 world units, which no part of this
 * renderer can resolve. Both directions of error shrink with resolution.
 */
#define SM_DILATE 0.50

static float *g_sm_depth;               /* distance from the light, +inf empty */
static uint16_t g_sm_dim;
static const RMBScene *g_sm_scene;
static double g_sm_lx,g_sm_ly,g_sm_lz;
static RMBVec3 g_sm_f,g_sm_r,g_sm_u;    /* light-space basis */
static double g_sm_tan_x,g_sm_tan_y;    /* half-extents at unit forward depth */
static double g_sm_near;
static uint8_t g_sm_valid;
static uint32_t g_sm_skipped_tris;

void rmb_shadow_map_reset(void){
    free(g_sm_depth);
    g_sm_depth=(float *)0;
    g_sm_dim=0u;
    g_sm_scene=(const RMBScene *)0;
    g_sm_valid=0u;
    g_sm_skipped_tris=0u;
}

int rmb_shadow_map_ready(const RMBScene *s,double lx,double ly,double lz){
    return g_sm_valid&&g_sm_scene==s&&
           g_sm_lx==lx&&g_sm_ly==ly&&g_sm_lz==lz;
}

/* Light-space coordinates of a world point: forward distance plus the two
 * transverse offsets, before the division that makes them a screen position. */
static void sm_light_space(double wx,double wy,double wz,
                           double *fz,double *rr,double *uu){
    double dx=wx-g_sm_lx,dy=wy-g_sm_ly,dz=wz-g_sm_lz;
    *fz=dx*g_sm_f.x+dy*g_sm_f.y+dz*g_sm_f.z;
    *rr=dx*g_sm_r.x+dy*g_sm_r.y+dz*g_sm_r.z;
    *uu=dx*g_sm_u.x+dy*g_sm_u.y+dz*g_sm_u.z;
}

/* Project to fractional map pixels. Returns 0 when the point is behind the
 * near plane, where the projection is meaningless. */
static int sm_project(double wx,double wy,double wz,
                      double *px,double *py,double *fz_out){
    double fz,rr,uu;
    sm_light_space(wx,wy,wz,&fz,&rr,&uu);
    if(fz<=g_sm_near)return 0;
    *px=(rr/(fz*g_sm_tan_x)*0.5+0.5)*(double)g_sm_dim;
    *py=(uu/(fz*g_sm_tan_y)*0.5+0.5)*(double)g_sm_dim;
    *fz_out=fz;
    return 1;
}

static void sm_store(int x,int y,double fz){
    float *slot;
    if(x<0||y<0||x>=(int)g_sm_dim||y>=(int)g_sm_dim)return;
    slot=&g_sm_depth[(size_t)y*g_sm_dim+(size_t)x];
    if((float)fz<*slot)*slot=(float)fz;
}

/*
 * Rasterise one triangle into the depth map.
 *
 * Depth is interpolated as 1/fz, which is the quantity that is linear in
 * screen space under a perspective projection. Interpolating fz directly is
 * the classic error and it bows the stored surface toward the light in the
 * middle of every large triangle -- which on a floor-length shadow shows up as
 * a bite taken out of the middle of the silhouette.
 */
static void sm_raster_triangle(RMBVec3 a,RMBVec3 b,RMBVec3 c){
    double pax,pay,paz,pbx,pby,pbz,pcx,pcy,pcz;
    double minx,maxx,miny,maxy,area;
    double nx[3],ny[3],dmin,dmax;
    int x0,x1,y0,y1,x,y;
    if(!sm_project(a.x,a.y,a.z,&pax,&pay,&paz)||
       !sm_project(b.x,b.y,b.z,&pbx,&pby,&pbz)||
       !sm_project(c.x,c.y,c.z,&pcx,&pcy,&pcz)){
        /* Straddles the light's near plane. Counted rather than silently
         * dropped: a non-zero total means the map is not authoritative and the
         * caller must keep ray casting. */
        ++g_sm_skipped_tris;
        return;
    }
    area=(pbx-pax)*(pcy-pay)-(pby-pay)*(pcx-pax);
    if(fabs(area)<1e-12)return;
    if(area<0.0){
        /*
         * Back-facing in light space. Swap two corners so the winding is
         * positive and the inside test below works, rather than dropping the
         * triangle.
         *
         * Dropping it is the tempting shortcut -- for a closed manifold the
         * front faces alone define the silhouette exactly -- but these are
         * decimated shells with no guarantee of being closed or consistently
         * wound, so a discarded back face can be the only surface covering
         * that part of the map. Keeping both costs nothing: the buffer keeps
         * the minimum, so a back face behind a front face never wins.
         */
        double tx=pbx,ty=pby,tz=pbz;
        pbx=pcx;pby=pcy;pbz=pcz;
        pcx=tx;pcy=ty;pcz=tz;
        area=-area;
    }

    /*
     * Conservative rasterisation: push each EDGE outward along its own normal
     * by half a pixel diagonal, then re-derive the corners as the
     * intersections of the pushed edges.
     *
     * Pixel-centre sampling loses any triangle thinner than a pixel, and the
     * triangles that go thin here are not an edge case -- they are the ones
     * that matter most. The statue's base is almost edge-on to a light that
     * sits barely above the floor, so the whole contact region projects into
     * light space as slivers a fraction of a texel wide. Dropping them punches
     * lit holes exactly where the object most needs to look planted, and the
     * holes radiate outward from the base because that is the direction the
     * projection magnifies.
     *
     * Expanding from the centroid instead -- the obvious cheap version -- does
     * not work: on a sliver the centroid lies on the sliver, so that offset
     * lengthens it without widening it at all. The offset has to be per EDGE,
     * along that edge's own normal.
     *
     * Worth about seven missed receivers in ten thousand at 1024 (see
     * SM_DILATE). Small, and one-sided in the direction that matters.
     */
    {
        double vx[3],vy[3],vz[3],ox[3],oy[3];
        int e;
        vx[0]=pax;vy[0]=pay;vz[0]=paz;
        vx[1]=pbx;vy[1]=pby;vz[1]=pbz;
        vx[2]=pcx;vy[2]=pcy;vz[2]=pcz;
        /* Outward normal offsets, one per edge i -> i+1. Winding is positive
         * by now, so the interior is left of each directed edge. */
        for(e=0;e<3;++e){
            double dx=vx[(e+1)%3]-vx[e],dy=vy[(e+1)%3]-vy[e];
            double l=sqrt(dx*dx+dy*dy);
            if(l<1e-12){ox[e]=0.0;oy[e]=0.0;continue;}
            ox[e]=dy/l*SM_DILATE;
            oy[e]=-dx/l*SM_DILATE;
        }
        for(e=0;e<3;++e){
            /* Corner e joins edge e-1 and edge e. */
            int p=(e+2)%3;
            double a1x=vx[p]+ox[p],a1y=vy[p]+oy[p];
            double d1x=vx[e]-vx[p],d1y=vy[e]-vy[p];
            double a2x=vx[e]+ox[e],a2y=vy[e]+oy[e];
            double d2x=vx[(e+1)%3]-vx[e],d2y=vy[(e+1)%3]-vy[e];
            double den=d1x*d2y-d1y*d2x,t;
            if(fabs(den)<1e-9){
                /* Edges effectively parallel: no well-defined intersection, so
                 * take the averaged offset rather than a point at infinity. */
                nx[e]=vx[e]+(ox[p]+ox[e])*0.5;
                ny[e]=vy[e]+(oy[p]+oy[e])*0.5;
                continue;
            }
            t=((a2x-a1x)*d2y-(a2y-a1y)*d2x)/den;
            nx[e]=a1x+d1x*t;
            ny[e]=a1y+d1y*t;
        }
        pax=nx[0];pay=ny[0];
        pbx=nx[1];pby=ny[1];
        pcx=nx[2];pcy=ny[2];
        /* Depths stay the originals; the expansion is a coverage device, not a
         * change of geometry. Interpolating to an expanded corner extrapolates
         * beyond the real triangle, so the result is clamped to the triangle's
         * own depth range below. */
        dmin=vz[0]<vz[1]?(vz[0]<vz[2]?vz[0]:vz[2]):(vz[1]<vz[2]?vz[1]:vz[2]);
        dmax=vz[0]>vz[1]?(vz[0]>vz[2]?vz[0]:vz[2]):(vz[1]>vz[2]?vz[1]:vz[2]);
        area=(pbx-pax)*(pcy-pay)-(pby-pay)*(pcx-pax);
        if(fabs(area)<1e-12)return;
    }

    minx=pax<pbx?(pax<pcx?pax:pcx):(pbx<pcx?pbx:pcx);
    maxx=pax>pbx?(pax>pcx?pax:pcx):(pbx>pcx?pbx:pcx);
    miny=pay<pby?(pay<pcy?pay:pcy):(pby<pcy?pby:pcy);
    maxy=pay>pby?(pay>pcy?pay:pcy):(pby>pcy?pby:pcy);
    x0=(int)floor(minx); x1=(int)ceil(maxx);
    y0=(int)floor(miny); y1=(int)ceil(maxy);
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>(int)g_sm_dim-1)x1=(int)g_sm_dim-1;
    if(y1>(int)g_sm_dim-1)y1=(int)g_sm_dim-1;
    if(x0>x1||y0>y1)return;

    for(y=y0;y<=y1;++y)for(x=x0;x<=x1;++x){
        double sx=(double)x+0.5,sy=(double)y+0.5;
        /* Edge functions, all three in the same orientation:
         * cross(edge, sample - edge start). Mixing the operand order on one of
         * them negates that barycentric, and the inside test then accepts a
         * half-plane the triangle does not occupy -- which does not blank the
         * map, it fills it with neighbouring triangles' wrong halves and
         * leaves structured holes that look like an aliasing problem. */
        double w0=((pbx-pax)*(sy-pay)-(pby-pay)*(sx-pax))/area;
        double w1=((pcx-pbx)*(sy-pby)-(pcy-pby)*(sx-pbx))/area;
        double w2,inv;
        if(w0<-1e-9||w1<-1e-9)continue;
        w2=1.0-w0-w1;
        if(w2<-1e-9)continue;
        /* w1,w2,w0 weight a,b,c respectively for this edge ordering. */
        inv=w1/paz+w2/pbz+w0/pcz;
        if(inv<=1e-12)continue;
        {
            double d=1.0/inv;
            if(d<dmin)d=dmin;
            if(d>dmax)d=dmax;
            sm_store(x,y,d);
        }
    }
}

int rmb_shadow_map_build(const RMBScene *s,double lx,double ly,double lz,
                         uint16_t resolution){
    RMBVec3 centre,d;
    double bmin[3],bmax[3];
    double len,maxr=0.0,maxu=0.0,minf=1e30;
    uint16_t i;
    uint8_t any=0u;
    int corner;
    size_t n;

    if(rmb_shadow_map_ready(s,lx,ly,lz)&&g_sm_dim==resolution)return 1;
    rmb_shadow_map_reset();
    if(!s||!s->triangle_count)return 0;
    if(resolution<64u)resolution=64u;
    if(resolution>RMB_SM_MAX_DIM)resolution=RMB_SM_MAX_DIM;

    /* Bounds of the shadow-casting geometry only. Sizing the frustum to the
     * whole scene would spend most of the map's resolution on the room. */
    bmin[0]=bmin[1]=bmin[2]=1e30;
    bmax[0]=bmax[1]=bmax[2]=-1e30;
    for(i=0u;i<s->triangle_count;++i){
        const RMBTriangle *t=&s->triangles[i];
        uint8_t k;
        if(!s->objects[t->object_id].casts_shadow)continue;
        for(k=0u;k<3u;++k){
            RMBVec3 p=s->vertices[t->v[k]];
            double q[3];
            q[0]=p.x;q[1]=p.y;q[2]=p.z;
            for(corner=0;corner<3;++corner){
                if(q[corner]<bmin[corner])bmin[corner]=q[corner];
                if(q[corner]>bmax[corner])bmax[corner]=q[corner];
            }
        }
        any=1u;
    }
    if(!any)return 0;

    centre.x=(bmin[0]+bmax[0])*0.5;
    centre.y=(bmin[1]+bmax[1])*0.5;
    centre.z=(bmin[2]+bmax[2])*0.5;
    d.x=centre.x-lx; d.y=centre.y-ly; d.z=centre.z-lz;
    len=sqrt(vdot(d,d));
    if(len<1e-9)return 0;              /* light inside the caster */
    g_sm_f.x=d.x/len; g_sm_f.y=d.y/len; g_sm_f.z=d.z/len;
    basis_from_normal(g_sm_f,&g_sm_r,&g_sm_u);

    g_sm_lx=lx; g_sm_ly=ly; g_sm_lz=lz;
    /* Half-angles that just contain the caster's eight corners, then a margin
     * so a bilinear/PCF tap at the very edge still reads real data. */
    for(corner=0;corner<8;++corner){
        double px=(corner&1)?bmax[0]:bmin[0];
        double py=(corner&2)?bmax[1]:bmin[1];
        double pz=(corner&4)?bmax[2]:bmin[2];
        double fz,rr,uu;
        sm_light_space(px,py,pz,&fz,&rr,&uu);
        if(fz<minf)minf=fz;
        if(fz<=1e-6)return 0;          /* caster straddles the light */
        if(fabs(rr)/fz>maxr)maxr=fabs(rr)/fz;
        if(fabs(uu)/fz>maxu)maxu=fabs(uu)/fz;
    }
    g_sm_tan_x=maxr*1.08+1e-3;
    g_sm_tan_y=maxu*1.08+1e-3;
    g_sm_near=minf*0.5;

    g_sm_dim=resolution;
    n=(size_t)g_sm_dim*(size_t)g_sm_dim;
    g_sm_depth=(float *)malloc(n*sizeof(float));
    if(!g_sm_depth){g_sm_dim=0u;return 0;}
    for(i=0u;i<g_sm_dim;++i){
        size_t j;
        for(j=0u;j<g_sm_dim;++j)g_sm_depth[(size_t)i*g_sm_dim+j]=1e30f;
    }

    g_sm_scene=s;
    g_sm_valid=1u;
    g_sm_skipped_tris=0u;
    for(i=0u;i<s->triangle_count;++i){
        const RMBTriangle *t=&s->triangles[i];
        if(!s->objects[t->object_id].casts_shadow)continue;
        sm_raster_triangle(s->vertices[t->v[0]],
                           s->vertices[t->v[1]],
                           s->vertices[t->v[2]]);
    }
    if(g_sm_skipped_tris){
        /* Incomplete map: refuse it rather than cast a shadow with holes. */
        rmb_shadow_map_reset();
        return 0;
    }
    return 1;
}

/* Nearest blocker depth at a fractional map position, or 0 when the tap is
 * outside the map or empty. */
static double sm_tap(double px,double py){
    int x=(int)floor(px),y=(int)floor(py);
    float v;
    if(x<0||y<0||x>=(int)g_sm_dim||y>=(int)g_sm_dim)return 0.0;
    v=g_sm_depth[(size_t)y*g_sm_dim+(size_t)x];
    return v>1e29f?0.0:(double)v;
}

uint8_t rmb_shadow_coverage(const RMBScene *s,
                            double lx,double ly,double lz,
                            double wx,double wy,double wz,
                            double source_radius){
    double px,py,fz,bias;
    if(!rmb_shadow_map_ready(s,lx,ly,lz))
        return rmb_segment_occluded_exact(s,lx,ly,lz,wx,wy,wz)?0u:255u;
    if(!sm_project(wx,wy,wz,&px,&py,&fz))return 255u;
    /* Scaled with depth: a fixed world bias is either useless close to the
     * light or a visible shadow gap far from it. */
    bias=fz*2e-3+1e-3;

    if(!(source_radius>0.0)){
        double b=sm_tap(px,py);
        return (b>0.0&&b+bias<fz)?0u:255u;
    }
    {
        /*
         * Percentage-closer soft shadows.
         *
         * Two passes: find how far in front of the receiver the blockers
         * actually are, then filter over a kernel sized from that distance.
         * The penumbra a disc source casts widens with the blocker-to-receiver
         * gap, so this is not a stylistic blur -- it is why a shadow is crisp
         * where the object touches the ground and diffuse where it is thrown
         * across the room, which is the cue that reads as "standing on".
         */
        double search,sum=0.0,pen;
        int count=0,k,taps=0,lit=0;
        double avg;
        /* Blocker search radius in map pixels: the source's angular size at
         * the receiver, expressed in the map's own projection. */
        search=(source_radius/(fz*g_sm_tan_x))*0.5*(double)g_sm_dim;
        if(search<1.0)search=1.0;
        if(search>64.0)search=64.0;
        for(k=0;k<16;++k){
            double a=(double)k*2.399963229728653;
            double rr=search*sqrt(((double)k+0.5)/16.0);
            double b=sm_tap(px+cos(a)*rr,py+sin(a)*rr);
            if(b>0.0&&b+bias<fz){sum+=b;++count;}
        }
        if(!count)return 255u;
        avg=sum/(double)count;
        /* Penumbra half-width at the receiver, projected back into map
         * pixels. (fz - avg)/avg is the similar-triangles ratio. */
        pen=(source_radius*(fz-avg)/avg);
        pen=(pen/(fz*g_sm_tan_x))*0.5*(double)g_sm_dim;
        if(pen<0.5)pen=0.5;
        if(pen>96.0)pen=96.0;
        for(k=0;k<24;++k){
            double a=(double)k*2.399963229728653;
            double rr=pen*sqrt(((double)k+0.5)/24.0);
            double b=sm_tap(px+cos(a)*rr,py+sin(a)*rr);
            ++taps;
            if(!(b>0.0&&b+bias<fz))++lit;
        }
        return (uint8_t)((lit*255)/taps);
    }
}


/* ---------------------------------------------------------------------------
 * Ground-contact occlusion, seen from the ground
 *
 * The companion to the per-vertex ground_contact term: that one darkens the
 * OBJECT where it approaches the floor, this one darkens the FLOOR where it
 * approaches the object. Both are the same physical statement -- a
 * finite-reach ambient probe finds less open sky inside a corner -- applied to
 * the two surfaces that form the corner, and they have to agree or the join
 * reads as two unrelated smudges rather than as one contact.
 *
 * Deliberately independent of the light. The cast shadow only anchors the
 * object on the side the light happens to throw it; from any other angle the
 * figure still floats. This term is there from every angle because ambient
 * occlusion does not have a direction.
 */
uint8_t rmb_ground_contact_openness(const RMBScene *s,
                                    double wx,double wy,double wz,
                                    double radius){
    /* Caster bounds, cached across calls.
     *
     * This function is asked about every cell of a floor-wide grid, and the
     * overwhelming majority of them are nowhere near the object -- the bounds
     * test below rejects them immediately. Recomputing the bounds to perform
     * that rejection made the rejection cost a full pass over the triangle
     * list, so the cheap path was the expensive one and the whole grid took
     * tens of seconds. */
    static const RMBScene *cache_scene;
    static uint16_t cache_tris;
    static double lo[3],hi[3];
    static uint8_t cache_ok;
    RMBVec3 n={0.0,0.0,1.0},o,tx,ty,d;
    uint16_t i;
    uint8_t k,open=0u;
    if(!s||!s->triangle_count||!(radius>0.0))return 255u;

    if(cache_scene!=s||cache_tris!=s->triangle_count){
        cache_scene=s;
        cache_tris=s->triangle_count;
        lo[0]=lo[1]=lo[2]=1e30;
        hi[0]=hi[1]=hi[2]=-1e30;
        for(i=0u;i<s->triangle_count;++i){
            const RMBTriangle *t=&s->triangles[i];
            uint8_t c;
            if(!s->objects[t->object_id].casts_shadow)continue;
            for(c=0u;c<3u;++c){
                RMBVec3 p=s->vertices[t->v[c]];
                double q[3];
                uint8_t a;
                q[0]=p.x;q[1]=p.y;q[2]=p.z;
                for(a=0u;a<3u;++a){
                    if(q[a]<lo[a])lo[a]=q[a];
                    if(q[a]>hi[a])hi[a]=q[a];
                }
            }
        }
        cache_ok=(uint8_t)(lo[0]<=hi[0]);
    }
    if(!cache_ok)return 255u;
    if(wx<lo[0]-radius||wx>hi[0]+radius||
       wy<lo[1]-radius||wy>hi[1]+radius||
       wz<lo[2]-radius||wz>hi[2]+radius)return 255u;

    o.x=wx;o.y=wy;o.z=wz+1e-3;
    basis_from_normal(n,&tx,&ty);
    for(k=0u;k<RMB_AO_RAYS;++k){
        uint8_t blocked=0u;
        d=hemisphere_dir(k,RMB_AO_RAYS,n,tx,ty);
        d.x*=radius;d.y*=radius;d.z*=radius;
        for(i=0u;i<s->triangle_count;++i){
            const RMBTriangle *t=&s->triangles[i];
            if(!s->objects[t->object_id].casts_shadow)continue;
            if(segment_triangle_hit(o,d,s->vertices[t->v[0]],
                                    s->vertices[t->v[1]],s->vertices[t->v[2]])){
                blocked=1u;
                break;
            }
        }
        if(!blocked)++open;
    }
    return (uint8_t)(((uint16_t)open*255u)/RMB_AO_RAYS);
}

int rmb_shadow_map_write_pgm(const char *path){
    FILE *f;
    size_t i,n;
    double lo=1e30,hi=-1e30;
    if(!g_sm_valid||!g_sm_depth)return 0;
    n=(size_t)g_sm_dim*(size_t)g_sm_dim;
    for(i=0u;i<n;++i){
        double v=(double)g_sm_depth[i];
        if(v>1e29)continue;
        if(v<lo)lo=v;
        if(v>hi)hi=v;
    }
    if(lo>hi){lo=0.0;hi=1.0;}
    if(hi-lo<1e-9)hi=lo+1.0;
    f=fopen(path,"wb");
    if(!f)return 0;
    fprintf(f,"P5\n%u %u\n255\n",(unsigned)g_sm_dim,(unsigned)g_sm_dim);
    for(i=0u;i<n;++i){
        double v=(double)g_sm_depth[i];
        int q=v>1e29?0:(int)(255.0-(v-lo)/(hi-lo)*200.0);
        if(q<0)q=0;
        if(q>255)q=255;
        fputc(q,f);
    }
    fclose(f);
    return 1;
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
