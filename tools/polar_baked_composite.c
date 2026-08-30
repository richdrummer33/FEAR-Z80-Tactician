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
#include "generated/tilesector_polar_data.inc"

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
static uint8_t g_light_src[TSP_MAP_CELLS][PIXELS];
static uint8_t g_lighting_stage=TSP_HOST_LIGHT_BASELINE;
static int16_t g_camera_x_q4;
static int16_t g_camera_y_q4;
static uint8_t g_camera_yaw;

/* Host-only source texture and inverse screen-angle table. The texture is
 * deliberately not present in the Game Gear runtime: the bake converts it
 * into final 8x8 patterns and name-table words. */
static uint8_t g_wall_tex[128][64];
static int16_t g_screen_rel_q12[160];
static uint8_t g_texture_ready;
static uint8_t g_screen_rel_ready;

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
static uint8_t shade_sem(uint8_t shade);
static int16_t lerp_edge7(int16_t a,int16_t b,uint8_t x);

static int host_angle_x(int16_t rel){
    uint16_t a=(uint16_t)(rel<0?-rel:rel);
    int x;
    if(a>512u)a=512u;
    x=rel<0?(int)(160u-k_tspf_angle_x_pos[a]):(int)k_tspf_angle_x_pos[a];
    return x>159?159:(x<0?0:x);
}
static void init_screen_rel(void){
    int x,rel;
    if(g_screen_rel_ready)return;
    for(x=0;x<160;++x){
        int best=0,best_err=999;
        for(rel=-512;rel<=512;++rel){
            int q=host_angle_x((int16_t)rel),e=q>x?q-x:x-q;
            if(e<best_err){best_err=e;best=rel;if(!e)break;}
        }
        g_screen_rel_q12[x]=(int16_t)best;
    }
    g_screen_rel_ready=1u;
}
static void load_wall_texture(void){
    FILE *f;
    int ch,count=0;
    if(g_texture_ready)return;
    f=fopen("experiments/polar_texture/TWBARLT1_2bpp.txt","rb");
    if(!f)die("cannot open experiments/polar_texture/TWBARLT1_2bpp.txt");
    while((ch=fgetc(f))!=EOF){
        if(ch=='#'){while((ch=fgetc(f))!=EOF&&ch!='\n'){}continue;}
        if(ch>='0'&&ch<='3'){
            if(count>=64*128)die("wall texture has too many pixels");
            g_wall_tex[count/64][count%64]=(uint8_t)(ch-'0');
            ++count;
        }
    }
    fclose(f);
    if(count!=64*128)die("wall texture must contain exactly 64x128 class pixels");
    g_texture_ready=1u;
}
static int wall_texture_phase(uint8_t sx,uint8_t v0,uint8_t v1,double *phase){
    const double pi=3.14159265358979323846;
    double cx=(double)g_camera_x_q4/16.0,cy=(double)g_camera_y_q4/16.0;
    double ax=(double)k_tspf_vx[v0],ay=(double)k_tspf_vy[v0];
    double bx=(double)k_tspf_vx[v1],by=(double)k_tspf_vy[v1];
    double vx=bx-ax,vy=by-ay,len=sqrt(vx*vx+vy*vy);
    double ang=((double)((int)g_camera_yaw*16+(int)g_screen_rel_q12[sx]))*(2.0*pi/4096.0);
    double rx=cos(ang),ry=sin(ang);
    double qx=ax-cx,qy=ay-cy;
    double den=rx*vy-ry*vx,u,px,py,tx,ty,p;
    if(len<1e-9||fabs(den)<1e-9)return 0;
    u=(qx*ry-qy*rx)/den;
    if(u<0.0)u=0.0;else if(u>1.0)u=1.0;
    px=ax+u*vx;py=ay+u*vy;tx=vx/len;ty=vy/len;
    if(tx<0.0||(fabs(tx)<1e-9&&ty<0.0)){tx=-tx;ty=-ty;}
    p=fmod(px*tx+py*ty,64.0);
    if(p<0.0)p+=64.0;
    *phase=p;
    return 1;
}
static uint8_t texture_semantic_exact(uint8_t cls){
    if(!cls)return SEM_BLACK;
    return (uint8_t)(SEM_FAR+(cls-1u));
}
static double unwrap_period64(double from,double to){
    double d=to-from;
    while(d>32.0)d-=64.0;
    while(d<-32.0)d+=64.0;
    return d;
}
static int quant_span(double v,uint8_t coarse){
    static const uint8_t k_fine[]={
        1u,2u,4u,6u,8u,12u,16u,24u,32u,48u,64u,96u,128u
    };
    static const uint8_t k_edge[]={
        2u,4u,8u,16u,24u,32u,48u,64u,96u,128u
    };
    const uint8_t *k=coarse?k_edge:k_fine;
    unsigned n=coarse?sizeof(k_edge):sizeof(k_fine);
    unsigned i,best=0u;
    double e,beste=1e30;
    if(v<0.0)v=-v;
    for(i=0u;i<n;++i){
        e=fabs(v-(double)k[i]);
        if(e<beste){beste=e;best=i;}
    }
    return (int)k[best];
}
static int wrap64(int u){
    int q=u%64;
    if(q<0)q+=64;
    return q;
}
static uint8_t sample_wall_texture_quantized(uint8_t col,uint8_t row,
                                             uint8_t sx,int16_t y,
                                             int16_t tl,int16_t tr,
                                             int16_t bl,int16_t br,
                                             uint8_t v0,uint8_t v1){
    int sx0=(int)col*8,sx1=sx0+7,sxc=sx0+4;
    int ybase=(int)row*8;
    int16_t topc=lerp_edge7(tl,tr,4u),botc=lerp_edge7(bl,br,4u);
    int h=(int)(botc-topc+1);
    double ul,ur,uc,du,vspan,vcenter,uf,vf;
    int quspan,qvspan,ucenter_q,vcenter_q,ui,vi;
    int16_t yt=(int16_t)ybase,yb=(int16_t)(ybase+7);
    uint8_t edge_cell=(uint8_t)(tl>yt||tr>yt||bl<yb||br<yb);
    int phase_snap=edge_cell?8:4;
    int vphase_snap=edge_cell?8:4;
    if(h<2)return SEM_FAR;
    if(!wall_texture_phase((uint8_t)sx0,v0,v1,&ul)||
       !wall_texture_phase((uint8_t)sx1,v0,v1,&ur)||
       !wall_texture_phase((uint8_t)sxc,v0,v1,&uc))return SEM_FAR;

    du=unwrap_period64(ul,ur);
    quspan=quant_span(du,edge_cell);
    if(du<0.0)quspan=-quspan;

    /* IMPORTANT: quantize only the texture transform, NEVER wall ownership.
     * The renderer still walks every real projected wall pixel and clips to
     * its arbitrary top/bottom slopes. These buckets merely make nearby
     * camera poses reuse the same source-texture transform inside an 8x8
     * hardware pattern. */
    ucenter_q=(int)floor((uc+(double)phase_snap*0.5)/(double)phase_snap)*phase_snap;
    vspan=1024.0/(double)h; /* source texels covered by one 8px screen row */
    qvspan=quant_span(vspan,edge_cell);
    vcenter=(((double)(ybase+4-topc))*128.0)/(double)h;
    vcenter_q=(int)floor((vcenter+(double)vphase_snap*0.5)/(double)vphase_snap)*vphase_snap;

    uf=(double)ucenter_q+
       ((double)((int)sx-sx0)-3.5)*(double)quspan/7.0;
    vf=(double)vcenter_q+
       ((double)(y-ybase)-3.5)*(double)qvspan/7.0;
    ui=wrap64((int)floor(uf+0.5));
    vi=(int)floor(vf+0.5);
    if(vi<0)vi=0;else if(vi>127)vi=127;
    return texture_semantic_exact(g_wall_tex[vi][ui]);
}

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
    g_lighting_stage=stage>TSP_HOST_LIGHT_TEXTURE?TSP_HOST_LIGHT_TEXTURE:stage;
    if(camera){
        g_camera_x_q4=camera->x_q4;
        g_camera_y_q4=camera->y_q4;
        g_camera_yaw=camera->yaw;
    }
}

static uint8_t ao_pixel(uint8_t color,uint8_t strength,uint8_t sx,uint8_t sy){
    if(!strength||color<=SEM_FAR)return color;
    /* Stable ordered coverage rather than a temporal shimmer.  Stronger
     * authored corners darken more of the sub-tile footprint. */
    if(strength>=2u||(((uint8_t)(sx+sy)&1u)==0u))return (uint8_t)(color-1u);
    return color;
}

static uint8_t sem_at(uint8_t src[TSP_MAP_CELLS][PIXELS],int x,int y){
    uint16_t cell=(uint16_t)((y>>3)*TSP_COLS+(x>>3));
    return src[cell][(uint16_t)(y&7)*8u+(uint16_t)(x&7)];
}
static int shadow_blocked(int lx,int ly,int x,int y){
    int dx=x-lx,dy=y-ly,steps=abs(dx)>abs(dy)?abs(dx):abs(dy),i;
    if(steps<7)return 0;
    for(i=4;i<steps-2;++i){
        int sx=lx+(dx*i)/steps,sy=ly+(dy*i)/steps;
        uint8_t v;
        if(sx<0||sx>=160||sy<0||sy>=144)continue;
        v=sem_at(g_light_src,sx,sy);
        /* The horizon rule is a presentation separator, not physical cover. */
        if(v==SEM_BLACK&&sy!=72)return 1;
    }
    return 0;
}
static int clamp_i(int v,int lo,int hi){return v<lo?lo:(v>hi?hi:v);}
static void apply_point_light(void){
    const TSPHostStaticLight *light=&k_tsp_host_static_lights[0];
    const double pi=3.14159265358979323846;
    double cx=(double)g_camera_x_q4/16.0,cy=(double)g_camera_y_q4/16.0;
    double lxw=(double)light->x_q4/16.0,lyw=(double)light->y_q4/16.0;
    double dxw=lxw-cx,dyw=lyw-cy,dist=sqrt(dxw*dxw+dyw*dyw);
    double bearing=atan2(dyw,dxw),yaw=(double)g_camera_yaw*(2.0*pi/256.0);
    double rel=bearing-yaw;
    int lx,ly,radius,r2,row,col;

    while(rel>pi)rel-=2.0*pi;
    while(rel<-pi)rel+=2.0*pi;
    if(rel<-1.10||rel>1.10||dist<1.0)return;

    lx=(int)(80.0+tan(rel)*80.0);
    lx=clamp_i(lx,0,159);
    ly=72-(int)(((double)light->height_q4/16.0)*80.0/dist);
    ly=clamp_i(ly,46,69);
    radius=clamp_i((int)light->radius_world-(int)(dist/3.0),38,92);
    r2=radius*radius;

    memcpy(g_light_src,g_cells,sizeof(g_cells));

    /*
     * Quantize light/shadow ownership once per 8x8 display cell.  The cell's
     * existing sub-pixel wall/portal silhouette is retained exactly; only its
     * semantic shade indices are remapped.  This is the critical compression
     * guardrail: arbitrary per-pixel radial masks produced thousands of
     * one-off tile patterns and exceeded 48 tile uploads/VBlank.
     */
    for(row=0;row<TSP_ROWS;++row)for(col=0;col<TSP_COLS;++col){
        int sx=col*8+4,sy=row*8+4;
        int dx=sx-lx,dy=sy-ly,d2=dx*dx+dy*dy,strength;
        uint8_t blocked;
        uint8_t *cell;
        uint8_t i;

        if(d2>r2)continue;
        strength=((r2-d2)*255)/r2;
        if(strength<54)continue;
        blocked=(uint8_t)shadow_blocked(lx,ly,sx,sy);
        cell=g_cells[(uint16_t)row*TSP_COLS+col];

        for(i=0u;i<PIXELS;++i){
            uint8_t v=cell[i];
            if(v==SEM_BLACK||v==SEM_CEILING)continue;
            if(blocked){
                if(v>=SEM_MID)cell[i]=SEM_FAR;
            }else if(v==SEM_FLOOR){
                if(strength>112)cell[i]=SEM_FAR;
            }else if(v>=SEM_FAR&&v<SEM_NEAR){
                cell[i]=(uint8_t)(v+1u);
            }
        }
    }
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

void tsp_host_composite_begin_frame(void){
    uint8_t row,col;
    ensure_init();
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
                                uint8_t shade,uint8_t border,
                                uint8_t ao_left,uint8_t ao_right,
                                uint8_t profile,uint8_t v0,uint8_t v1){
    uint8_t sx,color=shade_sem(shade);
    uint16_t coarse_x=(uint16_t)col*8u;
    if(col>=TSP_COLS||clip_x0>clip_x1||clip_x1>159u)die("surface raster bounds invalid");

    if(g_lighting_stage==TSP_HOST_LIGHT_TEXTURE&&profile==TSP_PROFILE_FULL){
        init_screen_rel();
        load_wall_texture();
    }

    for(sx=clip_x0;sx<=clip_x1;++sx){
        uint8_t local=(uint8_t)((uint16_t)sx-coarse_x);
        int16_t top=lerp_edge7(tl,tr,local);
        int16_t bot=lerp_edge7(bl,br,local);
        int16_t y0=top<0?0:top;
        int16_t y1=bot>143?143:bot;
        int16_t y;
        uint8_t texture_ok=(uint8_t)(g_lighting_stage==TSP_HOST_LIGHT_TEXTURE&&
                                     profile==TSP_PROFILE_FULL);

        if(y0>y1)continue;
        for(y=y0;y<=y1;++y){
            uint8_t row=(uint8_t)((uint16_t)y>>3);
            uint8_t py=(uint8_t)((uint16_t)y&7u);
            uint8_t px=(uint8_t)((uint16_t)sx&7u);
            uint8_t *dst=g_cells[(uint16_t)row*TSP_COLS+col];
            uint8_t black=(uint8_t)(y==top||y==bot),pixel=color;
            if((border&1u)&&sx==clip_x0)black=1u;
            if((border&2u)&&sx==clip_x1)black=1u;
            if(!black&&texture_ok){
                /* Wall ownership remains exact and per-pixel. Only the
                 * texture transform is quantized for hardware pattern reuse. */
                pixel=sample_wall_texture_quantized(col,row,sx,y,tl,tr,bl,br,v0,v1);
            }else if(!black&&(g_lighting_stage==TSP_HOST_LIGHT_AO||
                              g_lighting_stage==TSP_HOST_LIGHT_POINT)){
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
            dst[(uint16_t)py*8u+px]=black?SEM_BLACK:pixel;
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

void tsp_host_composite_export(uint16_t out[TSP_MAP_CELLS]){
    FramePattern req[TSP_MAP_CELLS];
    int16_t htab[FRAME_HASH];
    uint16_t cell_req[TSP_MAP_CELLS],cell_attr[TSP_MAP_CELLS];
    uint8_t needed[HW_TILES];
    uint16_t req_count=0u,i;
    ensure_init();
    if(g_lighting_stage==TSP_HOST_LIGHT_POINT&&TSP_HOST_STATIC_LIGHT_COUNT)
        apply_point_light();
    ++g_frame_no;
    memset(htab,0xff,sizeof(htab));
    memset(needed,0,sizeof(needed));
    g_load_count=0u;

    for(i=0u;i<TSP_MAP_CELLS;++i){
        uint8_t canon[PIXELS];
        uint16_t attr,pos;
        uint64_t h;
        canonicalize(g_cells[i],canon,&attr);
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
        uint8_t v=g_cells[(uint16_t)row*TSP_COLS+col][(uint16_t)py*8u+px];
        if(v>5u)v=0u;
        fwrite(rgb[v],1,3,f);
    }
    fclose(f);return 1;
}
