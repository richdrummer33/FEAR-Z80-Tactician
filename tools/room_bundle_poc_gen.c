/*
 * Host-only independent room-bundle baker.
 *
 * This deliberately keeps the current heavyweight exact-output philosophy:
 * every camera frame is rasterized on the host, reduced to an exact GG name
 * table plus explicit 32-byte tile-pattern loads, and delta-patched against
 * the preceding frame. The experiment is about composability, NOT ROM size.
 *
 * Two different authored rooms share one canonical S-shaped seam. Each bundle
 * starts from a freshly reset simulated VRAM cache, explores its room, returns
 * into the mathematically safe seam leg, resets the dynamic cache there, and
 * must finish with the exact same canonical name-table words as it started.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar.h"
#include "polar_baked_composite.h"

#define BUNDLE_COUNT 2u
#define ROUTE_FRAMES 192u
#define MAX_SEGMENTS 24u
#define MAX_SCENE_VERTICES (MAX_SEGMENTS*2u)
#define MAX_SCENE_RECTS 4u
#define PATCH_MAX (2u + TSP_MAP_CELLS * 5u)
#define TILEPATCH_MAX (2u + TSP_MAP_CELLS * (2u + TSP_HOST_TILE_BYTES))
#define PI 3.14159265358979323846

typedef struct V2 { double x,y; } V2;
typedef struct Seg {
    V2 a,b;
    double z0,z1;
    int8_t shade_bias;
} Seg;
typedef struct World {
    Seg seg[MAX_SEGMENTS];
    uint8_t count;
    TSPHostSceneVertex scene_vertices[MAX_SCENE_VERTICES];
    TSPHostSceneSegment scene_segments[MAX_SEGMENTS];
    TSPHostSceneLight scene_lights[1];
    TSPHostSceneRect scene_rects[MAX_SCENE_RECTS];
    TSPHostCompositeScene scene;
    uint8_t scene_vertex_count;
    uint8_t scene_rect_count;
    uint8_t lighting_stage;
} World;
typedef struct Pose {
    double x,y,z;
    uint8_t yaw;
} Pose;
typedef struct FramePack {
    uint16_t patch_len;
    uint16_t changed;
    uint16_t runs;
    uint16_t tile_len;
    uint16_t tile_loads;
    uint8_t patch[PATCH_MAX];
    uint8_t tile[TILEPATCH_MAX];
} FramePack;
typedef struct BundleStats {
    uint32_t patch_bytes;
    uint32_t tile_bytes;
    uint32_t tile_loads;
    uint16_t peak_tile_loads;
    uint16_t raw_peak_tile_loads;
    uint16_t scheduled_budget;
    uint16_t changed_words;
} BundleStats;
typedef struct TileJob {
    uint16_t release;
    uint16_t deadline;
    uint16_t slot;
    uint16_t assigned;
    uint8_t bytes[TSP_HOST_TILE_BYTES];
} TileJob;

static void die(const char *msg){
    fprintf(stderr,"fatal: %s\n",msg);
    exit(2);
}
static void add_seg(World *w,double ax,double ay,double bx,double by,
                    double z0,double z1,int8_t bias){
    Seg *s;
    TSPHostSceneSegment *ls;
    uint8_t sid=w->count;
    uint8_t v0=w->scene_vertex_count;
    uint8_t v1=(uint8_t)(v0+1u);
    if(w->count>=MAX_SEGMENTS||v1>=MAX_SCENE_VERTICES)
        die("too many room PoC segments");
    s=&w->seg[w->count++];
    s->a.x=ax;s->a.y=ay;s->b.x=bx;s->b.y=by;
    s->z0=z0;s->z1=z1;s->shade_bias=bias;

    w->scene_vertices[v0].x=(int16_t)lround(ax);
    w->scene_vertices[v0].y=(int16_t)lround(ay);
    w->scene_vertices[v1].x=(int16_t)lround(bx);
    w->scene_vertices[v1].y=(int16_t)lround(by);
    w->scene_vertex_count=(uint8_t)(v1+1u);

    ls=&w->scene_segments[sid];
    ls->v0=v0;ls->v1=v1;
    ls->profile=TSP_PROFILE_FULL;
    ls->blocks_light=1u;
    ls->light_front_sign=0;
    ls->visual_front_sign=0;
}
static void add_rect(World *w,int16_t x0,int16_t y0,int16_t x1,int16_t y1,
                     int16_t floor_z,int16_t ceiling_z){
    TSPHostSceneRect *r;
    if(w->scene_rect_count>=MAX_SCENE_RECTS)die("too many room scene rectangles");
    r=&w->scene_rects[w->scene_rect_count++];
    r->x0=x0;r->y0=y0;r->x1=x1;r->y1=y1;
    r->floor_z=floor_z;r->ceiling_z=ceiling_z;
}
static void finalize_scene(World *w){
    w->scene.vertices=w->scene_vertices;
    w->scene.vertex_count=w->scene_vertex_count;
    w->scene.segments=w->scene_segments;
    w->scene.segment_count=w->count;
    w->scene.lights=w->scene_lights;
    w->scene.light_count=1u;
    w->scene.rects=w->scene_rects;
    w->scene.rect_count=w->scene_rect_count;
}
static void add_transformed_exit_seg(World *w,double ax,double ay,double bx,double by){
    add_seg(w,152.0-ax,48.0-ay,152.0-bx,48.0-by,0,32,0);
}

/* Shared S-shaped seam plus one room beyond its east aperture.
 *
 * seam:
 *   old aperture x=0, y=-4..4
 *   inner wall x=12, y=4..28
 *   inner wall x=20, y=-4..20
 *   new aperture x=36, y=20..28
 *
 * The route uses only the new/east room. The old side exists only to make the
 * seam geometry complete and symmetric for later predecessor handoff tests.
 */
static void make_world(uint8_t bundle,World *w){
    double mouth_lo=bundle==0u?12.0:16.0;
    double mouth_hi=bundle==0u?36.0:32.0;
    double room_y0=bundle==0u?-28.0:-4.0;
    double room_y1=bundle==0u?76.0:52.0;

    memset(w,0,sizeof(*w));

    /* Canonical inner S-throat remains narrow. Only the final hidden leg
     * flares toward the room. This preserves the seam serialization proof
     * while avoiding player-width room apertures. */
    add_seg(w,0,-4,20,-4,0,32,0);
    add_seg(w,20,-4,20,20,0,32,0);
    add_seg(w,20,20,28,20,0,32,0);
    add_seg(w,28,20,36,mouth_lo,0,32,0);
    add_seg(w,12,4,12,28,0,32,0);
    add_seg(w,12,28,28,28,0,32,0);
    add_seg(w,28,28,36,mouth_hi,0,32,0);

    /* Exact 180-degree transformed exit throat. */
    add_transformed_exit_seg(w,0,-4,20,-4);
    add_transformed_exit_seg(w,20,-4,20,20);
    add_transformed_exit_seg(w,20,20,28,20);
    add_transformed_exit_seg(w,28,20,36,mouth_lo);
    add_transformed_exit_seg(w,12,4,12,28);
    add_transformed_exit_seg(w,12,28,28,28);
    add_transformed_exit_seg(w,28,28,36,mouth_hi);

    /* Main room. Bundle zero is intentionally very wide; bundle one remains
     * tighter so the stream demonstrates spatial rhythm rather than one
     * repeated corridor width. */
    add_seg(w,36,room_y0,36,mouth_lo,0,32,0);
    add_seg(w,36,mouth_hi,36,room_y1,0,32,0);
    add_seg(w,116,room_y0,116,(48.0-mouth_hi),0,32,0);
    add_seg(w,116,(48.0-mouth_lo),116,room_y1,0,32,0);
    add_seg(w,36,room_y0,116,room_y0,0,32,0);
    add_seg(w,116,room_y1,36,room_y1,0,32,0);

    add_rect(w,36,(int16_t)room_y0,116,(int16_t)room_y1,0,32);

    if(bundle==0u){
        /* Portal-shadow room: put the lamp BEHIND the aperture. The two flared
         * jambs are therefore deliberate shadow casters, producing diverging
         * opening-corner shadows plus long far-wall cuts. */
        w->scene_lights[0].x_q4=(int16_t)(28<<4);
        w->scene_lights[0].y_q4=(int16_t)(24<<4);
        w->scene_lights[0].height_q4=(uint8_t)(12<<4);
        w->scene_lights[0].radius_world=112u;
        w->scene_lights[0].intensity=255u;
    }else{
        /* Inset/spooky room: a low side light sits below a short baffle.
         * Light escaping around its ends should form a curious asymmetric pool
         * while the deeper occluder contributes a strong vertical shadow cut. */
        add_seg(w,68,8,84,8,0,32,0);
        add_seg(w,94,18,94,38,0,32,1);
        w->scene_lights[0].x_q4=(int16_t)(76<<4);
        w->scene_lights[0].y_q4=(int16_t)(0<<4);
        w->scene_lights[0].height_q4=(uint8_t)(8<<4);
        w->scene_lights[0].radius_world=88u;
        w->scene_lights[0].intensity=255u;
    }

    w->lighting_stage=TSP_HOST_LIGHT_POINT;
    finalize_scene(w);
}

static uint8_t yaw_lerp(uint8_t a,uint8_t b,double q){
    int d=(int)(int8_t)(b-a);
    int v=(int)a+(int)lround((double)d*q);
    return (uint8_t)v;
}
static double lerp(double a,double b,double q){return a+(b-a)*q;}

static Pose entry_outbound_pose(uint16_t f){
    Pose p;
    double q;
    p.z=16.0;
    if(f<16u){
        q=(double)f/15.0;
        p.x=16.0;p.y=lerp(12.0,16.0,q);p.yaw=64u;return p;
    }
    if(f<32u){
        q=(double)(f-16u)/15.0;
        p.x=16.0;p.y=lerp(16.0,24.0,q);p.yaw=yaw_lerp(64u,0u,q);return p;
    }
    q=(double)(f-32u)/31.0;
    p.x=lerp(16.0,62.0,q);p.y=24.0;p.yaw=0u;return p;
}

static Pose exit_transform(Pose p){
    p.x=152.0-p.x;
    p.y=48.0-p.y;
    p.yaw=(uint8_t)(p.yaw+128u);
    return p;
}

static Pose route_pose(uint16_t f,uint8_t bundle){
    Pose p;
    double q;
    (void)bundle;

    /* 0..63: canonical entry seam -> inside room. */
    if(f<64u)return entry_outbound_pose(f);

    p.z=16.0;
    /* 64..79: inspect upper side of the room. */
    if(f<80u){
        q=(double)(f-64u)/15.0;
        p.x=62.0;p.y=24.0;p.yaw=yaw_lerp(0u,32u,q);return p;
    }
    /* 80..95: cross toward the far side on a slightly offset line. */
    if(f<96u){
        q=(double)(f-80u)/15.0;
        p.x=lerp(62.0,90.0,q);p.y=30.0;p.yaw=yaw_lerp(32u,0u,q);return p;
    }
    /* 96..111: look across the room rather than beelining. */
    if(f<112u){
        q=(double)(f-96u)/15.0;
        p.x=90.0;p.y=30.0;p.yaw=yaw_lerp(0u,224u,q);return p;
    }
    /* 112..127: settle beside exit while still looking back. */
    q=(double)(f-112u)/15.0;
    p.x=lerp(90.0,90.0,q);p.y=lerp(30.0,24.0,q);
    p.yaw=yaw_lerp(224u,128u,q);
    if(f<128u)return p;

    /* 128..191: room -> transformed exit seam. Reversing the transformed
     * entry trajectory intentionally means the player backs through the exit
     * while looking toward the room. Once the inner wall occludes it, the
     * final transformed canonical pose is exactly equivalent to frame zero. */
    p=entry_outbound_pose((uint16_t)(191u-f));
    return exit_transform(p);
}

static int ray_seg(double ox,double oy,double dx,double dy,const Seg *s,double *t_out){
    double sx=s->b.x-s->a.x,sy=s->b.y-s->a.y;
    double den=dx*sy-dy*sx,qx,qy,t,u;
    if(fabs(den)<1e-10)return 0;
    qx=s->a.x-ox;qy=s->a.y-oy;
    t=(qx*sy-qy*sx)/den;
    u=(qx*dy-qy*dx)/den;
    if(t<=1e-6||u<-1e-8||u>1.0+1e-8)return 0;
    *t_out=t;return 1;
}
static uint8_t shade_for_inv(double inv,int8_t bias){
    int s=inv>=82.0?2:(inv>=46.0?1:0);
    s+=bias;if(s<0)s=0;if(s>2)s=2;return (uint8_t)s;
}
static int iround(double v){return (int)floor(v+0.5);}

static void render_pose(const World *w,const Pose *p,uint16_t out[TSP_MAP_CELLS]){
    int sx;
    TSPState cam;
    memset(&cam,0,sizeof(cam));
    cam.x_q4=(int16_t)lround(p->x*16.0);
    cam.y_q4=(int16_t)lround(p->y*16.0);
    cam.z_q4=(int16_t)lround(p->z*16.0);
    cam.yaw=p->yaw;

    tsp_host_composite_set_lighting(w->lighting_stage,&cam);
    tsp_host_composite_begin_frame();

    for(sx=0;sx<160;++sx){
        double rel=atan(((double)sx+0.5-80.0)/80.0);
        double ang=(double)p->yaw*(2.0*PI/256.0)+rel;
        double dx=cos(ang),dy=sin(ang),best=1e30;
        int best_sid=-1;
        uint8_t sid;
        for(sid=0u;sid<w->count;++sid){
            double t;
            if(ray_seg(p->x,p->y,dx,dy,&w->seg[sid],&t)&&t<best){
                best=t;best_sid=(int)sid;
            }
        }
        if(best_sid>=0){
            const Seg *s=&w->seg[best_sid];
            double depth=best*cos(rel);
            double top,bottom,inv;
            int it,ib;
            if(depth<0.01)depth=0.01;
            top=72.0-(s->z1-p->z)*80.0/depth;
            bottom=72.0-(s->z0-p->z)*80.0/depth;
            inv=2560.0/depth;if(inv>255.0)inv=255.0;
            it=iround(top);ib=iround(bottom);
            tsp_host_composite_surface((uint8_t)(sx>>3),(uint8_t)sx,(uint8_t)sx,
                                       (int16_t)it,(int16_t)it,
                                       (int16_t)ib,(int16_t)ib,
                                       (uint8_t)best_sid,
                                       shade_for_inv(inv,s->shade_bias),
                                       0u,0u,0u);
        }
    }
    tsp_host_composite_export(out);
}

static size_t build_patch(const uint16_t *a,const uint16_t *b,uint8_t *dst,
                          uint16_t *changed_out,uint16_t *runs_out){
    size_t p=2u;
    uint16_t changed=0u,runs=0u;
    uint8_t row;
    for(row=0u;row<TSP_ROWS;++row){
        uint8_t x=0u;
        uint16_t base=(uint16_t)row*TSP_COLS;
        while(x<TSP_COLS){
            uint8_t start,count,c;
            while(x<TSP_COLS&&a[base+x]==b[base+x])++x;
            if(x>=TSP_COLS)break;
            start=x;
            while(x<TSP_COLS&&a[base+x]!=b[base+x])++x;
            count=(uint8_t)(x-start);
            if(p+3u+(size_t)count*2u>PATCH_MAX)die("patch overflow");
            dst[p++]=row;dst[p++]=start;dst[p++]=count;
            for(c=0u;c<count;++c){
                uint16_t v=b[base+(uint16_t)start+c];
                dst[p++]=(uint8_t)v;dst[p++]=(uint8_t)(v>>8);
            }
            changed=(uint16_t)(changed+count);++runs;
        }
    }
    dst[0]=(uint8_t)runs;dst[1]=(uint8_t)(runs>>8);
    *changed_out=changed;*runs_out=runs;
    return p;
}
static int apply_patch(uint16_t *map,const uint8_t *src,size_t len){
    size_t p=2u;
    uint16_t n,i;
    if(len<2u)return 0;
    n=(uint16_t)src[0]|((uint16_t)src[1]<<8);
    for(i=0u;i<n;++i){
        uint8_t row,x,count,c;
        uint16_t base;
        if(p+3u>len)return 0;
        row=src[p++];x=src[p++];count=src[p++];
        if(row>=TSP_ROWS||!count||(uint16_t)x+count>TSP_COLS)return 0;
        if(p+(size_t)count*2u>len)return 0;
        base=(uint16_t)row*TSP_COLS+x;
        for(c=0u;c<count;++c){
            map[base+c]=(uint16_t)src[p]|((uint16_t)src[p+1u]<<8);
            p+=2u;
        }
    }
    return p==len;
}
static void capture_tiles(FramePack *fp){
    uint16_t n=tsp_host_composite_frame_load_count(),i;
    const TSPHostTileLoad *loads=tsp_host_composite_frame_loads();
    size_t p=2u;
    fp->tile_loads=n;
    fp->tile[p?0:0]=(uint8_t)n;
    fp->tile[1]=(uint8_t)(n>>8);
    for(i=0u;i<n;++i){
        if(p+2u+TSP_HOST_TILE_BYTES>TILEPATCH_MAX)die("tilepatch overflow");
        fp->tile[p++]=(uint8_t)loads[i].slot;
        fp->tile[p++]=(uint8_t)(loads[i].slot>>8);
        memcpy(fp->tile+p,loads[i].bytes,TSP_HOST_TILE_BYTES);
        p+=TSP_HOST_TILE_BYTES;
    }
    fp->tile_len=(uint16_t)p;
}

static uint16_t frame_tile_loads(const FramePack *fp){
    return (uint16_t)fp->tile[0]|((uint16_t)fp->tile[1]<<8);
}

/* Same release/deadline model as polar_demo_patch_gen.c, but frame zero is
 * deliberately excluded: it is the canonical seam bootstrap. Every normal
 * runtime frame receives the smallest steady-state tile-upload budget that
 * can satisfy all slot-use constraints. */
static uint16_t schedule_bundle_tiles(FramePack frames[ROUTE_FRAMES],
                                      const uint16_t *maps){
    uint32_t job_count=0u,j=0u;
    TileJob *jobs;
    int16_t last_use[512];
    uint16_t t,i;
    uint16_t chosen=0u,budget;

    for(t=1u;t<ROUTE_FRAMES;++t)job_count+=frame_tile_loads(&frames[t]);
    jobs=(TileJob *)malloc((job_count?job_count:1u)*sizeof(TileJob));
    if(!jobs)die("bundle tile scheduler allocation failed");
    for(i=0u;i<512u;++i)last_use[i]=-1;
    for(i=0u;i<TSP_MAP_CELLS;++i){
        uint16_t slot=maps[i]&TSP_TILE_ID_MASK;
        last_use[slot]=0;
    }

    for(t=1u;t<ROUTE_FRAMES;++t){
        const uint8_t *p=frames[t].tile+2u;
        uint16_t n=frame_tile_loads(&frames[t]),q;
        for(q=0u;q<n;++q){
            uint16_t slot=(uint16_t)p[0]|((uint16_t)p[1]<<8);
            p+=2u;
            jobs[j].release=(uint16_t)(last_use[slot]+1);
            jobs[j].deadline=t;
            jobs[j].slot=slot;
            jobs[j].assigned=0xffffu;
            memcpy(jobs[j].bytes,p,TSP_HOST_TILE_BYTES);
            p+=TSP_HOST_TILE_BYTES;
            ++j;
        }
        for(i=0u;i<TSP_MAP_CELLS;++i){
            uint16_t slot=maps[(size_t)t*TSP_MAP_CELLS+i]&TSP_TILE_ID_MASK;
            last_use[slot]=(int16_t)t;
        }
    }
    if(j!=job_count)die("bundle tile scheduler job count mismatch");

    for(budget=1u;budget<=48u&&!chosen;++budget){
        uint32_t done=0u;
        for(j=0u;j<job_count;++j)jobs[j].assigned=0xffffu;
        for(t=1u;t<ROUTE_FRAMES;++t){
            uint16_t k;
            for(k=0u;k<budget;++k){
                uint32_t best=UINT32_MAX,x;
                uint16_t best_deadline=0xffffu;
                for(x=0u;x<job_count;++x){
                    if(jobs[x].assigned==0xffffu &&
                       jobs[x].release<=t &&
                       jobs[x].deadline<best_deadline){
                        best=x;
                        best_deadline=jobs[x].deadline;
                    }
                }
                if(best==UINT32_MAX)break;
                jobs[best].assigned=t;
                ++done;
            }
            for(j=0u;j<job_count;++j){
                if(jobs[j].assigned==0xffffu&&jobs[j].deadline==t)break;
            }
            if(j<job_count)break;
        }
        if(done==job_count)chosen=budget;
    }
    if(!chosen)die("bundle tile scheduler needs more than 48 uploads/VBlank");

    for(t=1u;t<ROUTE_FRAMES;++t){
        uint16_t n=0u;
        size_t p=2u;
        for(j=0u;j<job_count;++j)if(jobs[j].assigned==t)++n;
        frames[t].tile[0]=(uint8_t)n;
        frames[t].tile[1]=(uint8_t)(n>>8);
        frames[t].tile_loads=n;
        for(j=0u;j<job_count;++j)if(jobs[j].assigned==t){
            if(p+2u+TSP_HOST_TILE_BYTES>TILEPATCH_MAX)
                die("scheduled bundle tilepatch overflow");
            frames[t].tile[p++]=(uint8_t)jobs[j].slot;
            frames[t].tile[p++]=(uint8_t)(jobs[j].slot>>8);
            memcpy(frames[t].tile+p,jobs[j].bytes,TSP_HOST_TILE_BYTES);
            p+=TSP_HOST_TILE_BYTES;
        }
        frames[t].tile_len=(uint16_t)p;
    }

    free(jobs);
    return chosen;
}

static uint64_t fnv64(const void *data,size_t n){
    const uint8_t *p=(const uint8_t *)data;
    uint64_t h=UINT64_C(1469598103934665603);
    size_t i;
    for(i=0u;i<n;++i){h^=p[i];h*=UINT64_C(1099511628211);}
    return h;
}

static void write_u16(FILE *f,uint16_t v){fputc((int)(v&255u),f);fputc((int)(v>>8),f);}
static void write_u32(FILE *f,uint32_t v){write_u16(f,(uint16_t)v);write_u16(f,(uint16_t)(v>>16));}

static void emit_bundle(FILE *pack,FILE *manifest,uint8_t bundle,
                        FramePack frames[ROUTE_FRAMES],
                        const uint16_t canonical[TSP_MAP_CELLS],
                        BundleStats *stats){
    uint16_t i;
    fprintf(manifest,"bundle=%u frames=%u patch_bytes=%lu tile_bytes=%lu tile_loads=%lu raw_peak_tile_loads=%u scheduled_peak=%u scheduled_budget=%u changed_words=%u\n",
            (unsigned)bundle,(unsigned)ROUTE_FRAMES,
            (unsigned long)stats->patch_bytes,(unsigned long)stats->tile_bytes,
            (unsigned long)stats->tile_loads,
            (unsigned)stats->raw_peak_tile_loads,
            (unsigned)stats->peak_tile_loads,
            (unsigned)stats->scheduled_budget,
            (unsigned)stats->changed_words);
    printf("ROOM_BUNDLE_STATS bundle=%u frames=%u patch_bytes=%lu tile_bytes=%lu tile_loads=%lu raw_peak=%u scheduled_peak=%u scheduled_budget=%u changed_words=%u\n",
           (unsigned)bundle,(unsigned)ROUTE_FRAMES,
           (unsigned long)stats->patch_bytes,(unsigned long)stats->tile_bytes,
           (unsigned long)stats->tile_loads,
           (unsigned)stats->raw_peak_tile_loads,
           (unsigned)stats->peak_tile_loads,
           (unsigned)stats->scheduled_budget,
           (unsigned)stats->changed_words);

    fputc((int)bundle,pack);fputc(1,pack);
    write_u16(pack,ROUTE_FRAMES);
    write_u32(pack,stats->patch_bytes);
    write_u32(pack,stats->tile_bytes);
    for(i=0u;i<ROUTE_FRAMES;++i){
        write_u16(pack,frames[i].patch_len);
        write_u16(pack,frames[i].tile_len);
        fwrite(frames[i].patch,1,frames[i].patch_len,pack);
        fwrite(frames[i].tile,1,frames[i].tile_len,pack);
    }
    (void)canonical;
}

int main(int argc,char **argv){
    const char *outdir;
    char path[512];
    FILE *pack,*manifest;
    uint16_t canonical[TSP_MAP_CELLS];
    uint64_t canonical_hash=0u;
    uint8_t bundle;

    if(argc!=2){fprintf(stderr,"usage: %s OUTPUT_DIR\n",argv[0]);return 2;}
    outdir=argv[1];

    snprintf(path,sizeof(path),"%s/room_bundle_poc.pack",outdir);
    pack=fopen(path,"wb");if(!pack)die("cannot create room bundle pack");
    fwrite("RBP1",1,4,pack);write_u16(pack,1u);fputc(BUNDLE_COUNT,pack);fputc(0,pack);

    snprintf(path,sizeof(path),"%s/room_bundle_poc_manifest.txt",outdir);
    manifest=fopen(path,"w");if(!manifest)die("cannot create room bundle manifest");
    fprintf(manifest,"Room bundle PoC pack v1\n");

    for(bundle=0u;bundle<BUNDLE_COUNT;++bundle){
        World w;
        FramePack *frames=(FramePack *)calloc(ROUTE_FRAMES,sizeof(FramePack));
        uint16_t *maps=(uint16_t *)malloc((size_t)ROUTE_FRAMES*TSP_MAP_CELLS*sizeof(uint16_t));
        uint16_t prev[TSP_MAP_CELLS],cur[TSP_MAP_CELLS],replay[TSP_MAP_CELLS];
        BundleStats stats={0};
        uint16_t f;
        if(!frames||!maps)die("bundle frame/map allocation failed");
        make_world(bundle,&w);
        tsp_host_composite_set_scene(&w.scene);

        /* Independent bundle: no dynamic VRAM history inherited. */
        tsp_host_composite_reset_cache();

        for(f=0u;f<ROUTE_FRAMES;++f){
            Pose p=route_pose(f,bundle);

            /* Once back inside the proven safe seam leg, force the exact
             * canonical cache vocabulary before handing off to another room. */
            if(f==176u)tsp_host_composite_reset_cache();

            render_pose(&w,&p,cur);
            capture_tiles(&frames[f]);
            memcpy(maps+(size_t)f*TSP_MAP_CELLS,cur,sizeof(cur));

            if(f==0u){
                memcpy(prev,cur,sizeof(prev));
                memcpy(replay,cur,sizeof(replay));
                frames[f].patch_len=(uint16_t)build_patch(cur,cur,frames[f].patch,
                                                          &frames[f].changed,&frames[f].runs);
                if(bundle==0u){
                    memcpy(canonical,cur,sizeof(canonical));
                    canonical_hash=fnv64(canonical,sizeof(canonical));
                }else if(memcmp(canonical,cur,sizeof(canonical))!=0)
                    die("bundle initial seam name table != canonical seam");
            }else{
                frames[f].patch_len=(uint16_t)build_patch(prev,cur,frames[f].patch,
                                                          &frames[f].changed,&frames[f].runs);
                memcpy(replay,prev,sizeof(replay));
                if(!apply_patch(replay,frames[f].patch,frames[f].patch_len)||
                   memcmp(replay,cur,sizeof(cur))!=0)
                    die("bundle patch replay != oracle");
                memcpy(prev,cur,sizeof(prev));
            }

            stats.patch_bytes+=frames[f].patch_len;
            stats.tile_bytes+=frames[f].tile_len;
            stats.tile_loads+=frames[f].tile_loads;
            stats.changed_words=(uint16_t)(stats.changed_words+frames[f].changed);
            if(frames[f].tile_loads>stats.raw_peak_tile_loads)
                stats.raw_peak_tile_loads=frames[f].tile_loads;

            if((f==0u||f==64u||f==96u||f==176u||f==191u)){
                snprintf(path,sizeof(path),"%s/bundle%u_frame%u.ppm",outdir,
                         (unsigned)bundle,(unsigned)f);
                if(!tsp_host_composite_write_ppm(path))die("screenshot write failed");
            }
        }

        if(memcmp(prev,canonical,sizeof(canonical))!=0)
            die("bundle terminal seam name table != canonical seam");

        stats.scheduled_budget=schedule_bundle_tiles(frames,maps);
        stats.tile_bytes=0u;
        stats.tile_loads=0u;
        stats.peak_tile_loads=0u;
        for(f=0u;f<ROUTE_FRAMES;++f){
            stats.tile_bytes+=frames[f].tile_len;
            stats.tile_loads+=frames[f].tile_loads;
            if(frames[f].tile_loads>stats.peak_tile_loads)
                stats.peak_tile_loads=frames[f].tile_loads;
        }

        fprintf(manifest,"bundle=%u canonical_begin=PASS canonical_end=PASS terminal_hash=%016llX\n",
                (unsigned)bundle,(unsigned long long)fnv64(prev,sizeof(prev)));
        emit_bundle(pack,manifest,bundle,frames,canonical,&stats);
        free(maps);
        free(frames);
    }

    fprintf(manifest,"canonical_seam_fnv64=%016llX\n",(unsigned long long)canonical_hash);
    fprintf(manifest,"independent_bundle_replay=PASS\n");
    fprintf(manifest,"cross_bundle_canonical_handoff=PASS\n");

    snprintf(path,sizeof(path),"%s/room_bundle_poc_canonical.bin",outdir);
    {
        FILE *cf=fopen(path,"wb");
        if(!cf)die("cannot create canonical seam map");
        if(fwrite(canonical,1,sizeof(canonical),cf)!=sizeof(canonical))
            die("canonical seam map write failed");
        fclose(cf);
    }

    fclose(manifest);fclose(pack);

    printf("ROOM_BUNDLE_POC_PASS bundles=%u frames_per_bundle=%u canonical=%016llX\n",
           BUNDLE_COUNT,ROUTE_FRAMES,(unsigned long long)canonical_hash);
    return 0;
}
