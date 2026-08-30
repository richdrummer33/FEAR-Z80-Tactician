/*
 * Generate an exact, scripted Polar demo patch stream for the Game Gear
 * patch-player proof ROM.
 *
 * This is intentionally a proof harness, not the final world-state format:
 *   - initial patch: static GG name-table base -> exact initial Polar view
 *   - player-like exploration transitions: exact previous view -> exact next view
 *   - patch encoding: [run_count] then row,x,len + little-endian words
 *   - generated banks are capped conservatively and replay every changed word
 *
 * The runtime therefore performs no projection/visibility/raster work for this
 * known path. It only selects one banked patch, writes its final name words,
 * expands the already-proven row dirty extents, then lets the existing OTIR
 * uploader do the VDP transfer.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar.h"
#include "polar_explore_script.h"
#include "polar_baked_composite.h"

#define DEMO_FRAMES POLAR_EXPLORE_FRAMES
#define PATCH_COUNT (DEMO_FRAMES+1u)
#define MAX_BANKS 24u
#define MAX_BANK_STREAM 14000u
#define MAX_TILEPATCH_BANKS 24u
#define MAX_TILEPATCH_BANK_STREAM 14000u
/* Textured name-table deltas can fragment into several runs per row. */
#define PATCH_SCRATCH_MAX 2048u

typedef struct Patch {
    uint16_t len;
    uint16_t changed;
    uint8_t runs;
    uint8_t bytes[PATCH_SCRATCH_MAX];
} Patch;

typedef struct TilePatch {
    uint16_t loads;
    uint16_t len;
    uint8_t *bytes;
} TilePatch;

typedef struct TileJob {
    uint16_t release;
    uint16_t deadline;
    uint16_t slot;
    uint16_t assigned;
    uint8_t bytes[TSP_HOST_TILE_BYTES];
} TileJob;

typedef struct Bank {
    uint16_t first;
    uint16_t count;
    uint32_t bytes;
} Bank;

static uint8_t g_lighting_stage=TSP_HOST_LIGHT_BASELINE;
static const char *g_lighting_name="baseline";

static uint8_t parse_lighting_stage(const char *s){
    if(!strcmp(s,"baseline"))return TSP_HOST_LIGHT_BASELINE;
    if(!strcmp(s,"ao"))return TSP_HOST_LIGHT_AO;
    if(!strcmp(s,"point"))return TSP_HOST_LIGHT_POINT;
    if(!strcmp(s,"texture"))return TSP_HOST_LIGHT_TEXTURE;
    fprintf(stderr,"unknown lighting stage '%s' (expected baseline|ao|point|texture)\n",s);
    exit(2);
}
static int capture_frame(uint16_t frame){
    static const uint16_t k[] = {0u,120u,300u,520u,680u,760u,1000u,1113u};
    uint8_t i;
    for(i=0u;i<(uint8_t)(sizeof(k)/sizeof(k[0]));++i)if(frame==k[i])return 1;
    return 0;
}

static void die(const char *msg){
    fprintf(stderr,"fatal: %s\n",msg);
    exit(2);
}
static void path_join(char *dst,size_t cap,const char *dir,const char *name){
    int n=snprintf(dst,cap,"%s/%s",dir,name);
    if(n<0||(size_t)n>=cap)die("output path too long");
}
static uint16_t base_word(uint8_t row){
    if(row<9u)return TSP_TILE_CEILING;
    if(row==9u)return TSP_TILE_HORIZON;
    return TSP_TILE_FLOOR;
}
static void make_base(uint16_t map[TSP_MAP_CELLS]){
    uint8_t r,c;
    for(r=0u;r<TSP_ROWS;++r)
        for(c=0u;c<TSP_COLS;++c)
            map[(uint16_t)r*TSP_COLS+c]=base_word(r);
}
static void state_at(TSPState *s,int16_t xq,int16_t yq,uint8_t yaw){
    memset(s,0,sizeof(*s));
    s->x_q4=xq;s->y_q4=yq;s->yaw=yaw;s->speed_scale=1u;
}
static int map_valid(const uint16_t map[TSP_MAP_CELLS]){
    uint16_t i;
    const uint16_t legal=(uint16_t)(TSP_TILE_ID_MASK|TSP_ATTR_FLIPX|TSP_ATTR_FLIPY|TSP_ATTR_PALETTE);
    for(i=0u;i<TSP_MAP_CELLS;++i)if(map[i]&~legal)return 0;
    return 1;
}
static void render_fresh(const TSPState *src,uint16_t out[TSP_MAP_CELLS]){
    TSPState s;
    memset(out,0,sizeof(uint16_t)*TSP_MAP_CELLS);
    state_at(&s,src->x_q4,src->y_q4,src->yaw);
    /* Keep the geometric oracle fixed at appearance mode zero. AO and static
     * light are composed only after host rasterization, so the experiment
     * cannot smuggle extra Z80 projection/shading work into the runtime. */
    g_tspf_appearance_mode=0u;
    tsp_host_composite_set_lighting(g_lighting_stage,&s);
    tsp_host_composite_begin_frame();
    tsp_polar_renderer_reset();
    tsp_polar_render(&s,out,(TSPColumn *)0);
    tsp_host_composite_export(out);
    if(!map_valid(out))die("host compositor emitted invalid name-table word");
}
static void capture_tile_patch(TilePatch *tp){
    uint16_t n=tsp_host_composite_frame_load_count(),i;
    const TSPHostTileLoad *loads=tsp_host_composite_frame_loads();
    size_t len=2u+(size_t)n*(2u+TSP_HOST_TILE_BYTES),p=0u;
    tp->bytes=(uint8_t *)malloc(len?len:1u);
    if(!tp->bytes)die("out of memory capturing tile patch");
    tp->loads=n;tp->len=(uint16_t)len;
    tp->bytes[p++]=(uint8_t)n;tp->bytes[p++]=(uint8_t)(n>>8);
    for(i=0u;i<n;++i){
        uint16_t slot=loads[i].slot;
        tp->bytes[p++]=(uint8_t)slot;tp->bytes[p++]=(uint8_t)(slot>>8);
        memcpy(tp->bytes+p,loads[i].bytes,TSP_HOST_TILE_BYTES);p+=TSP_HOST_TILE_BYTES;
    }
    if(p!=len)die("tile patch length mismatch");
}

static uint16_t tilepatch_loads(const TilePatch *tp){
    return tp->bytes?(uint16_t)tp->bytes[0]|((uint16_t)tp->bytes[1]<<8):0u;
}

static uint32_t count_tile_jobs(const TilePatch *tp){
    uint16_t i;uint32_t n=0u;
    for(i=0u;i<PATCH_COUNT;++i)n+=tilepatch_loads(&tp[i]);
    return n;
}

/* Move tile-pattern uploads into earlier VBlanks whenever their hardware slot
 * is not referenced by the visible name table. This is the important second
 * half of baked compositing: the PC resolves both WHAT a cell looks like and
 * WHEN the 32-byte pattern can safely enter VRAM. */
static unsigned schedule_tilepatches(TilePatch *tp,const uint16_t *maps,const char *dir){
    uint32_t job_count=count_tile_jobs(tp),j=0u;
    TileJob *jobs=(TileJob *)malloc((size_t)job_count*sizeof(TileJob));
    int16_t last_use[512];
    uint16_t t,i;
    unsigned budget,chosen_budget=0u;
    if(!jobs)die("out of memory scheduling tile patterns");
    for(i=0u;i<512u;++i)last_use[i]=-1;

    for(t=0u;t<PATCH_COUNT;++t){
        const uint8_t *p=tp[t].bytes+2u;
        uint16_t n=tilepatch_loads(&tp[t]),q;
        for(q=0u;q<n;++q){
            uint16_t slot=(uint16_t)p[0]|((uint16_t)p[1]<<8);p+=2u;
            jobs[j].release=(uint16_t)(last_use[slot]+1);
            jobs[j].deadline=t;jobs[j].slot=slot;jobs[j].assigned=0xffffu;
            memcpy(jobs[j].bytes,p,TSP_HOST_TILE_BYTES);p+=TSP_HOST_TILE_BYTES;++j;
        }
        for(i=0u;i<TSP_MAP_CELLS;++i){
            uint16_t slot=maps[(size_t)t*TSP_MAP_CELLS+i]&TSP_TILE_ID_MASK;
            last_use[slot]=(int16_t)t;
        }
    }
    if(j!=job_count)die("tile job count mismatch");

    /* Diagnostic search ladder. We care first about the hardware question
     * "does this fit <=48?", then only need a coarse magnitude if it does not.
     * Trying all 1..360 budgets makes an intentionally-unsimplified texture
     * bake spend most of CI time proving the obvious. */
    {
    static const unsigned k_budget_probe[]={
        4u,8u,12u,16u,20u,24u,28u,32u,36u,40u,44u,48u,
        64u,80u,96u,128u,160u,192u,256u,320u,360u
    };
    unsigned bi;
    for(bi=0u;bi<sizeof(k_budget_probe)/sizeof(k_budget_probe[0])&&!chosen_budget;++bi){
        uint32_t done=0u;
        budget=k_budget_probe[bi];
        for(j=0u;j<job_count;++j)jobs[j].assigned=0xffffu;
        for(t=0u;t<PATCH_COUNT;++t){
            unsigned cap=(t==0u)?400u:budget,k;
            for(k=0u;k<cap;++k){
                uint32_t best=UINT32_MAX,x;
                uint16_t best_deadline=0xffffu;
                for(x=0u;x<job_count;++x)if(jobs[x].assigned==0xffffu &&
                    jobs[x].release<=t && jobs[x].deadline<best_deadline){
                    best=x;best_deadline=jobs[x].deadline;
                }
                if(best==UINT32_MAX)break;
                jobs[best].assigned=t;++done;
            }
            for(j=0u;j<job_count;++j)if(jobs[j].assigned==0xffffu&&jobs[j].deadline==t)
                break;
            if(j<job_count)break;
        }
        if(done==job_count)chosen_budget=budget;
    }
    }
    if(!chosen_budget)die("could not schedule tile uploads within 360 tiles/VBlank");

    /* Durable scheduler trace: lets us distinguish a bad visual bake from a
     * stale/reused VRAM slot without reverse-engineering generated C banks. */
    {
        char tracepath[512];
        FILE *trace;
        path_join(tracepath,sizeof(tracepath),dir,"tile_schedule.csv");
        trace=fopen(tracepath,"w");
        if(!trace)die("cannot write tile schedule trace");
        fprintf(trace,"job,slot,release,deadline,assigned\n");
        for(j=0u;j<job_count;++j)
            fprintf(trace,"%" PRIu32 ",%u,%u,%u,%u\n",j,(unsigned)jobs[j].slot,
                    (unsigned)jobs[j].release,(unsigned)jobs[j].deadline,
                    (unsigned)jobs[j].assigned);
        fclose(trace);
    }

    for(t=0u;t<PATCH_COUNT;++t){free(tp[t].bytes);tp[t].bytes=0;tp[t].len=0;tp[t].loads=0;}
    for(j=0u;j<job_count;++j)++tp[jobs[j].assigned].loads;
    for(t=0u;t<PATCH_COUNT;++t){
        size_t len=2u+(size_t)tp[t].loads*(2u+TSP_HOST_TILE_BYTES),p=2u;
        uint16_t n=tp[t].loads;
        tp[t].bytes=(uint8_t *)malloc(len?len:1u);
        if(!tp[t].bytes)die("out of memory rebuilding scheduled tilepatch");
        tp[t].len=(uint16_t)len;tp[t].bytes[0]=(uint8_t)n;tp[t].bytes[1]=(uint8_t)(n>>8);
        for(j=0u;j<job_count;++j)if(jobs[j].assigned==t){
            tp[t].bytes[p++]=(uint8_t)jobs[j].slot;
            tp[t].bytes[p++]=(uint8_t)(jobs[j].slot>>8);
            memcpy(tp[t].bytes+p,jobs[j].bytes,TSP_HOST_TILE_BYTES);p+=TSP_HOST_TILE_BYTES;
        }
        if(p!=len)die("scheduled tilepatch rebuild mismatch");
    }
    free(jobs);
    return chosen_budget;
}
static size_t build_patch(const uint16_t *a,const uint16_t *b,uint8_t *dst,
                          uint16_t *changed_out,uint8_t *runs_out){
    size_t p=1u;uint16_t changed=0u;uint8_t runs=0u,row;
    for(row=0u;row<TSP_ROWS;++row){
        uint8_t x=0u;uint16_t base=(uint16_t)row*TSP_COLS;
        while(x<TSP_COLS){
            uint8_t start,len,c;
            while(x<TSP_COLS&&a[base+x]==b[base+x])++x;
            if(x>=TSP_COLS)break;
            start=x;
            while(x<TSP_COLS&&a[base+x]!=b[base+x])++x;
            len=(uint8_t)(x-start);
            if(p+3u+(size_t)len*2u>PATCH_SCRATCH_MAX)die("patch scratch overflow");
            dst[p++]=row;dst[p++]=start;dst[p++]=len;
            for(c=0u;c<len;++c){
                uint16_t w=b[base+(uint16_t)start+c];
                dst[p++]=(uint8_t)w;dst[p++]=(uint8_t)(w>>8);
            }
            changed=(uint16_t)(changed+len);++runs;
        }
    }
    dst[0]=runs;*changed_out=changed;*runs_out=runs;return p;
}
static int apply_patch(uint16_t *map,const uint8_t *src,size_t len){
    size_t p=1u;uint8_t n,i;
    if(len<1u)return 0;n=src[0];
    for(i=0u;i<n;++i){
        uint8_t row,x,count,c;uint16_t base;
        if(p+3u>len)return 0;
        row=src[p++];x=src[p++];count=src[p++];
        if(row>=TSP_ROWS||x>=TSP_COLS||!count||(uint16_t)x+count>TSP_COLS)return 0;
        if(p+(size_t)count*2u>len)return 0;
        base=(uint16_t)row*TSP_COLS;
        for(c=0u;c<count;++c){
            map[base+(uint16_t)x+c]=(uint16_t)src[p]|((uint16_t)src[p+1u]<<8);
            p+=2u;
        }
    }
    return p==len;
}
static void verify(const uint16_t *a,const uint16_t *b,const Patch *p){
    uint16_t q[TSP_MAP_CELLS];
    memcpy(q,a,sizeof(q));
    if(!apply_patch(q,p->bytes,p->len))die("generated patch parser failed");
    if(memcmp(q,b,sizeof(q))!=0)die("generated patch replay != oracle");
}
static uint64_t fnv64(const void *data,size_t n){
    const uint8_t *p=(const uint8_t *)data;size_t i;
    uint64_t h=UINT64_C(1469598103934665603);
    for(i=0;i<n;++i){h^=p[i];h*=UINT64_C(1099511628211);}
    return h;
}
static void emit_u8_array(FILE *f,const char *name,const uint8_t *v,uint32_t n){
    uint32_t i;
    fprintf(f,"static const uint8_t %s[%u] = {\n",name,n?n:1u);
    if(!n)fprintf(f,"    0,\n");
    for(i=0u;i<n;++i){
        if((i&15u)==0u)fprintf(f,"    ");
        fprintf(f,"%u,",(unsigned)v[i]);
        if((i&15u)==15u||i+1u==n)fprintf(f,"\n");else fputc(' ',f);
    }
    fprintf(f,"};\n\n");
}
static void emit_u16_array(FILE *f,const char *name,const uint16_t *v,uint32_t n){
    uint32_t i;
    fprintf(f,"static const uint16_t %s[%u] = {\n",name,n?n:1u);
    if(!n)fprintf(f,"    0,\n");
    for(i=0u;i<n;++i){
        if((i&11u)==0u)fprintf(f,"    ");
        fprintf(f,"%u,",(unsigned)v[i]);
        if((i%12u)==11u||i+1u==n)fprintf(f,"\n");else fputc(' ',f);
    }
    fprintf(f,"};\n\n");
}
static void emit_bank(const char *dir,unsigned bank,const Patch *patches,const Bank *b){
    char name[128],path[512];FILE *f;
    uint16_t *off=(uint16_t *)malloc((size_t)(b->count+1u)*sizeof(uint16_t));
    uint8_t *data=(uint8_t *)malloc(b->bytes?b->bytes:1u);
    uint32_t pos=0u;uint16_t i;
    if(!off||!data)die("out of memory emitting bank");
    off[0]=0u;
    for(i=0u;i<b->count;++i){
        const Patch *p=&patches[b->first+i];
        if(pos+p->len>65535u)die("single generated bank offset overflow");
        memcpy(data+pos,p->bytes,p->len);pos+=p->len;off[i+1u]=(uint16_t)pos;
    }
    snprintf(name,sizeof(name),"polar_demo_patch_bank%u.c",bank);
    path_join(path,sizeof(path),dir,name);f=fopen(path,"w");
    if(!f){fprintf(stderr,"cannot write %s: %s\n",path,strerror(errno));exit(2);}
    fprintf(f,
      "/* GENERATED exact Polar demo patch bank %u. */\n"
      "#include <stdint.h>\n#include <gbdk/platform.h>\n#include \"tilesector_polar.h\"\n"
      "#pragma bank 255\nBANKREF(polar_demo_patch_bank%u)\n\n"
      "extern uint16_t g_map[TSP_MAP_CELLS];\n"
      "extern uint8_t g_polar_nt_row_min[TSP_ROWS];\n"
      "extern uint8_t g_polar_nt_row_max[TSP_ROWS];\n\n",
      bank,bank);
    emit_u16_array(f,"k_off",off,(uint32_t)b->count+1u);
    emit_u8_array(f,"k_data",data,pos);
    fprintf(f,
      "#define PATCHS_IN_BANK %uu\n"
      "void tsp_polar_demo_patch_bank%u(uint16_t local) BANKED {\n"
      "    const uint8_t *p; uint8_t n,i;\n"
      "    if(local>=PATCHS_IN_BANK)return;\n"
      "    p=&k_data[k_off[local]]; n=*p++;\n"
      "    for(i=0u;i<n;++i){\n"
      "        uint8_t row=*p++,x=*p++,count=*p++,c;\n"
      "        uint16_t idx=(uint16_t)row*TSP_COLS+x;\n"
      "        uint8_t last=(uint8_t)(x+count-1u);\n"
      "        if(g_polar_nt_row_min[row]==0xffu||x<g_polar_nt_row_min[row])g_polar_nt_row_min[row]=x;\n"
      "        if(last>g_polar_nt_row_max[row])g_polar_nt_row_max[row]=last;\n"
      "        for(c=0u;c<count;++c){\n"
      "            uint16_t w=(uint16_t)*p++; w|=(uint16_t)*p++<<8; g_map[idx++]=w;\n"
      "        }\n"
      "    }\n"
      "}\n",
      b->count,bank);
    fclose(f);free(off);free(data);
}
static void emit_dispatch(const char *dir,const Bank banks[MAX_BANKS],unsigned bank_count){
    char path[512];FILE *f;unsigned i;
    path_join(path,sizeof(path),dir,"polar_demo_patch_dispatch.c");f=fopen(path,"w");
    if(!f)die("cannot write generated dispatcher");
    fprintf(f,
      "/* GENERATED exact Polar demo patch dispatcher. */\n"
      "#include <stdint.h>\n#include <gbdk/platform.h>\n#include \"tilesector_polar.h\"\n\n");
    for(i=0u;i<MAX_BANKS;++i)
        fprintf(f,"void tsp_polar_demo_patch_bank%u(uint16_t local) BANKED;\n",i);
    fprintf(f,"\nvoid tsp_polar_demo_patch_apply(uint16_t patch){\n");
    for(i=0u;i<bank_count;++i){
        uint16_t end=(uint16_t)(banks[i].first+banks[i].count);
        fprintf(f,"    %s(patch<%uu){tsp_polar_demo_patch_bank%u((uint16_t)(patch-%uu));return;}\n",
                i?"else if":"if",end,i,banks[i].first);
    }
    fprintf(f,"}\n");
    fclose(f);
}
static void emit_meta(const char *dir,const Bank banks[MAX_BANKS],unsigned bank_count,
                      uint64_t expected_hash){
    char path[512];FILE *f;unsigned i;
    path_join(path,sizeof(path),dir,"polar_demo_patch_meta.h");f=fopen(path,"w");
    if(!f)die("cannot write generated patch metadata");
    fprintf(f,
      "/* GENERATED exact Polar demo patch metadata. */\n"
      "#ifndef POLAR_DEMO_PATCH_META_H\n#define POLAR_DEMO_PATCH_META_H\n"
      "#define POLAR_DEMO_PATCH_COUNT %uu\n#define POLAR_DEMO_FRAME_COUNT %uu\n"
      "#define POLAR_DEMO_BANK_COUNT %uu\n"
      "#define POLAR_DEMO_REFERENCE_FNV64 0x%016" PRIX64 "ULL\n",
      PATCH_COUNT,DEMO_FRAMES,bank_count,expected_hash);
    for(i=0u;i<bank_count;++i)
        fprintf(f,"#define POLAR_DEMO_BANK%u_FIRST %uu\n#define POLAR_DEMO_BANK%u_COUNT %uu\n",
                i,banks[i].first,i,banks[i].count);
    fprintf(f,"#endif\n");fclose(f);
}
static void emit_manifest(const char *dir,const Patch *patches,const Bank banks[MAX_BANKS],
                          unsigned bank_count,uint64_t expected_hash){
    char path[512];FILE *f;unsigned i;uint64_t total=0u;
    path_join(path,sizeof(path),dir,"polar_demo_patch_manifest.txt");f=fopen(path,"w");
    if(!f)die("cannot write generated patch manifest");
    fprintf(f,"Polar exact scripted demo patch pack\npatches=%u (initial + %u transitions)\n",
            PATCH_COUNT,DEMO_FRAMES);
    for(i=0u;i<PATCH_COUNT;++i)total+=patches[i].len;
    fprintf(f,"raw_patch_bytes=%" PRIu64 "\nreference_sequence_fnv64=%016" PRIX64 "\n",
            total,expected_hash);
    for(i=0u;i<bank_count;++i)
        fprintf(f,"bank%u first=%u count=%u stream_bytes=%" PRIu32 "\n",
                i,banks[i].first,banks[i].count,banks[i].bytes);
    fclose(f);
}

static void emit_tilepatch_bank(const char *dir,unsigned bank,const TilePatch *tp,const Bank *b){
    char name[128],path[512];FILE *f;
    uint16_t *off=(uint16_t *)malloc((size_t)(b->count+1u)*sizeof(uint16_t));
    uint8_t *data=(uint8_t *)malloc(b->bytes?b->bytes:1u);
    uint32_t pos=0u;uint16_t i;
    if(!off||!data)die("out of memory emitting tilepatch bank");
    off[0]=0u;
    for(i=0u;i<b->count;++i){
        const TilePatch *p=&tp[b->first+i];
        if(pos+p->len>65535u)die("tilepatch bank offset overflow");
        memcpy(data+pos,p->bytes,p->len);pos+=p->len;off[i+1u]=(uint16_t)pos;
    }
    snprintf(name,sizeof(name),"polar_demo_tilepatch_bank%u.c",bank);
    path_join(path,sizeof(path),dir,name);f=fopen(path,"w");
    if(!f)die("cannot write tilepatch bank");
    fprintf(f,
      "/* GENERATED baked composite tile-pattern update bank %u. */\n"
      "#include <stdint.h>\n#include <gbdk/platform.h>\n"
      "#pragma bank 255\nBANKREF(polar_demo_tilepatch_bank%u)\n\n",
      bank,bank);
    emit_u16_array(f,"k_off",off,(uint32_t)b->count+1u);
    emit_u8_array(f,"k_data",data,pos);
    fprintf(f,
      "#define PATCHS_IN_BANK %uu\n"
      "void tsp_polar_demo_tilepatch_bank%u(uint16_t local) BANKED {\n"
      "    const uint8_t *p; uint16_t n,i;\n"
      "    if(local>=PATCHS_IN_BANK)return;\n"
      "    p=&k_data[k_off[local]]; n=(uint16_t)*p++; n|=(uint16_t)*p++<<8;\n"
      "    for(i=0u;i<n;++i){\n"
      "        uint16_t slot=(uint16_t)*p++; slot|=(uint16_t)*p++<<8;\n"
      "        set_bkg_4bpp_data(slot,1u,p); p+=32u;\n"
      "    }\n"
      "}\n",
      b->count,bank);
    fclose(f);free(off);free(data);
}
static void emit_tilepatch_dispatch(const char *dir,const Bank *banks,unsigned bank_count){
    char path[512];FILE *f;unsigned i;
    path_join(path,sizeof(path),dir,"polar_demo_tilepatch_dispatch.c");f=fopen(path,"w");
    if(!f)die("cannot write tilepatch dispatcher");
    fprintf(f,"/* GENERATED baked composite tilepatch dispatcher. */\n"
              "#include <stdint.h>\n#include <gbdk/platform.h>\n\n");
    for(i=0u;i<MAX_TILEPATCH_BANKS;++i)
        fprintf(f,"void tsp_polar_demo_tilepatch_bank%u(uint16_t local) BANKED;\n",i);
    fprintf(f,"\nvoid tsp_polar_demo_tilepatch_apply(uint16_t patch){\n");
    for(i=0u;i<bank_count;++i){
        uint16_t end=(uint16_t)(banks[i].first+banks[i].count);
        fprintf(f,"    %s(patch<%uu){tsp_polar_demo_tilepatch_bank%u((uint16_t)(patch-%uu));return;}\n",
                i?"else if":"if",end,i,banks[i].first);
    }
    fprintf(f,"}\n");fclose(f);
}

int main(int argc,char **argv){
    const char *dir;
    Patch patches[PATCH_COUNT];
    TilePatch *tilepatches=(TilePatch *)calloc(PATCH_COUNT,sizeof(TilePatch));
    Bank banks[MAX_BANKS],tilebanks[MAX_TILEPATCH_BANKS];
    TSPState s;
    uint16_t prev[TSP_MAP_CELLS],cur[TSP_MAP_CELLS],base[TSP_MAP_CELLS];
    uint16_t *all_maps=(uint16_t *)malloc((size_t)PATCH_COUNT*TSP_MAP_CELLS*sizeof(uint16_t));
    FILE *refs,*camera_csv,*lighting_info;
    char refpath[512],camerapath[512],lightpath[512];
    uint64_t seq_hash=UINT64_C(1469598103934665603),total=0u;
    uint32_t zero=0u,changed_total=0u;
    uint16_t i;
    unsigned bank_count=0u,tilebank_count=0u,tile_vblank_budget=0u;
    PolarExploreCursor explore;

    if(argc<2||argc>3){fprintf(stderr,"usage: %s OUTPUT_DIR [baseline|ao|point|texture]\n",argv[0]);return 2;}
    if(!tilepatches||!all_maps)die("out of memory allocating bake tables");
    dir=argv[1];
    if(argc==3){g_lighting_name=argv[2];g_lighting_stage=parse_lighting_stage(argv[2]);}

    path_join(camerapath,sizeof(camerapath),dir,"camera_poses.csv");
    camera_csv=fopen(camerapath,"w");
    if(!camera_csv){fprintf(stderr,"cannot write %s: %s\n",camerapath,strerror(errno));return 2;}
    fprintf(camera_csv,"stage,frame,x_world,y_world,yaw_u8,yaw_deg\n");

    path_join(lightpath,sizeof(lightpath),dir,"lighting_stage.txt");
    lighting_info=fopen(lightpath,"w");
    if(!lighting_info){fprintf(stderr,"cannot write %s: %s\n",lightpath,strerror(errno));return 2;}
    fprintf(lighting_info,"stage=%s\n",g_lighting_name);
    if(g_lighting_stage==TSP_HOST_LIGHT_POINT)
        fprintf(lighting_info,"static_point_light_world=92.0,50.0 height=8.0 radius=76\n");
    if(g_lighting_stage==TSP_HOST_LIGHT_TEXTURE)
        fprintf(lighting_info,"wall_texture=TWBARLT1_2bpp.txt projection=ray-wall perspective snap=1/2/4/8 texels\n");
    fclose(lighting_info);

    tsp_reset(&s);polar_explore_cursor_reset(&explore);make_base(base);render_fresh(&s,cur);
    if(capture_frame(0u)){
        char shotpath[512];
        path_join(shotpath,sizeof(shotpath),dir,"host_composite_frame_0.ppm");
        if(!tsp_host_composite_write_ppm(shotpath))die("host composite screenshot write failed");
        fprintf(camera_csv,"%s,0,%.4f,%.4f,%u,%.3f\n",g_lighting_name,
                s.x_q4/16.0,s.y_q4/16.0,(unsigned)s.yaw,(double)s.yaw*360.0/256.0);
    }
    capture_tile_patch(&tilepatches[0]);
    patches[0].len=(uint16_t)build_patch(base,cur,patches[0].bytes,
                                         &patches[0].changed,&patches[0].runs);
    verify(base,cur,&patches[0]);
    memcpy(all_maps,cur,sizeof(cur));
    memcpy(prev,cur,sizeof(prev));

    path_join(refpath,sizeof(refpath),dir,"polar_demo_reference_maps.bin");
    refs=fopen(refpath,"wb");
    if(!refs){fprintf(stderr,"cannot write %s: %s\n",refpath,strerror(errno));return 2;}
    if(fwrite(cur,1,sizeof(cur),refs)!=sizeof(cur))die("reference-map write failed");
    seq_hash=fnv64(cur,sizeof(cur));

    for(i=1u;i<PATCH_COUNT;++i){
        size_t len;
        tsp_step(&s,polar_explore_next(&explore));render_fresh(&s,cur);
        if(capture_frame(i)){
            char shotname[96],shotpath[512];
            snprintf(shotname,sizeof(shotname),"host_composite_frame_%u.ppm",(unsigned)i);
            path_join(shotpath,sizeof(shotpath),dir,shotname);
            if(!tsp_host_composite_write_ppm(shotpath))die("host composite screenshot write failed");
            fprintf(camera_csv,"%s,%u,%.4f,%.4f,%u,%.3f\n",g_lighting_name,(unsigned)i,
                    s.x_q4/16.0,s.y_q4/16.0,(unsigned)s.yaw,(double)s.yaw*360.0/256.0);
        }
        capture_tile_patch(&tilepatches[i]);
        len=build_patch(prev,cur,patches[i].bytes,&patches[i].changed,&patches[i].runs);
        patches[i].len=(uint16_t)len;verify(prev,cur,&patches[i]);
        if(!patches[i].changed)++zero;
        changed_total+=patches[i].changed;
        if(fwrite(cur,1,sizeof(cur),refs)!=sizeof(cur))die("reference-map write failed");
        {
            uint64_t h=fnv64(cur,sizeof(cur));
            seq_hash^=h;seq_hash*=UINT64_C(1099511628211);
        }
        memcpy(all_maps+(size_t)i*TSP_MAP_CELLS,cur,sizeof(cur));
        memcpy(prev,cur,sizeof(prev));
    }
    fclose(refs);
    fclose(camera_csv);

    tile_vblank_budget=schedule_tilepatches(tilepatches,all_maps,dir);

    memset(banks,0,sizeof(banks));
    for(i=0u;i<PATCH_COUNT;){
        Bank *b;
        if(bank_count>=MAX_BANKS)die("demo patches exceed generated bank budget");
        b=&banks[bank_count];b->first=i;
        while(i<PATCH_COUNT){
            uint32_t next=b->bytes+patches[i].len;
            if(b->count&&next>MAX_BANK_STREAM)break;
            if(next>MAX_BANK_STREAM)die("single patch exceeds bank stream cap");
            b->bytes=next;++b->count;++i;
        }
        ++bank_count;
    }
    for(i=0u;i<MAX_BANKS;++i){
        Bank empty={0u,0u,0u};
        emit_bank(dir,i,patches,i<bank_count?&banks[i]:&empty);
    }
    emit_dispatch(dir,banks,bank_count);
    emit_meta(dir,banks,bank_count,seq_hash);
    emit_manifest(dir,patches,banks,bank_count,seq_hash);

    memset(tilebanks,0,sizeof(tilebanks));
    for(i=0u;i<PATCH_COUNT;){
        Bank *b;
        if(tilebank_count>=MAX_TILEPATCH_BANKS)die("tilepatches exceed generated bank budget");
        b=&tilebanks[tilebank_count];b->first=i;
        while(i<PATCH_COUNT){
            uint32_t next=b->bytes+tilepatches[i].len;
            if(b->count&&next>MAX_TILEPATCH_BANK_STREAM)break;
            if(next>MAX_TILEPATCH_BANK_STREAM)die("single tilepatch exceeds bank stream cap");
            b->bytes=next;++b->count;++i;
        }
        ++tilebank_count;
    }
    for(i=0u;i<MAX_TILEPATCH_BANKS;++i){
        Bank empty={0u,0u,0u};
        emit_tilepatch_bank(dir,i,tilepatches,i<tilebank_count?&tilebanks[i]:&empty);
    }
    emit_tilepatch_dispatch(dir,tilebanks,tilebank_count);

    for(i=0u;i<PATCH_COUNT;++i)total+=patches[i].len;
    printf("=== POLAR PLAYER-LIKE EXPLORE PATCH GENERATOR ===\n");
    printf("lighting_stage=%s (%u)\n",g_lighting_name,(unsigned)g_lighting_stage);
    printf("patches=%u initial+%u; raw_stream=%" PRIu64 " bytes; banks=%u cap=%u\n",
           PATCH_COUNT,DEMO_FRAMES,total,bank_count,MAX_BANK_STREAM);
    printf("motion transitions: zero=%" PRIu32 "/%u (%.2f%%); mean_changed=%.2f words\n",
           zero,DEMO_FRAMES,100.0*(double)zero/DEMO_FRAMES,
           (double)changed_total/DEMO_FRAMES);
    for(i=0u;i<bank_count;++i)
        printf("  bank%u first=%u count=%u stream=%" PRIu32 " bytes\n",
               i,banks[i].first,banks[i].count,banks[i].bytes);
    printf("generated runtime sources + exact %u-map exploration reference sequence; replay=PASS\n", PATCH_COUNT);
    printf("baked composite cache: peak_unique/frame=%u original_peak_loads=%u total_tile_loads=%" PRIu32 " scheduled_vblank_budget=%u tilepatch_banks=%u\n",
           (unsigned)tsp_host_composite_peak_unique_count(),
           (unsigned)tsp_host_composite_peak_load_count(),
           tsp_host_composite_total_load_count(),tile_vblank_budget,tilebank_count);
    printf("final state=(%.2f,%.2f) yaw=%u\n",s.x_q4/16.0,s.y_q4/16.0,s.yaw);
    for(i=0u;i<PATCH_COUNT;++i)free(tilepatches[i].bytes);
    free(tilepatches);free(all_maps);
    return 0;
}
