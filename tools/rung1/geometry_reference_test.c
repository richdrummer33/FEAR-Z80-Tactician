/* Permanent regression test for the continuous-geometry reference.
 *
 * The depth adjudication concluded that the ROM's screen_depth_plane derivation
 * is far more faithful to the intended perspective geometry than the historical
 * host inv0/inv1 path. That conclusion is only as good as the reference it was
 * measured against, and the failure mode to guard against is not arithmetic: it
 * is the reference and the implementation quietly sharing the same mistaken
 * convention, which is exactly how "the host is the oracle" became folklore.
 *
 * So this test attacks the reference from four independent directions, and it
 * touches no sampled data of any kind.
 *
 *   A  CONVENTIONS. The three constants the derivation reads off the shipped
 *      tables are asserted against those tables: k_tspf_invz == round(2560/z),
 *      k_tspf_angle_x_pos == round(80 + 80 tan) to within one LSB,
 *      k_tspf_sec_q7 == round(128/cos) to within one LSB. If a table is ever
 *      regenerated with a different field of view or projection scale, this
 *      fails loudly instead of silently invalidating the adjudication.
 *
 *   B  ALGEBRA. The closed linear form (K/D)(A + B tan theta) is checked against
 *      an explicit ray/plane intersection written a structurally different way:
 *      build the ray direction, dot it with the normal, solve for t, take
 *      z = t cos theta, invert. Swept over normals, distances, headings and
 *      every screen column. This catches an error in the reduction to a line.
 *
 *   C  CONVENTIONS, INDEPENDENTLY. Hand-derived cases whose expected values are
 *      written as literal arithmetic a reader can check on paper, with the
 *      derivation for each given in a comment. These are the ones that catch a
 *      shared convention error, because they were obtained from the geometry
 *      rather than from either implementation. Two of them differ only in the
 *      sign of B, so a flipped bearing convention cannot pass both.
 *
 *   D  SHIPPED CODE. The reference is tied back to the renderer's own
 *      inv_for_dq4 at points where that function is exact by construction, so
 *      the projection constant is confirmed against live code and not only
 *      against the table it was fitted from.
 *
 *   E  BEARING CONVENTION, against bearing_q12 itself. Added after arms B and C
 *      failed to catch a handedness error: both had been written from the same
 *      reading of the source, so they agreed with each other and with nothing
 *      else. A convention is now decided by the shipped function over thousands
 *      of vectors, not by an argument in a comment.
 *
 * Prints GEOMETRY_REFERENCE_OK on success and returns non-zero on any failure.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tilesector_polar_renderer.c"
#include "geometry_reference.h"

static int g_fail=0;
static void ck(int ok,const char *what,double got,double want,double tol)
{
    if(ok) return;
    ++g_fail;
    printf("  FAIL %-58s got %.6f want %.6f (tol %.6f)\n",what,got,want,tol);
}
static void near_(const char *what,double got,double want,double tol)
{
    ck(fabs(got-want)<=tol,what,got,want,tol);
}

/* ---- A: the conventions the derivation reads off the shipped tables ------ */
static void arm_conventions(void)
{
    int z,a,worst_ax=0,worst_sec=0,bad_invz=0;
    printf("A  conventions asserted against the shipped tables\n");
    for(z=11;z<128;++z){
        int want=(int)floor(2560.0/(double)z+0.5);
        if((int)k_tspf_invz[z]!=want){ ++bad_invz;
            if(bad_invz<4) printf("  FAIL k_tspf_invz[%d] = %u, round(2560/%d) = %d\n",
                                  z,k_tspf_invz[z],z,want); }
    }
    if(bad_invz) g_fail+=bad_invz;
    printf("  k_tspf_invz[z] == round(2560/z) for z in 11..127            %s\n",
           bad_invz?"FAIL":"ok");
    for(a=0;a<=512;++a){
        double th=2.0*M_PI*(double)a/4096.0, pred=80.0+80.0*tan(th);
        int d;
        if(pred>160.0) pred=160.0;
        d=(int)fabs(pred-(double)k_tspf_angle_x_pos[a]);
        if(d>worst_ax) worst_ax=d;
    }
    ck(worst_ax<=1,"k_tspf_angle_x_pos == round(80 + 80 tan)",(double)worst_ax,0.0,1.0);
    printf("  k_tspf_angle_x_pos == round(80 + 80 tan), worst %d LSB        %s\n",
           worst_ax,worst_ax<=1?"ok":"FAIL");
    for(a=0;a<=512;++a){
        double th=2.0*M_PI*(double)a/4096.0, pred=128.0/cos(th);
        int d;
        if(pred>255.0) pred=255.0;
        d=(int)fabs(pred-(double)k_tspf_sec_q7[a]);
        if(d>worst_sec) worst_sec=d;
    }
    ck(worst_sec<=1,"k_tspf_sec_q7 == round(128/cos)",(double)worst_sec,0.0,1.0);
    printf("  k_tspf_sec_q7 == round(128/cos), worst %d LSB                 %s\n",
           worst_sec,worst_sec<=1?"ok":"FAIL");
    ck(TSPF_HORIZON==72,"TSPF_HORIZON == 72",(double)TSPF_HORIZON,72.0,0.0);
    ck(TSPF_NEAR_Z_Q4==(10<<4),"TSPF_NEAR_Z_Q4 == 10 cells",(double)TSPF_NEAR_Z_Q4,160.0,0.0);
    ck(TSPF_FAR_Z_Q4==(127<<4),"TSPF_FAR_Z_Q4 == 127 cells",(double)TSPF_FAR_Z_Q4,2032.0,0.0);
    printf("  TSPF_HORIZON 72, near clip 10 cells, far clip 127 cells       ok\n");
}

/* ---- B: the linear form against explicit ray/plane intersection ---------- */
/* Deliberately written a different way: no A/B, no tangent, no reduction. */
static double inv_by_intersection(double nx,double ny,double vx,double vy,
                                  double px,double py,double yaw,double x_pixel)
{
    double phi=2.0*M_PI*yaw/256.0;
    double theta=atan((x_pixel-80.0)/80.0);
    double beta=phi+theta;
    double ux=cos(beta), uy=sin(beta);           /* u(b) = (cos B, +sin B), pinned by arm E */
    double D=nx*(vx-px)+ny*(vy-py);
    double denom=nx*ux+ny*uy;
    double t,z;
    if(fabs(denom)<1e-12) return -1.0;           /* ray parallel to the plane */
    t=D/denom;
    z=t*cos(theta);
    if(fabs(z)<1e-12) return -1.0;
    return fabs(GEOM_K/z);
}
static void arm_algebra(void)
{
    static const double NRM[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
    int ni,di,yi,c,n=0; double worst=0.0;
    printf("\nB  closed linear form vs explicit ray/plane intersection\n");
    for(ni=0;ni<4;++ni) for(di=0;di<9;++di){
        /* place the plane at a chosen perpendicular distance from the camera */
        double dist=12.0+(double)di*13.0;           /* 12 .. 116 cells, inside the clip */
        double px=100.0,py=100.0;
        double vx=px+NRM[ni][0]*dist, vy=py+NRM[ni][1]*dist;
        for(yi=0;yi<256;yi+=1){
            GeomPlane p; geom_plane(NRM[ni][0],NRM[ni][1],vx,vy,px,py,(double)yi,&p);
            if(p.clipped) continue;
            for(c=0;c<20;++c){
                double want=inv_by_intersection(NRM[ni][0],NRM[ni][1],vx,vy,px,py,
                                                (double)yi,8.0*(double)c);
                double got,e;
                if(want<0.0) continue;
                got=geom_inv_at_column(&p,c);
                e=fabs(got-want);
                /* relative, since inverse depth spans three orders of magnitude */
                if(want>1e-6) e/=want;
                if(e>worst) worst=e;
                ++n;
            }
        }
    }
    ck(worst<1e-9,"linear form agrees with intersection",worst,0.0,1e-9);
    printf("  %d samples over 4 normals x 9 distances x 256 headings x 20 columns\n",n);
    printf("  worst relative disagreement %.3e                             %s\n",
           worst,worst<1e-9?"ok":"FAIL");
}

/* ---- C: hand-derived cases, expected values written as checkable arithmetic */
static void arm_hand_cases(void)
{
    GeomPlane p;
    const double R2=0.70710678118654752440;   /* cos 45 = sin 45 */
    printf("\nC  hand-derived cases (expected values obtained from geometry,\n");
    printf("   not from either implementation)\n");

    /* 1. Wall x = 80, normal +x, camera at (20,20) facing +x (yaw 0).
     *    The wall is perpendicular to the view axis, so camera-axis depth is a
     *    constant 60 cells at every screen column and inv = 2560/60 everywhere. */
    geom_plane(1,0, 80,0, 20,20, 0, &p);
    near_("C1 perpendicular wall, column 0 ",geom_inv_at_column(&p,0), 2560.0/60.0, 1e-9);
    near_("C1 perpendicular wall, column 10",geom_inv_at_column(&p,10),2560.0/60.0, 1e-9);
    near_("C1 perpendicular wall, column 19",geom_inv_at_column(&p,19),2560.0/60.0, 1e-9);
    printf("   C1 wall perpendicular to view: inv constant at 2560/60        %s\n",
           g_fail?"FAIL":"ok");

    /* 2. Same wall, camera turned 90 degrees (yaw 64) so the wall is edge on.
     *    Ray at theta hits x = 80 at t = 60/|sin theta|, z = 60/|tan theta|,
     *    so inv = (2560/60) * |tan theta| = (2560/60) * |x-80|/80. */
    {
        int before=g_fail;
        geom_plane(1,0, 80,0, 20,20, 64, &p);
        near_("C2 edge-on wall, column 10",geom_inv_at_column(&p,10), 0.0, 1e-9);
        near_("C2 edge-on wall, column 0 ",geom_inv_at_column(&p,0),  (2560.0/60.0)*(80.0/80.0), 1e-9);
        near_("C2 edge-on wall, column 19",geom_inv_at_column(&p,19), (2560.0/60.0)*(72.0/80.0), 1e-9);
        printf("   C2 wall edge on: inv proportional to |x-80|                  %s\n",
               g_fail==before?"ok":"FAIL");
    }

    /* 3. Same wall, camera at yaw 32 (45 degrees). Ray bearing 45+theta, so
     *    t = 60/cos(45+theta), z = 60 cos(theta)/cos(45+theta), and
     *    inv = (2560/60) * cos(45+theta)/cos(theta)
     *        = (2560/60) * (cos45 - sin45 tan theta).
     *    At column 0, tan theta = -1, so inv = (2560/60) * 2 cos45. */
    {
        int before=g_fail;
        geom_plane(1,0, 80,0, 20,20, 32, &p);
        near_("C3 yaw 45, column 10",geom_inv_at_column(&p,10),(2560.0/60.0)*R2,       1e-9);
        near_("C3 yaw 45, column 0 ",geom_inv_at_column(&p,0), (2560.0/60.0)*R2*2.0,   1e-9);
        near_("C3 yaw 45, column 15",geom_inv_at_column(&p,15),(2560.0/60.0)*R2*0.5,   1e-9);
        printf("   C3 yaw 45: inv = (2560/60)(cos45 - sin45 tan theta)          %s\n",
               g_fail==before?"ok":"FAIL");
    }

    /* 4. The mirror of case 3 at yaw 224 (-45 degrees), where B changes sign:
     *    inv = (2560/60)(cos45 + sin45 tan theta), which is LARGEST at the right
     *    edge instead of the left. Cases 3 and 4 differ only in the sign of B,
     *    so a flipped bearing convention cannot satisfy both. */
    {
        int before=g_fail;
        geom_plane(1,0, 80,0, 20,20, 224, &p);
        near_("C4 yaw -45, column 0 ",geom_inv_at_column(&p,0), 0.0,                 1e-9);
        near_("C4 yaw -45, column 19",geom_inv_at_column(&p,19),(2560.0/60.0)*R2*1.9,1e-9);
        printf("   C4 yaw -45 is the mirror of C3: bearing sign is pinned       %s\n",
               g_fail==before?"ok":"FAIL");
    }

    /* 5. A wall with a +y normal instead of +x, to pin the normal convention.
     *    Plane y = 80, camera at (20,20) facing +x. Ray (cos theta, sin theta)
     *    reaches y = 80 at t = 60/sin theta, z = 60/tan theta = 60*80/(x-80),
     *    so inv = (2560/60)|x-80|/80. */
    {
        int before=g_fail;
        geom_plane(0,1, 0,80, 20,20, 0, &p);
        near_("C5 +y normal, column 0",geom_inv_at_column(&p,0), (2560.0/60.0)*1.0, 1e-9);
        near_("C5 +y normal, column 5",geom_inv_at_column(&p,5), (2560.0/60.0)*0.5, 1e-9);
        near_("C5 +y normal, column 10",geom_inv_at_column(&p,10),0.0,              1e-9);
        printf("   C5 +y normal behaves as the geometry requires                %s\n",
               g_fail==before?"ok":"FAIL");
    }

    /* 6. The screen model: h = inv/2, top = 71 - h, bottom = 72 + h, and the two
     *    are symmetric about 71.5. */
    {
        int before=g_fail;
        double yt,yb;
        geom_plane(1,0, 80,0, 20,20, 0, &p);
        yt=geom_y_at_column(&p,10,0); yb=geom_y_at_column(&p,10,2);
        near_("C6 top edge   ",yt,71.0-(2560.0/60.0)/2.0,1e-9);
        near_("C6 bottom edge",yb,72.0+(2560.0/60.0)/2.0,1e-9);
        near_("C6 symmetry about 71.5",(yt+yb)/2.0,71.5,1e-9);
        printf("   C6 h = inv/2 about a horizon of 71.5                         %s\n",
               g_fail==before?"ok":"FAIL");
    }

    /* 8. A NON-CARDINAL normal, where the two bearing conventions genuinely
     *    disagree. For n = (0.6, 0.8) through V = (80,60) with the camera at
     *    (20,20) facing +x, D = 0.6*60 + 0.8*40 = 68 and
     *        inv(x) = (2560/68) * |0.6 + 0.8*(x-80)/80|
     *    which is SMALLEST at the left edge. Under the opposite handedness the
     *    sign of the second term flips and it would be largest there, so this
     *    case alone separates them. Every FULL wall on this map is axis aligned,
     *    which is why the handedness error went unnoticed; this pins it anyway. */
    {
        int before=g_fail;
        geom_plane(0.6,0.8, 80,60, 20,20, 0, &p);
        near_("C8 non-cardinal, D    ",p.D,                      68.0,         1e-9);
        near_("C8 non-cardinal, col10",geom_inv_at_column(&p,10),(2560.0/68.0)*0.6,1e-9);
        near_("C8 non-cardinal, col 0",geom_inv_at_column(&p,0), (2560.0/68.0)*0.2,1e-9);
        near_("C8 non-cardinal, col19",geom_inv_at_column(&p,19),(2560.0/68.0)*1.32,1e-9);
        printf("   C8 non-cardinal normal separates the two handedness choices  %s\n",
               g_fail==before?"ok":"FAIL");
    }

    /* 7. The near clip is a policy of inv_for_dq4, not a property of either
     *    (iq,step) derivation, so the reference carries it and reports it. A
     *    wall 4 cells away is inside the 10-cell clip: the clipped reference
     *    must read 2560/10 while the true geometry is 2560/4, and the plane must
     *    say so. This is what lets the near-field class be quarantined from the
     *    projection-accuracy statistics rather than contaminating them. */
    {
        int before=g_fail;
        geom_plane(1,0, 24,0, 20,20, 0, &p);      /* D = 4 cells */
        ck(p.clipped==1,"C7 near clip flagged",(double)p.clipped,1.0,0.0);
        near_("C7 clipped inv  ",geom_inv_at_column(&p,10),      2560.0/10.0,1e-9);
        near_("C7 unclipped inv",geom_inv_unclipped(&p,80.0),    2560.0/4.0, 1e-9);
        geom_plane(1,0, 220,0, 20,20, 0, &p);     /* D = 200 cells, past the far clip */
        ck(p.clipped==2,"C7 far clip flagged",(double)p.clipped,2.0,0.0);
        near_("C7 far-clipped inv",geom_inv_at_column(&p,10),    2560.0/127.0,1e-9);
        printf("   C7 near/far clip is reported, not folded into the error       %s\n",
               g_fail==before?"ok":"FAIL");
    }
}

/* ---- E: pin the bearing convention against bearing_q12 itself ------------ */
/* This arm exists because arms B and C could not catch a handedness error: both
 * were written from the same reading of bearing_q12's sy branch, and that
 * reading was wrong. Sharing a premise is exactly the failure mode the hand
 * cases were meant to rule out, and they did not. So the convention is now
 * decided by the shipped function, over thousands of vectors, with the losing
 * candidate rejected by three orders of magnitude rather than by argument. */
static double wrap12(double a){ while(a>2048.0)a-=4096.0; while(a<-2048.0)a+=4096.0; return a; }
static void arm_bearing_convention(void)
{
    int dx,dy; double wA=0.0,wB=0.0; long n=0,diag=0,diagbad=0; int worstdiag=0;
    printf("\nE  bearing convention pinned against bearing_q12\n");
    for(dx=-200;dx<=200;dx+=3) for(dy=-200;dy<=200;dy+=3){
        double b,eA,eB; int ax,ay;
        if(!dx&&!dy) continue;
        ax=dx<0?-dx:dx; ay=dy<0?-dy:dy;
        b=(double)bearing_q12((int16_t)dx,(int16_t)dy);
        if(ax==ay){
            /* ratio_q8_exact used to return 0 rather than 255 for n == d, because
             * the true ratio 1.0 is 256 and does not fit the byte. Every caller
             * read that as a ratio of zero, so a 45-degree diagonal reported an
             * axis direction: a 512 Q12 error and, downstream, a 32-pixel raster
             * jump. It now saturates. This is the regression guard, so the
             * diagonals are ASSERTED here rather than merely counted. */
            double e=fabs(wrap12(b-atan2((double)dy,(double)dx)*4096.0/(2.0*M_PI)));
            ++diag; if(e>8.0){ ++diagbad; if((int)e>worstdiag) worstdiag=(int)e; }
            continue;
        }
        eA=fabs(wrap12(b-atan2( (double)dy,(double)dx)*4096.0/(2.0*M_PI)));
        eB=fabs(wrap12(b-atan2(-(double)dy,(double)dx)*4096.0/(2.0*M_PI)));
        if(eA>wA) wA=eA;
        if(eB>wB) wB=eB;
        ++n;
    }
    printf("   %ld vectors:  u(b)=(cos,+sin) worst %6.2f Q12   u(b)=(cos,-sin) worst %8.2f Q12\n",
           n,wA,wB);
    ck(wA<8.0,"bearing_q12 is a plain atan2",wA,0.0,8.0);
    ck(wB>100.0,"the opposite handedness is rejected",wB,4096.0,4096.0);
    printf("   the reference uses (cos, +sin); the opposite is rejected by %.0fx   %s\n",
           wB/(wA>0?wA:1.0), (wA<8.0&&wB>100.0)?"ok":"FAIL");
    {   int rbad=0,dd;
        for(dd=1;dd<=255;++dd) if(ratio_q8_exact((uint8_t)dd,(uint8_t)dd)!=255u) ++rbad;
        ck(rbad==0,"ratio_q8_exact(n,n) saturates to 255",(double)rbad,0.0,0.0);
        printf("   ratio_q8_exact(n,n) == 255 for all 255 n                      %s\n",
               rbad?"FAIL":"ok");
    }
    ck(diagbad==0,"no diagonal reads as an axis direction",(double)diagbad,0.0,0.0);
    printf("   %ld vectors whose scaled operands are equal, %ld off by more than\n",diag,diagbad);
    printf("   8 Q12 units, worst %d (%.2f degrees)                           %s\n",
           worstdiag,worstdiag*360.0/4096.0,diagbad?"FAIL":"ok");
    if(diagbad)
        printf("   REGRESSION: the ratio_q8_exact saturation has been lost.\n");
}

/* ---- D: tie the constant back to the shipped inv_for_dq4 ----------------- */
static void arm_shipped(void)
{
    int dq4,n=0; double worst=0.0;
    printf("\nD  projection constant against the shipped inv_for_dq4\n");
    /* Between the near and far clip inv_for_dq4 interpolates the invz table, so
     * it should track 2560/D to within the table's own rounding plus the
     * interpolation, which is well under two LSB. */
    for(dq4=(10<<4)+1;dq4<(127<<4);++dq4){
        double want=2560.0/((double)dq4/16.0);
        double got=(double)inv_for_dq4((int16_t)dq4);
        double e=fabs(got-want);
        if(e>worst) worst=e;
        ++n;
    }
    ck(worst<=2.0,"inv_for_dq4 tracks 2560/D",worst,0.0,2.0);
    printf("  %d distances between the clips, worst deviation %.3f LSB      %s\n",
           n,worst,worst<=2.0?"ok":"FAIL");
    /* And the clip endpoints are where the reference says they are. */
    ck(inv_for_dq4((int16_t)((10<<4)-1))==255u,"near clip saturates at 255",
       (double)inv_for_dq4((int16_t)((10<<4)-1)),255.0,0.0);
    ck(inv_for_dq4((int16_t)((127<<4)+1))==k_tspf_invz[127],"far clip pins to invz[127]",
       (double)inv_for_dq4((int16_t)((127<<4)+1)),(double)k_tspf_invz[127],0.0);
    printf("  near clip saturates at 255, far clip pins to invz[127]        ok\n");
}

int main(void)
{
    printf("continuous-geometry reference regression test\n");
    printf("(guards the reference the depth adjudication was measured against)\n\n");
    arm_conventions();
    arm_algebra();
    arm_bearing_convention();
    arm_hand_cases();
    arm_shipped();
    printf("\n");
    if(g_fail){ printf("GEOMETRY_REFERENCE_FAIL %d checks failed\n",g_fail); return 1; }
    printf("GEOMETRY_REFERENCE_OK all conventions, algebra, hand cases and\n");
    printf("shipped-code ties agree; the adjudication reference stands\n");
    return 0;
}
