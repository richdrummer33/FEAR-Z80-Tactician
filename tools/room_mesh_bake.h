#ifndef ROOM_MESH_BAKE_H
#define ROOM_MESH_BAKE_H

#include <stdint.h>

#define RMB_MAX_VERTICES 8192u
#define RMB_MAX_TRIANGLES 12288u
#define RMB_MAX_EDGES 4096u
#define RMB_MAX_OBJECTS 32u

enum {
    RMB_OUTLINE_NONE = 0u,
    RMB_OUTLINE_SILHOUETTE = 1u,
    RMB_OUTLINE_SILHOUETTE_CREASE = 2u
};

typedef struct RMBVec3 { double x,y,z; } RMBVec3;

typedef struct RMBTransform {
    double tx,ty,tz;
    double rx,ry,rz; /* radians */
    double sx,sy,sz;
} RMBTransform;

typedef struct RMBTriangle {
    uint16_t v[3];
    uint8_t object_id;
    int8_t shade_bias;
} RMBTriangle;

typedef struct RMBEdge {
    uint16_t a,b;
    int16_t tri0,tri1;
    uint8_t object_id;
} RMBEdge;

typedef struct RMBObject {
    uint8_t outline_mode;
    uint8_t visible;
    uint8_t casts_shadow;
    uint8_t shade_levels;
    uint8_t overlay_target_object;
} RMBObject;

typedef struct RMBScene {
    RMBVec3 vertices[RMB_MAX_VERTICES];
    RMBTriangle triangles[RMB_MAX_TRIANGLES];
    RMBEdge edges[RMB_MAX_EDGES];
    RMBObject objects[RMB_MAX_OBJECTS];
    uint16_t vertex_count;
    uint16_t triangle_count;
    uint16_t edge_count;
    uint8_t object_count;
    RMBVec3 bounds_min;
    RMBVec3 bounds_max;
    uint8_t bounds_valid;
} RMBScene;

typedef struct RMBLight {
    double x,y,z;
    uint8_t enabled;
} RMBLight;

void rmb_scene_init(RMBScene *s);
uint8_t rmb_new_object(RMBScene *s,uint8_t outline_mode);
void rmb_set_object_flags(RMBScene *s,uint8_t object_id,
                          uint8_t visible,uint8_t casts_shadow);
void rmb_set_object_shade_levels(RMBScene *s,uint8_t object_id,uint8_t levels);
void rmb_set_object_overlay_target(RMBScene *s,uint8_t object_id,
                                   uint8_t target_object_id);
RMBTransform rmb_transform(double tx,double ty,double tz,
                           double rx_deg,double ry_deg,double rz_deg,
                           double sx,double sy,double sz);
RMBTransform rmb_compose(const RMBTransform *parent,const RMBTransform *child);

void rmb_add_box(RMBScene *s,uint8_t object_id,const RMBTransform *xf,
                 double hx,double hy,double hz,int8_t shade_bias);

/* Compact imported-mesh path. xyz_q8 stores interleaved signed Q8 local
 * coordinates (x,y,z); indices stores triangle triplets. The transform is
 * applied after Q8 decode. This is host-only authoring data. */
void rmb_add_indexed_mesh_q8(RMBScene *s,uint8_t object_id,
                             const RMBTransform *xf,
                             const int16_t *xyz_q8,uint16_t vertex_count,
                             const uint16_t *indices,uint16_t triangle_count,
                             int8_t shade_bias);
void rmb_add_cylinder(RMBScene *s,uint8_t object_id,const RMBTransform *xf,
                      double radius,double height,uint8_t sides,
                      int8_t shade_bias,uint8_t caps);
void rmb_add_uv_sphere(RMBScene *s,uint8_t object_id,const RMBTransform *xf,
                       double radius,uint8_t rings,uint8_t slices,
                       int8_t shade_bias);
void rmb_add_dome(RMBScene *s,uint8_t object_id,const RMBTransform *xf,
                  double radius,uint8_t rings,uint8_t slices,
                  int8_t shade_bias,uint8_t base_cap);

void rmb_render(const RMBScene *s,double cam_x,double cam_y,double cam_z,
                uint8_t yaw,const RMBLight *light);

int rmb_segment_occluded(const RMBScene *s,
                         double lx,double ly,double lz,
                         double wx,double wy,double wz);

#endif
