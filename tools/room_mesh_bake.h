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
    /* Optional fraction of the object's brightness distribution reserved for
     * the top ramp stop. Zero keeps equal quantiles. A small nonzero value
     * (for example 0.10) prevents a front-lit hero from spending a fifth of
     * its entire surface on the same near-white used by bright architecture,
     * while preserving true highlights and all five geometric shade levels. */
    double highlight_fraction;
    /* The counterpart at the dark end: the fraction of the distribution given
     * to the BOTTOM ramp stop. Zero keeps the existing policy exactly.
     *
     * Equal quantiles are the right default for an unknown light rig, but they
     * also fix how much of the figure is allowed to be dark, and on a figure
     * whose form lives in its creases that is the wrong thing to hold fixed.
     * Raising this hands more of the surface to the darkest stop, and because
     * occlusion and crease dominate the bottom of the ranking, the surface it
     * hands over is the creases and the rim -- which is where the extra range
     * is wanted and nowhere else.
     *
     * (1 - highlight_fraction) / (ramp_levels - 1) reproduces equal quantiles
     * exactly; the code is written so that identity holds, not approximately,
     * so this knob has a provable no-op setting. */
    double shadow_fraction;
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
    /* Set when the importer supplied a per-vertex material family. */
    uint8_t family_supplied;
    /* Treat the plane z = ground_z as an occluder in this object's ambient
     * occlusion probe, so its lower surfaces darken where they meet it. */
    uint8_t ground_contact;
    double ground_contact_z;
    /* Reach of the ground-contact probe, kept separate from ao_radius. The
     * two measure different things at different scales: ao_radius is sized to
     * the object's own crevices, while contact is about how far up the object
     * the floor still matters, which is a fraction of its height. */
    double ground_contact_radius;
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
    /* Optional per-vertex MATERIAL FAMILY, also measured on the source. This
     * is categorical, not a quantity: it says which material the surface is,
     * never how bright it is. Lightness is the shade ramp's job and mixing the
     * two would darken a shadowed pixel twice. */
    uint8_t vertex_family[RMB_MAX_VERTICES];
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
void rmb_set_object_ramp_highlight_fraction(RMBScene *s,uint8_t object_id,
                                            double fraction);
void rmb_set_object_ramp_shadow_fraction(RMBScene *s,uint8_t object_id,
                                         double fraction);
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
/* As above, plus a per-vertex material family. Pass NULL for vertex_family to
 * get behaviour identical to rmb_add_indexed_mesh_q8_ex. */
void rmb_add_indexed_mesh_q8_family(RMBScene *s,uint8_t object_id,
                                    const RMBTransform *xf,
                                    const int16_t *xyz_q8,uint16_t vertex_count,
                                    const uint16_t *indices,
                                    uint16_t triangle_count,int8_t shade_bias,
                                    const uint8_t *vertex_recess,
                                    const uint8_t *vertex_family);
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
/* The exact ray cast, bypassing any shadow map. This is what the map is
 * measured against; it is also still the answer for a light the map was not
 * built for. */
int rmb_segment_occluded_exact(const RMBScene *s,
                               double lx,double ly,double lz,
                               double wx,double wy,double wz);

/* ---------------------------------------------------------------------------
 * Silhouette shadow map
 *
 * The cast shadow used to be answered by ray-casting the segment from the
 * light to every receiver pixel against a decimated shadow proxy. That is
 * expensive enough that the proxy has to stay coarse -- and a coarse proxy is
 * exactly why the shadow on the floor reads as a flat polygon while the statue
 * above it reads as a detailed object. The two are the same silhouette and
 * they do not look like it.
 *
 * Rasterising the caster ONCE from a pinhole at the light inverts that trade.
 * Building the map is linear in triangles, so the caster can be the full
 * visual mesh instead of a 188-triangle blob, and every subsequent query is a
 * projection and a texture read rather than a mesh traversal. Higher fidelity
 * and less work: the per-pixel cost stops depending on the caster's complexity
 * at all.
 *
 * The map stores distance from the light along its own forward axis, so a
 * query is the standard depth comparison and a receiver nearer the light than
 * the caster is correctly left lit -- which a pure 2D silhouette mask could
 * not do.
 * ------------------------------------------------------------------------ */

/* Build (or rebuild) the shadow map for this scene and light position. Safe to
 * call repeatedly: it rebuilds only when the scene pointer, the light or the
 * requested resolution actually changed. Returns 0 when no shadow-casting
 * geometry exists or the light sits inside the caster bounds, in which case
 * every query falls back to the exact ray cast. */
int rmb_shadow_map_build(const RMBScene *s,double lx,double ly,double lz,
                         uint16_t resolution);
/* Discard the map; the next query falls back to ray casting. */
void rmb_shadow_map_reset(void);
/* 1 when a map is built and covers this scene/light. */
int rmb_shadow_map_ready(const RMBScene *s,double lx,double ly,double lz);

/*
 * Fraction of the source visible from a receiver, 0 (fully shadowed) to 255
 * (fully lit).
 *
 * source_radius 0 asks for the exact point cast and answers 0 or 255. A
 * positive radius runs percentage-closer soft shadows: the penumbra is sized
 * from the measured distance between the blocker and the receiver, so contact
 * is sharp and the far end of the shadow is soft. That is not a stylistic
 * choice -- it is what a disc source actually does, and it is the single
 * strongest cue that an object is standing ON something rather than floating
 * over it.
 */
uint8_t rmb_shadow_coverage(const RMBScene *s,
                            double lx,double ly,double lz,
                            double wx,double wy,double wz,
                            double source_radius);

/* Write the depth map as a PGM for eyes-on debugging. 0 on failure. */
int rmb_shadow_map_write_pgm(const char *path);

/* ---------------------------------------------------------------------------
 * Ground-plane contact occlusion
 *
 * A finite-radius ambient occlusion probe treats the ground as an occluder, so
 * surfaces close to it darken and surfaces further up do not. An INFINITE
 * plane would be useless here: it subtends exactly the lower hemisphere from
 * any height, so it occludes every point on a vertical wall equally and
 * produces no gradient at all. The gradient that makes an object look planted
 * comes entirely from the probe's limited reach -- a point at height h is
 * occluded by the floor only while h is small next to the AO radius.
 * ------------------------------------------------------------------------ */
void rmb_set_object_ground_contact(RMBScene *s,uint8_t object_id,
                                   uint8_t enabled,double ground_z,
                                   double reach);
/* The same measurement taken FROM the ground: how much of the upward
 * hemisphere the shadow-casting geometry leaves open at a floor point, 0
 * (enclosed) .. 255 (open). Light-independent by construction, which is the
 * point -- a cast shadow only anchors the object on the side the light throws
 * it, and from every other angle the figure still floats. */
uint8_t rmb_ground_contact_openness(const RMBScene *s,
                                    double wx,double wy,double wz,
                                    double radius);

#endif
