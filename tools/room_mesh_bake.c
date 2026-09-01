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
        (void)add_vertex(s,apply_xf(xf,p));
    }
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
        if(s->objects[t->object_id].overlay_target_object!=0xffu){
            uint8_t target=(uint8_t)(0x80u+
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
    double yaw=(double)yaw8*(2.0*RMB_PI/256.0);
    RMBVec3 cam={cx,cy,cz};
    uint8_t front[RMB_MAX_TRIANGLES];

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
}
