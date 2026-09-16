#ifndef RUNG1_GEOMETRY_REFERENCE_H
#define RUNG1_GEOMETRY_REFERENCE_H
/* The authoritative continuous-geometry reference for the tilesector renderer.
 *
 * This is the single implementation of "what the answer should be". Both the
 * adjudicator (tools/rung1/depth_adjudicate.c) and the regression test
 * (tools/rung1/geometry_reference_test.c) include this file, so the test really
 * does guard the thing that produced the result rather than a copy of it.
 *
 * It takes plain numbers, not renderer tables, so it can be exercised on
 * synthetic planes that appear nowhere in the map.
 *
 * ---------------------------------------------------------------------------
 * THE DERIVATION, and where each constant comes from
 * ---------------------------------------------------------------------------
 *
 * Bearing convention. bearing_q12() computes atan(|dy|/|dx|) and then, in the
 * sy branch, negates for dy < 0. So a world bearing b in 4096-unit turns names
 * the direction
 *
 *     u(b) = (cos B, -sin B),          B = 2*pi*b/4096
 *
 * Verify: (dx,dy) = (1,1) has ax == ay, ratio 1.0, atan_q12 ~ 512, sy set, so
 * b = -512 = 3584, and u(3584) = (cos -45, -sin -45) = (0.707, 0.707). Correct.
 *
 * Screen mapping. angle_x() indexes k_tspf_angle_x_pos by |rel| in q12 and
 * mirrors about 80. That table equals round(80 + 80 tan(theta)) to within one
 * LSB over all 513 entries (asserted by arm A of the regression test), so
 *
 *     x = 80 + 80 tan(theta)     <=>     tan(theta) = (x - 80)/80
 *
 * Column to pixel. screen_depth_plane() anchors iq at column 10 and marches by
 * +-step per column; column 10 is pixel 80, which is theta = 0. So tile column c
 * samples screen pixel x = 8c, and one column of step is 8 pixels of slope.
 *
 * Projection constant. k_tspf_invz[z] == round(2560/z) for every entry
 * (asserted by arm A), so inverse depth is K/z with K = 2560 and z the
 * camera-axis depth.
 *
 * Screen model. draw_run() forms tl = TSPF_HORIZON - hl with TSPF_HORIZON = 72,
 * then decrements for TSP_PROFILE_FULL, and bl = TSPF_HORIZON + hl. With
 * h = inv >> 1 that is y_top = 71 - h and y_bottom = 72 + h.
 *
 * The law. For a plane of unit normal n through V, at signed perpendicular
 * distance D = n.(V - P) from the camera at P with facing phi, the ray at
 * screen angle theta has direction u(phi + theta), meets the plane at
 * t = D / (n . u(phi+theta)), and the camera-axis depth is z = t cos(theta).
 * Expanding n . u(phi+theta) = A cos(theta) + B sin(theta) with
 *
 *     A = n.u(phi)              =  nx cos(phi) - ny sin(phi)
 *     B = n.u(phi + 90 degrees) = -(nx sin(phi) + ny cos(phi))
 *
 * gives
 *
 *     inv(x) = (K/D) * (A + B * (x - 80)/80)
 *
 * which is EXACTLY LINEAR IN SCREEN X. The screen-depth-plane model is
 * therefore the correct model for a planar wall, and only its quantization was
 * ever in question.
 *
 * Depth clip. inv_for_dq4() saturates at 255 below TSPF_NEAR_Z_Q4 (10 cells)
 * and returns k_tspf_invz[127] above TSPF_FAR_Z_Q4 (127 cells). Both integer
 * derivations inherit that identically, so it is a property of the
 * inverse-depth table and not of either derivation. D_eff carries it, and
 * `clipped` reports it so the clip can be measured on its own instead of being
 * charged to a derivation.
 */
#include <math.h>

#define GEOM_K 2560.0            /* projection constant, from k_tspf_invz */
#define GEOM_NEAR_CELLS 10.0     /* TSPF_NEAR_Z_Q4 >> 4 */
#define GEOM_FAR_CELLS 127.0     /* TSPF_FAR_Z_Q4 >> 4 */
#define GEOM_HORIZON_TOP 71.0    /* TSPF_HORIZON - 1, the FULL profile decrement */
#define GEOM_HORIZON_BOT 72.0    /* TSPF_HORIZON */
#define GEOM_HALF_W 80.0         /* screen half width in pixels */

typedef struct { double A,B,D,D_eff; int clipped; } GeomPlane;

/* nx,ny: unit normal. vx,vy: any point on the plane, in cells.
 * px,py: camera position in cells. yaw: heading in 256-unit turns. */
static void geom_plane(double nx,double ny,double vx,double vy,
                       double px,double py,double yaw,GeomPlane *p)
{
    double phi=2.0*M_PI*yaw/256.0, a;
    p->D = nx*(vx-px) + ny*(vy-py);
    a = p->D<0.0 ? -p->D : p->D;
    p->clipped = 0;
    if(a<GEOM_NEAR_CELLS){ a=GEOM_NEAR_CELLS; p->clipped=1; }
    else if(a>GEOM_FAR_CELLS){ a=GEOM_FAR_CELLS; p->clipped=2; }
    p->D_eff = p->D<0.0 ? -a : a;
    p->A = nx*cos(phi) - ny*sin(phi);
    p->B = -(nx*sin(phi) + ny*cos(phi));
}
/* inverse depth at screen pixel x, under the renderer's own depth clip */
static double geom_inv_at_pixel(const GeomPlane *p,double x)
{
    double v=(GEOM_K/p->D_eff)*(p->A + p->B*(x-GEOM_HALF_W)/GEOM_HALF_W);
    return v<0.0?-v:v;
}
/* the same with no clip at all; used only to size the clip itself */
static double geom_inv_unclipped(const GeomPlane *p,double x)
{
    double v=(GEOM_K/p->D)*(p->A + p->B*(x-GEOM_HALF_W)/GEOM_HALF_W);
    return v<0.0?-v:v;
}
static double geom_inv_at_column(const GeomPlane *p,int c){ return geom_inv_at_pixel(p,8.0*(double)c); }
/* fam 0 is the top edge (y = 71 - h), fam 2 the bottom (y = 72 + h) */
static double geom_y_at_column(const GeomPlane *p,int c,int fam)
{
    double h=geom_inv_at_column(p,c)*0.5;
    if(h>127.5) h=127.5;
    return fam==0 ? GEOM_HORIZON_TOP-h : GEOM_HORIZON_BOT+h;
}
#endif
