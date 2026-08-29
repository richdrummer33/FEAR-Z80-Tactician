/*
 * Host-side exact name-table transition baker / entropy probe.
 *
 * Phase A deliberately optimizes NOTHING for ROM size.  It asks the current
 * Polar host renderer (the correctness oracle) for final 20x18 GG name tables,
 * deduplicates identical display states, compiles exact A->B row-run patches,
 * and proves every patch by replaying it back to B.
 *
 * Spatial nodes are the centres of the existing 4-world-unit Polar cells.
 * Yaw is exhaustive by default (all 256 values).  This is intentionally a
 * coarse spatial state graph / ROM-entropy experiment, NOT yet the final
 * sub-cell event-boundary representation.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar.h"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64
#define ACTION_COUNT 6u
#define NO_ID UINT32_MAX
#define PATCH_SCRATCH_MAX (1u + TSP_ROWS * (3u + TSP_COLS * 2u))

enum Action {
    ACT_X_NEG=0, ACT_X_POS=1, ACT_Y_NEG=2, ACT_Y_POS=3,
    ACT_TURN_NEG=4, ACT_TURN_POS=5
};

static const char *k_action_name[ACTION_COUNT] = {
    "x-", "x+", "y-", "y+", "turn-", "turn+"
};

typedef struct Node {
    uint8_t gx, gy;
    int16_t xq, yq;
} Node;

typedef struct DisplayState {
    uint64_t hash;
    uint16_t words[TSP_MAP_CELLS];
} DisplayState;

typedef struct PatchMeta {
    uint64_t hash;
    uint32_t off;
    uint16_t len;
    uint16_t changed;
    uint8_t runs;
} PatchMeta;

typedef struct Edge {
    uint32_t target_pose;
    uint32_t patch_id;
} Edge;

typedef struct StressPoint {
    const char *name;
    int16_t xq, yq;
} StressPoint;

typedef struct Options {
    uint8_t appearance;
    uint16_t yaw_step;
    const char *emit_path;
} Options;

static void die(const char *msg) {
    fprintf(stderr, "fatal: %s\n", msg);
    exit(2);
}
static void *xmalloc(size_t n) {
    void *p=malloc(n?n:1u);
    if(!p) die("out of memory");
    return p;
}
static void *xcalloc(size_t n,size_t s) {
    void *p=calloc(n?n:1u,s?s:1u);
    if(!p) die("out of memory");
    return p;
}
static void *xrealloc(void *p,size_t n) {
    void *q=realloc(p,n?n:1u);
    if(!q) die("out of memory");
    return q;
}
static uint32_t next_pow2_u32(uint32_t v) {
    uint32_t p=1u;
    while(p<v) {
        if(p>0x80000000u) die("hash table size overflow");
        p<<=1;
    }
    return p;
}
static uint64_t fnv1a64(const void *data,size_t n) {
    const uint8_t *p=(const uint8_t *)data;
    uint64_t h=UINT64_C(1469598103934665603);
    size_t i;
    for(i=0;i<n;++i){h^=p[i];h*=UINT64_C(1099511628211);}
    return h;
}
static uint32_t hash_slot(uint64_t h,uint32_t mask) {
    h^=h>>33;h*=UINT64_C(0xff51afd7ed558ccd);h^=h>>33;
    return (uint32_t)h&mask;
}

static void state_at(TSPState *s,int16_t xq,int16_t yq,uint8_t yaw) {
    memset(s,0,sizeof(*s));
    s->x_q4=xq;s->y_q4=yq;s->yaw=yaw;s->speed_scale=1u;
}
static int map_valid(const uint16_t map[TSP_MAP_CELLS]) {
    uint16_t i;
    for(i=0;i<TSP_MAP_CELLS;++i)
        if((map[i]&TSP_TILE_ID_MASK)>=TSP_GENERATED_TILE_COUNT) return 0;
    return 1;
}
static void render_fresh(int16_t xq,int16_t yq,uint8_t yaw,uint8_t appearance,
                         uint16_t out[TSP_MAP_CELLS]) {
    TSPState s;
    memset(out,0,sizeof(uint16_t)*TSP_MAP_CELLS);
    state_at(&s,xq,yq,yaw);
    g_tspf_appearance_mode=appearance;
    tsp_polar_renderer_reset();
    tsp_polar_render(&s,out,(TSPColumn *)0);
    if(!map_valid(out)) die("oracle emitted invalid tile id");
}

static uint16_t display_find_or_add(
    DisplayState *states,uint32_t *count,uint32_t max_count,
    uint32_t *slots,uint32_t slot_cap,
    const uint16_t words[TSP_MAP_CELLS])
{
    uint64_t h=fnv1a64(words,sizeof(uint16_t)*TSP_MAP_CELLS);
    uint32_t mask=slot_cap-1u,pos=hash_slot(h,mask);
    for(;;){
        uint32_t v=slots[pos];
        if(v==0u){
            uint32_t id=*count;
            if(id>=max_count)die("display-state capacity exceeded");
            states[id].hash=h;
            memcpy(states[id].words,words,sizeof(states[id].words));
            slots[pos]=id+1u;*count=id+1u;
            return id;
        }
        {
            uint32_t id=v-1u;
            if(states[id].hash==h &&
               memcmp(states[id].words,words,sizeof(states[id].words))==0)
                return id;
        }
        pos=(pos+1u)&mask;
    }
}

static size_t build_patch(const uint16_t *a,const uint16_t *b,uint8_t *dst,
                          uint16_t *changed_out,uint8_t *runs_out) {
    size_t p=1u;
    uint16_t changed=0u;
    uint8_t runs=0u,row;
    for(row=0u;row<TSP_ROWS;++row){
        uint8_t x=0u;
        uint16_t base=(uint16_t)row*TSP_COLS;
        while(x<TSP_COLS){
            uint8_t start,len,c;
            while(x<TSP_COLS && a[base+x]==b[base+x])++x;
            if(x>=TSP_COLS)break;
            start=x;
            while(x<TSP_COLS && a[base+x]!=b[base+x])++x;
            len=(uint8_t)(x-start);
            if(p+3u+(size_t)len*2u>PATCH_SCRATCH_MAX)die("patch scratch overflow");
            dst[p++]=row;dst[p++]=start;dst[p++]=len;
            for(c=0u;c<len;++c){
                uint16_t w=b[base+(uint16_t)start+c];
                dst[p++]=(uint8_t)w;dst[p++]=(uint8_t)(w>>8);
            }
            changed=(uint16_t)(changed+len);
            ++runs;
        }
    }
    dst[0]=runs;
    *changed_out=changed;*runs_out=runs;
    return p;
}
static int apply_patch(uint16_t *map,const uint8_t *src,size_t len) {
    size_t p=1u;
    uint8_t n,i;
    if(len<1u)return 0;
    n=src[0];
    for(i=0u;i<n;++i){
        uint8_t row,x,count,c;
        uint16_t base;
        if(p+3u>len)return 0;
        row=src[p++];x=src[p++];count=src[p++];
        if(row>=TSP_ROWS||x>=TSP_COLS||count==0u||(uint16_t)x+count>TSP_COLS)return 0;
        if(p+(size_t)count*2u>len)return 0;
        base=(uint16_t)row*TSP_COLS;
        for(c=0u;c<count;++c){
            uint16_t w=(uint16_t)src[p]|((uint16_t)src[p+1u]<<8);
            p+=2u;map[base+(uint16_t)x+c]=w;
        }
    }
    return p==len;
}
static void verify_patch_or_die(const uint16_t *a,const uint16_t *b,
                                const uint8_t *patch,size_t len) {
    uint16_t replay[TSP_MAP_CELLS];
    memcpy(replay,a,sizeof(replay));
    if(!apply_patch(replay,patch,len))die("patch parser rejected generated patch");
    if(memcmp(replay,b,sizeof(replay))!=0)die("patch replay != oracle target");
}

static void pool_reserve(uint8_t **pool,size_t *cap,size_t need) {
    size_t n=*cap?*cap:(1u<<20);
    if(need<=*cap)return;
    while(n<need){
        if(n>SIZE_MAX/2u)die("patch pool overflow");
        n*=2u;
    }
    *pool=(uint8_t *)xrealloc(*pool,n);*cap=n;
}
static uint32_t patch_find_or_add(
    PatchMeta *meta,uint32_t *count,uint32_t max_count,
    uint32_t *slots,uint32_t slot_cap,
    uint8_t **pool,size_t *pool_len,size_t *pool_cap,
    const uint8_t *bytes,uint16_t len,uint16_t changed,uint8_t runs)
{
    uint64_t h=fnv1a64(bytes,len);
    uint32_t mask=slot_cap-1u,pos=hash_slot(h,mask);
    for(;;){
        uint32_t v=slots[pos];
        if(v==0u){
            uint32_t id=*count;
            if(id>=max_count)die("patch capacity exceeded");
            pool_reserve(pool,pool_cap,*pool_len+len);
            memcpy(*pool+*pool_len,bytes,len);
            meta[id].hash=h;meta[id].off=(uint32_t)*pool_len;meta[id].len=len;
            meta[id].changed=changed;meta[id].runs=runs;
            *pool_len+=len;slots[pos]=id+1u;*count=id+1u;
            return id;
        }
        {
            uint32_t id=v-1u;
            if(meta[id].hash==h && meta[id].len==len &&
               memcmp(*pool+meta[id].off,bytes,len)==0)
                return id;
        }
        pos=(pos+1u)&mask;
    }
}

static int cmp_u16(const void *a,const void *b) {
    uint16_t x=*(const uint16_t *)a,y=*(const uint16_t *)b;
    return (x>y)-(x<y);
}
static uint16_t percentile_u16(uint16_t *v,uint32_t n,unsigned pct) {
    uint64_t rank;
    if(!n)return 0u;
    rank=((uint64_t)(n-1u)*pct+50u)/100u;
    return v[(uint32_t)rank];
}

static int write_u8(FILE *f,uint8_t v){return fputc(v,f)==EOF?-1:0;}
static int write_u16(FILE *f,uint16_t v){
    return write_u8(f,(uint8_t)v)||write_u8(f,(uint8_t)(v>>8))?-1:0;
}
static int write_u32(FILE *f,uint32_t v){
    return write_u16(f,(uint16_t)v)||write_u16(f,(uint16_t)(v>>16))?-1:0;
}
static int emit_pack(const char *path,const Node *nodes,uint16_t node_count,
                     uint16_t yaw_slots,uint16_t yaw_step,uint8_t appearance,
                     const uint32_t *pose_display,uint32_t pose_count,
                     uint32_t display_count,const Edge *edges,uint32_t edge_slots,
                     uint32_t legal_edges,const PatchMeta *patches,uint32_t patch_count,
                     const uint8_t *pool,uint32_t pool_len) {
    FILE *f=fopen(path,"wb");
    uint32_t i;
    if(!f){fprintf(stderr,"cannot open %s: %s\n",path,strerror(errno));return 0;}
#define W(expr) do{if((expr)<0){fclose(f);return 0;}}while(0)
    if(fwrite("PTP1",1,4,f)!=4){fclose(f);return 0;}
    W(write_u16(f,1u));W(write_u16(f,TSP_COLS));W(write_u16(f,TSP_ROWS));
    W(write_u16(f,node_count));W(write_u16(f,yaw_slots));W(write_u16(f,yaw_step));
    W(write_u8(f,appearance));W(write_u8(f,ACTION_COUNT));
    W(write_u32(f,pose_count));W(write_u32(f,display_count));W(write_u32(f,patch_count));
    W(write_u32(f,legal_edges));W(write_u32(f,edge_slots));W(write_u32(f,pool_len));
    for(i=0u;i<node_count;++i){
        W(write_u8(f,nodes[i].gx));W(write_u8(f,nodes[i].gy));
        W(write_u16(f,(uint16_t)nodes[i].xq));W(write_u16(f,(uint16_t)nodes[i].yq));
    }
    for(i=0u;i<pose_count;++i)W(write_u32(f,pose_display[i]));
    for(i=0u;i<edge_slots;++i){
        W(write_u32(f,edges[i].target_pose));W(write_u32(f,edges[i].patch_id));
    }
    for(i=0u;i<patch_count;++i){
        W(write_u32(f,patches[i].off));W(write_u16(f,patches[i].len));
        W(write_u16(f,patches[i].changed));W(write_u8(f,patches[i].runs));W(write_u8(f,0u));
    }
    if(pool_len && fwrite(pool,1,pool_len,f)!=pool_len){fclose(f);return 0;}
    if(fclose(f)!=0)return 0;
#undef W
    return 1;
}

static uint32_t stress_pair(int16_t ax,int16_t ay,uint8_t aa,
                            int16_t bx,int16_t by,uint8_t ba,uint8_t appearance) {
    uint16_t a[TSP_MAP_CELLS],b[TSP_MAP_CELLS];
    uint8_t patch[PATCH_SCRATCH_MAX];
    uint16_t changed;uint8_t runs;size_t len;
    render_fresh(ax,ay,aa,appearance,a);render_fresh(bx,by,ba,appearance,b);
    len=build_patch(a,b,patch,&changed,&runs);
    verify_patch_or_die(a,b,patch,len);
    return changed;
}

static void run_stress_suite(uint8_t appearance) {
    static const StressPoint pts[]={
        {"left-min-wall",20*16,20*16},
        {"left-max-q4",(77*16)-1,(77*16)-1},
        {"left-centre",48*16,48*16},
        {"left-east-wall",76*16,48*16},
        {"portal-A-before",(80*16)-1,50*16},
        {"portal-A-line",80*16,50*16},
        {"portal-A-after",(80*16)+1,50*16},
        {"connector-centre",96*16,50*16},
        {"portal-B-before",(112*16)-1,50*16},
        {"portal-B-line",112*16,50*16},
        {"portal-B-after",(112*16)+1,50*16},
        {"right-centre",142*16,49*16},
        {"right-min-wall",112*16,20*16},
        {"right-max-q4",(173*16)-1,(79*16)-1}
    };
    uint32_t checks=0u,blocked=0u,direct=0u;
    size_t i;
    for(i=0u;i<sizeof(pts)/sizeof(pts[0]);++i){
        uint16_t yaw;
        if(!tsp_is_walkable_q4(pts[i].xq,pts[i].yq)){
            fprintf(stderr,"stress point unexpectedly non-walkable: %s\n",pts[i].name);
            exit(3);
        }
        for(yaw=0u;yaw<256u;yaw+=16u){
            static const int8_t ox[4]={-1,1,0,0};
            static const int8_t oy[4]={0,0,-1,1};
            unsigned k;
            (void)stress_pair(pts[i].xq,pts[i].yq,(uint8_t)yaw,
                              pts[i].xq,pts[i].yq,(uint8_t)(yaw+1u),appearance);
            ++checks;
            for(k=0u;k<4u;++k){
                int16_t nx=(int16_t)(pts[i].xq+ox[k]),ny=(int16_t)(pts[i].yq+oy[k]);
                if(tsp_is_walkable_q4(nx,ny)){
                    (void)stress_pair(pts[i].xq,pts[i].yq,(uint8_t)yaw,nx,ny,(uint8_t)yaw,appearance);
                    ++checks;
                }else ++blocked;
            }
        }
    }
    /* Direct, non-adjacent jumps prove the patch codec itself is not relying
     * on intermediate nodes. Runtime legality/physics remains a separate job. */
    (void)stress_pair(48*16,48*16,0u,80*16,50*16,7u,appearance);++direct;
    (void)stress_pair(80*16,50*16,7u,96*16,50*16,64u,appearance);++direct;
    (void)stress_pair(96*16,50*16,64u,112*16,50*16,127u,appearance);++direct;
    (void)stress_pair(112*16,50*16,127u,142*16,49*16,192u,appearance);++direct;

    /* Lifetime/order sanity: precompute independent references, then jump
     * through the same states without resetting the host renderer. */
    {
        enum { NSEQ=12 };
        uint16_t refs[NSEQ][TSP_MAP_CELLS],seq[TSP_MAP_CELLS];
        TSPState s;
        int q;
        memset(seq,0,sizeof(seq));
        for(q=0;q<NSEQ;++q){
            const StressPoint *sp=&pts[(size_t)q%(sizeof(pts)/sizeof(pts[0]))];
            render_fresh(sp->xq,sp->yq,(uint8_t)(q*37u),appearance,refs[q]);
        }
        g_tspf_appearance_mode=appearance;tsp_polar_renderer_reset();
        for(q=0;q<NSEQ;++q){
            const StressPoint *sp=&pts[(size_t)q%(sizeof(pts)/sizeof(pts[0]))];
            state_at(&s,sp->xq,sp->yq,(uint8_t)(q*37u));
            tsp_polar_render(&s,seq,(TSPColumn *)0);
            if(memcmp(seq,refs[q],sizeof(seq))!=0)die("host oracle sequence != fresh render");
        }
    }
    printf("stress: named_points=%zu micro/turn_replay_checks=%" PRIu32
           " blocked_micro_steps=%" PRIu32 " direct_jump_replays=%" PRIu32
           " lifetime_jump_sequence=PASS\n",
           sizeof(pts)/sizeof(pts[0]),checks,blocked,direct);
}

static void usage(const char *argv0) {
    fprintf(stderr,
      "usage: %s [--appearance 0|1|2] [--yaw-step N] [--emit PATH]\n"
      "  default yaw-step=1 (all 256 yaws); N must divide 256\n",argv0);
}
static Options parse_args(int argc,char **argv) {
    Options o;int i;
    o.appearance=0u;o.yaw_step=1u;o.emit_path=(const char *)0;
    for(i=1;i<argc;++i){
        if(strcmp(argv[i],"--appearance")==0&&i+1<argc){
            long v=strtol(argv[++i],0,0);if(v<0||v>2)die("bad --appearance");
            o.appearance=(uint8_t)v;
        }else if(strcmp(argv[i],"--yaw-step")==0&&i+1<argc){
            long v=strtol(argv[++i],0,0);
            if(v<1||v>256||(256%v)!=0)die("--yaw-step must divide 256");
            o.yaw_step=(uint16_t)v;
        }else if(strcmp(argv[i],"--emit")==0&&i+1<argc){
            o.emit_path=argv[++i];
        }else {usage(argv[0]);exit(2);}
    }
    return o;
}

int main(int argc,char **argv) {
    Options opt=parse_args(argc,argv);
    int32_t node_at[GRID_H][GRID_W];
    Node nodes[GRID_W*GRID_H];
    uint16_t node_count=0u,yaw_slots=(uint16_t)(256u/opt.yaw_step);
    uint32_t pose_count,display_count=0u,display_slot_cap;
    DisplayState *displays;
    uint32_t *display_slots,*pose_display;
    Edge *edges;
    uint32_t edge_slots,max_patches,patch_slot_cap,*patch_slots,patch_count=0u;
    PatchMeta *patches;
    uint8_t *patch_pool=0,*scratch=(uint8_t *)xmalloc(PATCH_SCRATCH_MAX);
    size_t patch_pool_len=0u,patch_pool_cap=0u;
    uint16_t *changed_values,*run_values;
    uint32_t legal_edges=0u,zero_edges=0u;
    uint64_t changed_sum=0u,run_sum=0u;
    uint32_t action_edges[ACTION_COUNT]={0},action_zero[ACTION_COUNT]={0};
    uint64_t action_changed[ACTION_COUNT]={0};
    uint32_t n,pidx;
    uint8_t gx,gy;
    uint32_t estimated_pack;
    FILE *probe;

    memset(node_at,0xff,sizeof(node_at));
    for(gy=0u;gy<GRID_H;++gy)for(gx=0u;gx<GRID_W;++gx){
        int16_t xq=(int16_t)((int16_t)gx*CELL_Q4+CELL_Q4/2);
        int16_t yq=(int16_t)((int16_t)gy*CELL_Q4+CELL_Q4/2);
        if(tsp_is_walkable_q4(xq,yq)){
            Node *nd=&nodes[node_count];
            nd->gx=gx;nd->gy=gy;nd->xq=xq;nd->yq=yq;
            node_at[gy][gx]=(int32_t)node_count++;
        }
    }
    pose_count=(uint32_t)node_count*yaw_slots;
    if(!pose_count)die("no walkable nodes");
    displays=(DisplayState *)xmalloc((size_t)pose_count*sizeof(*displays));
    display_slot_cap=next_pow2_u32(pose_count*2u+1u);
    display_slots=(uint32_t *)xcalloc(display_slot_cap,sizeof(*display_slots));
    pose_display=(uint32_t *)xmalloc((size_t)pose_count*sizeof(*pose_display));

    printf("=== POLAR EXACT TRANSITION PATCH BAKE V1 ===\n");
    printf("oracle=host Polar final 20x18 name table; appearance=%u\n",opt.appearance);
    printf("spatial sampling=existing 4-world-unit Polar cell centres; nodes=%u; yaw_step=%u; yaw_states=%u\n",
           node_count,opt.yaw_step,yaw_slots);
    printf("NOTE: Phase A is intentionally NOT the final sub-cell event-boundary graph and performs no ROM-size optimization.\n");

    for(n=0u;n<node_count;++n){
        uint16_t ys;
        for(ys=0u;ys<yaw_slots;++ys){
            uint16_t map[TSP_MAP_CELLS];
            uint8_t yaw=(uint8_t)(ys*opt.yaw_step);
            uint32_t id;
            render_fresh(nodes[n].xq,nodes[n].yq,yaw,opt.appearance,map);
            id=display_find_or_add(displays,&display_count,pose_count,
                                   display_slots,display_slot_cap,map);
            pose_display[n*(uint32_t)yaw_slots+ys]=id;
        }
    }
    printf("layer2/display: poses=%" PRIu32 " unique_exact_name_tables=%" PRIu32
           " dedup=%.2f%%\n",pose_count,display_count,
           100.0*(1.0-(double)display_count/(double)pose_count));

    edge_slots=pose_count*ACTION_COUNT;
    edges=(Edge *)xmalloc((size_t)edge_slots*sizeof(*edges));
    for(pidx=0u;pidx<edge_slots;++pidx){edges[pidx].target_pose=NO_ID;edges[pidx].patch_id=NO_ID;}
    changed_values=(uint16_t *)xmalloc((size_t)edge_slots*sizeof(*changed_values));
    run_values=(uint16_t *)xmalloc((size_t)edge_slots*sizeof(*run_values));
    max_patches=edge_slots+1u;
    patches=(PatchMeta *)xmalloc((size_t)max_patches*sizeof(*patches));
    patch_slot_cap=next_pow2_u32(max_patches*2u+1u);
    patch_slots=(uint32_t *)xcalloc(patch_slot_cap,sizeof(*patch_slots));

    /* Reserve/deduplicate patch zero first: one-byte stream, zero runs. */
    {
        uint8_t empty=0u;
        uint32_t id=patch_find_or_add(patches,&patch_count,max_patches,patch_slots,patch_slot_cap,
                                      &patch_pool,&patch_pool_len,&patch_pool_cap,
                                      &empty,1u,0u,0u);
        if(id!=0u)die("empty patch did not become patch 0");
    }

    for(n=0u;n<node_count;++n){
        uint16_t ys;
        for(ys=0u;ys<yaw_slots;++ys){
            uint32_t src_pose=n*(uint32_t)yaw_slots+ys;
            uint32_t src_display=pose_display[src_pose];
            unsigned act;
            for(act=0u;act<ACTION_COUNT;++act){
                int32_t tn=(int32_t)n;
                uint16_t tys=ys;
                uint32_t target_pose,target_display,patch_id;
                uint16_t changed;uint8_t runs;
                size_t plen;
                if(act==ACT_X_NEG){ if(nodes[n].gx==0u)tn=-1; else tn=node_at[nodes[n].gy][nodes[n].gx-1u]; }
                else if(act==ACT_X_POS){ if(nodes[n].gx+1u>=GRID_W)tn=-1; else tn=node_at[nodes[n].gy][nodes[n].gx+1u]; }
                else if(act==ACT_Y_NEG){ if(nodes[n].gy==0u)tn=-1; else tn=node_at[nodes[n].gy-1u][nodes[n].gx]; }
                else if(act==ACT_Y_POS){ if(nodes[n].gy+1u>=GRID_H)tn=-1; else tn=node_at[nodes[n].gy+1u][nodes[n].gx]; }
                else if(act==ACT_TURN_NEG){tys=(uint16_t)((ys+yaw_slots-1u)%yaw_slots);}
                else {tys=(uint16_t)((ys+1u)%yaw_slots);}
                if(tn<0)continue;
                target_pose=(uint32_t)tn*(uint32_t)yaw_slots+tys;
                target_display=pose_display[target_pose];
                plen=build_patch(displays[src_display].words,displays[target_display].words,
                                 scratch,&changed,&runs);
                verify_patch_or_die(displays[src_display].words,displays[target_display].words,
                                    scratch,plen);
                patch_id=patch_find_or_add(patches,&patch_count,max_patches,patch_slots,patch_slot_cap,
                                           &patch_pool,&patch_pool_len,&patch_pool_cap,
                                           scratch,(uint16_t)plen,changed,runs);
                edges[src_pose*ACTION_COUNT+act].target_pose=target_pose;
                edges[src_pose*ACTION_COUNT+act].patch_id=patch_id;
                changed_values[legal_edges]=changed;run_values[legal_edges]=runs;
                ++legal_edges;changed_sum+=changed;run_sum+=runs;
                ++action_edges[act];action_changed[act]+=changed;
                if(changed==0u){++zero_edges;++action_zero[act];if(patch_id!=0u)die("zero delta not patch 0");}
            }
        }
    }
    qsort(changed_values,legal_edges,sizeof(*changed_values),cmp_u16);
    qsort(run_values,legal_edges,sizeof(*run_values),cmp_u16);

    printf("layer1/world adjacency: directed_legal_edges=%" PRIu32
           " (fixed slots=%" PRIu32 ")\n",legal_edges,edge_slots);
    printf("layer2/quantization: zero_delta_edges=%" PRIu32 " (%.2f%%)\n",
           zero_edges,100.0*(double)zero_edges/(double)legal_edges);
    printf("layer3/output patches: changed_words mean=%.2f median=%u p95=%u max=%u; "
           "row_runs mean=%.2f median=%u p95=%u max=%u\n",
           (double)changed_sum/(double)legal_edges,
           percentile_u16(changed_values,legal_edges,50u),
           percentile_u16(changed_values,legal_edges,95u),
           percentile_u16(changed_values,legal_edges,100u),
           (double)run_sum/(double)legal_edges,
           percentile_u16(run_values,legal_edges,50u),
           percentile_u16(run_values,legal_edges,95u),
           percentile_u16(run_values,legal_edges,100u));
    for(n=0u;n<ACTION_COUNT;++n){
        printf("  action %-5s edges=%" PRIu32 " zero=%6.2f%% mean_changed=%6.2f\n",
               k_action_name[n],action_edges[n],
               action_edges[n]?100.0*(double)action_zero[n]/action_edges[n]:0.0,
               action_edges[n]?(double)action_changed[n]/action_edges[n]:0.0);
    }
    printf("patch dictionary: unique=%" PRIu32 " (including empty) reuse=%.2fx stream_bytes=%zu\n",
           patch_count,(double)legal_edges/(double)patch_count,patch_pool_len);

    run_stress_suite(opt.appearance);

    /* Component estimate equals the explicit PTP1 writer below. */
    estimated_pack=(uint32_t)(
        42u + (uint32_t)node_count*6u + pose_count*4u + edge_slots*8u +
        patch_count*10u + (uint32_t)patch_pool_len);
    printf("PTP1 uncompressed Phase-A pack estimate: %" PRIu32 " bytes (%.2f KiB, %.2f MiB)\n",
           estimated_pack,estimated_pack/1024.0,estimated_pack/(1024.0*1024.0));
    printf("  components: header=42 nodes=%" PRIu32 " pose->display=%" PRIu32
           " edge_slots=%" PRIu32 " patch_dir=%" PRIu32 " patch_stream=%zu\n",
           (uint32_t)node_count*6u,pose_count*4u,edge_slots*8u,patch_count*10u,patch_pool_len);
    printf("  IMPORTANT: display tables themselves are NOT stored in PTP1; they are oracle-only bake intermediates.\n");

    if(opt.emit_path){
        if(!emit_pack(opt.emit_path,nodes,node_count,yaw_slots,opt.yaw_step,opt.appearance,
                      pose_display,pose_count,display_count,edges,edge_slots,legal_edges,
                      patches,patch_count,patch_pool,(uint32_t)patch_pool_len))
            die("failed to emit PTP1 pack");
        probe=fopen(opt.emit_path,"rb");
        if(probe){
            long sz;fseek(probe,0,SEEK_END);sz=ftell(probe);fclose(probe);
            printf("emitted %s bytes=%ld\n",opt.emit_path,sz);
            if(sz>=0 && (uint32_t)sz!=estimated_pack)die("emitted pack size != estimate");
        }
    }
    printf("verification: every graph patch replay == exact oracle target; stress/extents/portal suite PASS\n");

    free(displays);free(display_slots);free(pose_display);free(edges);
    free(changed_values);free(run_values);free(patches);free(patch_slots);
    free(patch_pool);free(scratch);
    return 0;
}
