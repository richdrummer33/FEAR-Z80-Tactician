#ifndef POLAR_BAKED_COMPOSITE_H
#define POLAR_BAKED_COMPOSITE_H

#include <stdint.h>
#include "tilesector_polar.h"

#define TSP_HOST_TILE_BYTES 32u
#define TSP_HOST_MAX_FRAME_LOADS TSP_MAP_CELLS

typedef struct TSPHostTileLoad {
    uint16_t slot;
    uint8_t bytes[TSP_HOST_TILE_BYTES];
} TSPHostTileLoad;

typedef struct TSPHostSceneVertex {
    /* Whole-unit fallback preserves the original static lighting scene. */
    int16_t x;
    int16_t y;
    /* Room-bundle overrides may retain exact 1/16-world-unit coordinates. */
    int16_t x_q4;
    int16_t y_q4;
    uint8_t has_exact_q4;
} TSPHostSceneVertex;

typedef struct TSPHostSceneSegment {
    uint8_t v0;
    uint8_t v1;
    uint8_t profile;
    uint8_t blocks_light;
    int8_t light_front_sign;
    int8_t visual_front_sign;
    /* Optional exact host-bake vertical interval. Static legacy scenes leave
     * this disabled and continue using the coarse profile-derived height. */
    int16_t z0_q4;
    int16_t z1_q4;
    uint8_t has_exact_z;
} TSPHostSceneSegment;

typedef struct TSPHostSceneLight {
    int16_t x_q4;
    int16_t y_q4;
    int16_t height_q4;
    uint8_t radius_world;
    uint8_t intensity;
} TSPHostSceneLight;

/* Horizontal receiver volume. x/y bounds are inclusive for host ray tests.
 * The floor/ceiling pair lets room-local bakes retain raised/sunken spaces
 * without hard-coding one global world polygon into the compositor. */
typedef struct TSPHostSceneRect {
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
    int16_t floor_z;
    int16_t ceiling_z;
} TSPHostSceneRect;

typedef int (*TSPHostExtraOccluderFn)(const void *user,
                                      double lx,double ly,double lz,
                                      double wx,double wy,double wz);

typedef struct TSPHostCompositeScene {
    const TSPHostSceneVertex *vertices;
    uint8_t vertex_count;
    const TSPHostSceneSegment *segments;
    uint8_t segment_count;
    const TSPHostSceneLight *lights;
    uint8_t light_count;
    const TSPHostSceneRect *rects;
    uint8_t rect_count;
    /* Optional host-only arbitrary 3D occluder. Used by imported/static mesh
     * props so the existing floor/wall light bake can see mesh-cast shadows.
     * Nothing here exists in the GG runtime representation. */
    TSPHostExtraOccluderFn extra_occluder;
    const void *extra_occluder_user;
    /* Angular size of the light source, in world units. Zero keeps the exact
     * point-light cast. Non-zero bakes a soft floor shadow instead: a point
     * source resolves every gap in an occluder perfectly, which for foliage
     * means each gap in the proxy prints as its own hard-edged polygon. A
     * real source has width, and penumbra scales with how far the occluder
     * sits above the receiver, so a high canopy's gaps blur wider than the
     * gaps themselves and merge into one mottled mass. */
    double source_radius;
} TSPHostCompositeScene;

enum {
    TSP_HOST_LIGHT_BASELINE = 0u,
    TSP_HOST_LIGHT_AO = 1u,
    /* Correct world/portal hard cast, no soft edge. */
    TSP_HOST_LIGHT_HARD = 2u,
    /* Same hard cast plus one-sided ordered-dither penumbra. */
    TSP_HOST_LIGHT_POINT = 3u
};

/* Select one host-bake presentation layer and provide the exact camera state.
 * This API is host-only; the Game Gear runtime never sees a light record. */
void tsp_host_composite_set_lighting(uint8_t stage,const TSPState *camera);

/* Optional room-local host scene. NULL restores the original static Polar
 * lighting scene. This never exists on the Game Gear runtime path. */
void tsp_host_composite_set_scene(const TSPHostCompositeScene *scene);

/*
 * Host-only sub-tile compositor + persistent 512-slot VRAM cache model.
 *
 * The host resolves partial edge coverage at 8x8 pixel granularity. Final
 * patterns are canonicalized under H/V flips, then assigned to a simulated
 * Game Gear tile cache. Patterns retained between frames keep their slot;
 * newly needed patterns generate explicit pre-baked tile-pattern uploads.
 */
/* Reset the simulated VRAM cache to the canonical permanent base tiles.
 * Room-bundle bakes call this before each independent bundle so no route
 * inherits dynamic slot state from a previous room. */
void tsp_host_composite_reset_cache(void);
void tsp_host_composite_begin_frame(void);
void tsp_host_composite_write(uint8_t row,uint8_t col,uint16_t word);
void tsp_host_composite_surface(uint8_t col,uint8_t clip_x0,uint8_t clip_x1,
                                int16_t tl,int16_t tr,int16_t bl,int16_t br,
                                uint8_t sid,uint8_t shade,uint8_t border,
                                uint8_t ao_left,uint8_t ao_right);

/* Room-bundle host baker only: depth-tested equivalents used when several
 * vertical/horizontal surfaces can occupy one screen ray. Depth is camera-
 * forward world distance. These APIs do not exist on the GG runtime path. */
void tsp_host_composite_surface_depth(uint8_t col,uint8_t clip_x0,uint8_t clip_x1,
                                      int16_t tl,int16_t tr,int16_t bl,int16_t br,
                                      uint8_t sid,uint8_t shade,uint8_t border,
                                      uint8_t ao_left,uint8_t ao_right,
                                      double depth);
void tsp_host_composite_pixel_depth(uint8_t sx,uint8_t sy,uint8_t sid,
                                    uint8_t shade,uint8_t black,double depth);
/* Number of stops in the ambient surface-brightness ramp. Walls address it in
 * steps of two (three-stop compatible); meshes may address every stop. */
#define TSP_HOST_SHADE_RAMP_LEN 5u
/* Mesh pixel addressed by ramp position (quantized incident angle) with the
 * separate binary cast-shadow visibility on the point-light channel. */
void tsp_host_composite_pixel_ramp(uint8_t sx,uint8_t sy,uint8_t sid,
                                   uint8_t ramp_level,uint8_t black,
                                   uint8_t lit,uint8_t recess,double depth);

/* Number of stops in the ambient surface-brightness ramp. Walls address it in
 * steps of two (three-stop compatible); meshes may address every stop. */
#define TSP_HOST_SHADE_RAMP_LEN 5u
/* Mesh pixel addressed by ramp position (quantized incident angle) with the
 * separate binary cast-shadow visibility on the point-light channel. */
void tsp_host_composite_pixel_ramp(uint8_t sx,uint8_t sy,uint8_t sid,
                                   uint8_t ramp_level,uint8_t black,
                                   uint8_t lit,uint8_t recess,double depth);
/* Ordered-dither crease emphasis over one object, darkening by ramp stops in
 * proportion to the per-pixel recess written by the mesh raster. Never darkens
 * past ramp position floor_pos, so a crease can never reach the SEM_BLACK
 * edge-boundary value and read as ink rather than as shade. */
void tsp_host_composite_crease_owner(uint8_t sid,uint8_t threshold,
                                     uint8_t max_steps,uint8_t floor_pos);
/* Depth-tested mesh pixel whose lighting is carried as a binary lit bit on the
 * existing point-light channel rather than as a per-pixel shade semantic. The
 * object keeps one uniform ambient shade, so mixed cells are resolved by the
 * shared straight-edge vocabulary and uniform cells cost no tile upload. */
void tsp_host_composite_pixel_depth_lit(uint8_t sx,uint8_t sy,uint8_t sid,
                                        uint8_t shade,uint8_t black,
                                        uint8_t lit,double depth);
/* Host-only clipped lighting overlay. Writes only where target_sid is already
 * the visible owner, and independently depth-tests multiple overlay facets.
 * The base owner's geometric depth/identity are preserved. */
void tsp_host_composite_pixel_overlay_depth(uint8_t sx,uint8_t sy,
                                            uint8_t target_sid,
                                            uint8_t shade,uint8_t black,
                                            double depth);
/* Host-only screen-space shade consolidation for a single mesh object.
 * Majority-votes each shade pixel against the eight neighbours that share the
 * same owner, so the object silhouette and its SEM_BLACK outline are never
 * crossed. This reduces tile vocabulary where the cost actually comes from --
 * unsupported single-pixel shade flips -- without moving the lit/unlit
 * boundary off the source geometry the way mesh decimation does.
 * min_support: same-shade same-owner neighbours required to keep a pixel. */
void tsp_host_composite_consolidate_owner(uint8_t sid,uint8_t min_support,
                                          uint8_t passes);

/*
 * Optional host-only enclosed-cell codec.
 *
 * Training sees exact final 8x8 patterns only where every pixel belongs to the
 * selected owner and no boundary-black pixel is present.  The committed
 * dictionary is then pinned into resident VRAM slots and only those fully
 * enclosed cells may be approximated; silhouette, holes, boundary crossings,
 * background interaction and cast shadows stay on the exact ordinary path.
 *
 * Call train_begin(), render/export representative views, then train_commit().
 * The dictionary remains active across cache resets until codec_disable().
 */
void tsp_host_composite_codec_train_begin(uint8_t owner_sid,uint8_t patterns);
void tsp_host_composite_codec_train_commit(void);
void tsp_host_composite_codec_disable(void);
/* Clear the atomic copy-on-write visible-slot set before a route's frame-zero
 * oracle. Frame zero is a bootstrap/reference frame and is never published as
 * a transition from the preceding independently baked route. */
void tsp_host_composite_codec_begin_route(void);
uint8_t tsp_host_composite_codec_pattern_count(void);
/* Copy one resident dictionary pattern in native GG 4bpp byte layout. */
void tsp_host_composite_codec_pattern_4bpp(uint8_t index,
                                           uint8_t out[TSP_HOST_TILE_BYTES]);
uint32_t tsp_host_composite_codec_sample_count(void);
uint32_t tsp_host_composite_codec_cell_count(void);
uint32_t tsp_host_composite_codec_error_sum(void);
uint8_t tsp_host_composite_codec_error_max(void);

void tsp_host_composite_export(uint16_t out[TSP_MAP_CELLS]);

uint16_t tsp_host_composite_frame_load_count(void);
const TSPHostTileLoad *tsp_host_composite_frame_loads(void);
uint16_t tsp_host_composite_frame_unique_count(void);
uint16_t tsp_host_composite_peak_unique_count(void);
uint16_t tsp_host_composite_peak_load_count(void);
uint32_t tsp_host_composite_total_load_count(void);
/* Host-only semantic probes used to validate portal sidedness. */
uint16_t tsp_host_composite_owner_pixel_count(uint8_t sid);
uint16_t tsp_host_composite_lit_owner_pixel_count(uint8_t sid);
int tsp_host_composite_write_ppm(const char *path);
/* Diagnostics: same preview but masked to one owner id, so a histogram
 * measures a single object's shade distribution. */
int tsp_host_composite_write_owner_ppm(const char *path,uint8_t sid);
/* Diagnostics: false-colour map of the per-pixel recess field for one object,
 * including sub-threshold values the crease dither will not draw. */
int tsp_host_composite_write_recess_ppm(const char *path,uint8_t sid);

#endif
