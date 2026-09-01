#ifndef ROOM_MESH_BAKE_H
#define ROOM_MESH_BAKE_H

#include <stdint.h>

#define RMB_MAX_VERTICES 16384u
#define RMB_MAX_TRIANGLES 20000u
#define RMB_MAX_EDGES 4096u
#define RMB_MAX_OBJECTS 48u
/* Must match SHADE_RAMP_LEN in the host compositor. */
#define RMB_SHADE_RAMP_LEN 5u

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
    /* Screen-space shade consolidation applied after this object rasters.
     * consolidate_support is the same-shade neighbour count required to keep
     * a pixel (0 disables); consolidate_passes iterates the majority vote. */
    uint8_t consolidate_support;
    uint8_t consolidate_passes;
    /* Interpolate a per-vertex normal across each triangle instead of using
     * one flat face normal. Host-only cost; it moves the shade band boundary
     * onto the real surface curvature rather than onto the tessellation. */
    uint8_t smooth_shading;
    /* Address the compositor's brightness ramp directly instead of the
     * three-stop wall shade. ramp_levels is how many of the ramp's stops this
     * object may use (2..RMB_SHADE_RAMP_LEN); 0 keeps the legacy path. */
    uint8_t ramp_levels;
    /* Static per-vertex lighting bake. The light and the hero are both fixed,
     * so self-shadowing and cavity occlusion are properties of the geometry,
     * not of the frame: solve them once and interpolate. */
    uint8_t static_light;
    /* How much of the ranking the light direction is allowed to own, 0..1.
     * At 1.0 a strongly directional source pushes every surface on the lit
     * side to the top of the ramp and every surface on the dark side to the
     * bottom, leaving no tonal room for occlusion or crease to describe form.
     * Lowering it lets those terms carry the ordering, which is how low-colour
     * sprite art reads solid from every angle. */
    double incident_weight;
    double ao_radius;      /* crevice probe length in world units; 0 = off */
    double ao_strength;    /* 0..1 maximum darkening from full occlusion */
    double light_radius;   /* source radius in world units; 0 = hard point */
    double shadow_floor;   /* 0..1 brightness retained inside cast shadow */
    /* Choose ramp thresholds from this object's own brightness distribution so
     * every stop carries a similar share of the surface. The thresholds
     * themselves are bake state, not authored data, and live with the bake. */
    uint8_t equalize;
    /* Crease emphasis. crease_coverage is the fraction of the surface that
     * receives it, taken as a percentile of the measured recess field, so the
     * control means "how much of the model reads as folded" rather than an
     * arbitrary magnitude that has to be retuned per asset. 0 disables.
     * crease_steps is the maximum darkening in ramp stops, and crease_floor is
     * the ramp position it may never darken past -- creases stay a gradation
     * of shade and never reach the SEM_BLACK edge value. */
    /* Set when the importer supplied a source-measured recess field. The shell
     * cannot measure its own folds: decimation removes them first. */
    uint8_t recess_supplied;
    double crease_coverage;
    /* How far a fully-recessed pixel is darkened, as a fraction of its
     * brightness. Applied BEFORE quantization so the ramp equalization sees
     * it, which is why a crease in an already-dark region still deepens
     * instead of clipping against the bottom of the ramp. */
    double crease_depth;
    /* Ordered-dither the fractional ramp position, so five stops read as a
     * continuous gradation instead of five hard bands. */
    uint8_t ramp_dither;
    uint8_t lit_mask;
    uint8_t lit_ambient_shade;
    uint8_t lit_threshold;
    /* Ordered screen-space coverage for clipped overlays:
     * 4=solid (default), 3=75%, 2=50%, 1=25%. */
    uint8_t overlay_dither_quarters;
} RMBObject;

typedef struct RMBScene {
    RMBVec3 vertices[RMB_MAX_VERTICES];
    RMBTriangle triangles[RMB_MAX_TRIANGLES];
    RMBEdge edges[RMB_MAX_EDGES];
    RMBObject objects[RMB_MAX_OBJECTS];
    /* Optional per-vertex recess supplied by the importer, measured on the
     * full-resolution source rather than on this decimated shell. */
    uint8_t vertex_recess[RMB_MAX_VERTICES];
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
void rmb_set_object_overlay_dither(RMBScene *s,uint8_t object_id,
                                   uint8_t quarters);
void rmb_set_object_shade_consolidate(RMBScene *s,uint8_t object_id,
                                      uint8_t support,uint8_t passes);
void rmb_set_object_smooth_shading(RMBScene *s,uint8_t object_id,uint8_t on);
void rmb_set_object_ramp_shading(RMBScene *s,uint8_t object_id,
                                 uint8_t levels,uint8_t smooth);
void rmb_set_object_ramp_equalize(RMBScene *s,uint8_t object_id,uint8_t on);
void rmb_set_object_crease(RMBScene *s,uint8_t object_id,double coverage,
                           double depth);
void rmb_set_object_ramp_dither(RMBScene *s,uint8_t object_id,uint8_t on);
void rmb_set_object_static_light(RMBScene *s,uint8_t object_id,
                                 double ao_radius,double ao_strength,
                                 double light_radius,double shadow_floor);
void rmb_set_object_incident_weight(RMBScene *s,uint8_t object_id,double w);
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
/* As above, plus a per-vertex recess field measured on the source mesh. */
void rmb_add_indexed_mesh_q8_ex(RMBScene *s,uint8_t object_id,
                                const RMBTransform *xf,
                                const int16_t *xyz_q8,uint16_t vertex_count,
                                const uint16_t *indices,uint16_t triangle_count,
                                int8_t shade_bias,const uint8_t *vertex_recess);
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
