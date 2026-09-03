/*
 * Hero dense-corpus analyzer.
 *
 * Reads a DHC1 corpus (see bake_hero_dense_corpus in room_bundle_poc_gen.c)
 * and answers the questions that have to be settled BEFORE a runtime codec
 * layout is chosen:
 *
 *   * how much does the hero actually change per angular step, once camera
 *     placement has been removed analytically rather than searched out?
 *   * of the candidate motion-compensation predictors, which one buys enough
 *     accuracy to justify its per-view side information?
 *   * how many sparse anchors does a band need before the worst predicted view
 *     is good enough, and how does that curve bend?
 *   * what does one angular step cost the VDP regardless of any codec?
 *
 * Everything is measured in a common local frame. Each sample's placement
 * anchor is exact (the corpus guarantees the pivot projects to screen x=80.0
 * and to a per-band constant y), so "normalize to the anchor" is a constant
 * integer translation, not a registration search. Whatever difference remains
 * between two normalized samples is genuinely a difference in appearance.
 *
 * Silhouette is weighted hardest throughout: on a five-stop ramp a wrong shade
 * reads as slightly odd lighting, whereas a wrong silhouette reads as a
 * different object.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Local (anchor-relative) canvas. Wide enough for every band, with the origin
 * placed so the hero's extent -- which reaches far above the anchor and only a
 * little below it -- lands inside without clamping. */
#define LW 192
#define LH 192
#define LOX 64   /* local x = column - LOX */
#define LOY 144  /* local y = row - LOY   */
#define LWORDS 3
#define MAX_SHIFT 40

#define SCREEN_W 160
#define SCREEN_H 144
#define TILE_COLS (SCREEN_W/8)
#define TILE_ROWS (SCREEN_H/8)
#define TILE_COUNT (TILE_COLS*TILE_ROWS)
/* Sustained pattern uploads per VBlank measured by the streaming PoC. */
#define VBLANK_TILE_BUDGET 48u

enum { PRED_P0=0,PRED_P1,PRED_P2,PRED_P3,PRED_P4,PRED_COUNT };
static const char *k_pred_name[PRED_COUNT]={
    "P0_nearest_anchor","P1_global_shift","P2_row_span_morph",
    "P3_two_region_shift","P4_four_region_shift"
};
/* Per-view side information each predictor must ship next to the anchor id. */
static const char *k_pred_side[PRED_COUNT]={
    "0 bytes","2 bytes","2 bytes per occupied row","4 bytes","8 bytes"
};

typedef struct {
    uint16_t angle;
    uint8_t band,yaw;
    int32_t cam_x_q8,cam_y_q8;
    int16_t anchor_x_q8,anchor_y_q8;
    uint8_t sx0,sy0,sx1,sy1;
    uint16_t pixels;
    int anchor_x,anchor_y;     /* rounded screen anchor */
    int lx0,ly0,lx1,ly1;       /* local bbox, inclusive */
    uint64_t (*rows)[LWORDS];  /* LH rows of silhouette bits */
    uint8_t *plane;            /* LH*LW semantic bytes, 0 = not the hero */
    int16_t *span_l,*span_r;   /* per local row, -1 when the row is empty */
    uint16_t occupied_rows;
} Sample;

typedef struct {
    uint32_t mask_xor,mask_union;
    uint32_t shade_bad,overlap;
    uint32_t tiles;      /* tiles needing re-upload: any pixel differs */
    uint32_t tiles_mask; /* tiles whose SILHOUETTE differs, shade ignored */
} Err;

static void die(const char *msg){fprintf(stderr,"fatal: %s\n",msg);exit(1);}

#if defined(__GNUC__)
#define popcount64(v) __builtin_popcountll(v)
#else
static int popcount64(uint64_t v){
    int n=0;
    while(v){v&=v-1u;++n;}
    return n;
}
#endif

/* Shift one 192-bit row. Positive dx moves the image toward larger x. */
static void row_shift(const uint64_t *in,uint64_t *out,int dx){
    int i;
    if(dx==0){for(i=0;i<LWORDS;++i)out[i]=in[i];return;}
    if(dx>0){
        for(i=LWORDS-1;i>=0;--i){
            uint64_t v=in[i]<<dx;
            if(i>0)v|=in[i-1]>>(64-dx);
            out[i]=v;
        }
    }else{
        int s=-dx;
        for(i=0;i<LWORDS;++i){
            uint64_t v=in[i]>>s;
            if(i+1<LWORDS)v|=in[i+1]<<(64-s);
            out[i]=v;
        }
    }
}

static const uint64_t k_zero_row[LWORDS]={0u,0u,0u};

static double pct(uint32_t n,uint32_t d){return d?100.0*(double)n/(double)d:0.0;}


/* Silhouette-only cost of predicting rows [y0,y1] of t from a shifted a.
 * Bitwise, so the shift search below stays cheap enough to run over every
 * ordered pair in a band. */
static uint32_t shift_cost(const Sample *a,const Sample *t,
                           int dx,int dy,int y0,int y1){
    uint64_t buf[LWORDS];
    uint32_t xr=0u;
    int y;
    for(y=y0;y<=y1;++y){
        const uint64_t *ar;
        int ay=y-dy,i;
        ar=(ay>=0&&ay<LH)?a->rows[ay]:k_zero_row;
        row_shift(ar,buf,dx);
        for(i=0;i<LWORDS;++i)xr+=(uint32_t)popcount64(buf[i]^t->rows[y][i]);
    }
    return xr;
}

/* Seed the search from the bounding-box centre offset, then refine locally.
 * The corpus already removed camera placement, so any residual shift is the
 * figure's own mass moving -- it is small, and a wide search would only find
 * the same minimum more slowly. */
static void best_shift(const Sample *a,const Sample *t,int y0,int y1,
                       int ay0,int ay1,int *out_dx,int *out_dy){
    int seed_dx,seed_dy,bdx,bdy,ddx,ddy;
    uint32_t best;
    if(a->lx1<a->lx0||t->lx1<t->lx0||y1<y0){*out_dx=0;*out_dy=0;return;}
    seed_dx=((t->lx0+t->lx1)-(a->lx0+a->lx1))/2;
    seed_dy=((y0+y1)-(ay0+ay1))/2;
    if(seed_dx>MAX_SHIFT)seed_dx=MAX_SHIFT;
    if(seed_dx<-MAX_SHIFT)seed_dx=-MAX_SHIFT;
    if(seed_dy>MAX_SHIFT)seed_dy=MAX_SHIFT;
    if(seed_dy<-MAX_SHIFT)seed_dy=-MAX_SHIFT;
    bdx=seed_dx;bdy=seed_dy;
    best=shift_cost(a,t,bdx,bdy,y0,y1);
    for(ddy=-2;ddy<=2;++ddy)for(ddx=-2;ddx<=2;++ddx){
        int dx=seed_dx+ddx,dy=seed_dy+ddy;
        uint32_t c;
        if(dx>MAX_SHIFT||dx<-MAX_SHIFT||dy>MAX_SHIFT||dy<-MAX_SHIFT)continue;
        c=shift_cost(a,t,dx,dy,y0,y1);
        if(c<best||(c==best&&abs(dx)+abs(dy)<abs(bdx)+abs(bdy))){
            best=c;bdx=dx;bdy=dy;
        }
    }
    *out_dx=bdx;*out_dy=bdy;
}

/* Silhouette XOR and union for rows [y0,y1] of t against a shifted a. */
static void mask_err_bits(const Sample *a,const Sample *t,int dx,int dy,
                          int y0,int y1,uint32_t *xr,uint32_t *un){
    uint64_t buf[LWORDS];
    int y,i;
    for(y=y0;y<=y1;++y){
        const uint64_t *ar;
        int ay=y-dy;
        if(y<0||y>=LH)continue;
        ar=(ay>=0&&ay<LH)?a->rows[ay]:k_zero_row;
        row_shift(ar,buf,dx);
        for(i=0;i<LWORDS;++i){
            *xr+=(uint32_t)popcount64(buf[i]^t->rows[y][i]);
            *un+=(uint32_t)popcount64(buf[i]|t->rows[y][i]);
        }
    }
}

/*
 * Silhouette-only prediction cost in hundredths of a percent.
 *
 * Anchor selection has to look at every ordered pair in a band, so it runs on
 * packed bit rows rather than on the byte planes. The byte-level report below
 * then re-scores only the chosen assignment, where shade and tile counts
 * matter and the sample count is back down to one per view.
 */
static uint16_t predict_mask_cost(const Sample *a,const Sample *t,int pred){
    uint32_t xr=0u,un=0u;
    int regions,r;
    if(pred==PRED_P0){
        int y0=a->ly0<t->ly0?a->ly0:t->ly0;
        int y1=a->ly1>t->ly1?a->ly1:t->ly1;
        mask_err_bits(a,t,0,0,y0,y1,&xr,&un);
    }else if(pred==PRED_P2){
        int y,x;
        for(y=t->ly0;y<=t->ly1;++y){
            int tl=t->span_l[y],tr=t->span_r[y];
            int al=a->span_l[y],ar=a->span_r[y];
            int tspan,aspan;
            if(tl<0)continue;
            if(al<0){
                un+=(uint32_t)(tr-tl+1);
                xr+=(uint32_t)(tr-tl+1);
                continue;
            }
            tspan=tr-tl;aspan=ar-al;
            for(x=tl;x<=tr;++x){
                int sxp=tspan?al+(int)(((long)(x-tl)*aspan+tspan/2)/tspan):al;
                uint8_t pv,tv;
                if(sxp<0)sxp=0;
                if(sxp>=LW)sxp=LW-1;
                pv=(uint8_t)((a->rows[y][sxp>>6]>>(sxp&63))&1u);
                tv=(uint8_t)((t->rows[y][x>>6]>>(x&63))&1u);
                if(pv||tv)++un;
                if(pv!=tv)++xr;
            }
        }
    }else{
        regions=(pred==PRED_P1)?1:((pred==PRED_P3)?2:4);
        for(r=0;r<regions;++r){
            int th=t->ly1-t->ly0+1,ah=a->ly1-a->ly0+1;
            int y0=t->ly0+(th*r)/regions;
            int y1=t->ly0+(th*(r+1))/regions-1;
            int ay0=a->ly0+(ah*r)/regions;
            int ay1=a->ly0+(ah*(r+1))/regions-1;
            int dx,dy;
            if(y1<y0)continue;
            best_shift(a,t,y0,y1,ay0,ay1,&dx,&dy);
            mask_err_bits(a,t,dx,dy,y0,y1,&xr,&un);
        }
    }
    {
        double v=pct(xr,un)*100.0;
        if(v>65534.0)v=65534.0;
        return (uint16_t)(v+0.5);
    }
}

/* Build the predicted local plane for one (anchor, target, predictor) triple.
 * Every predictor is expressed as "resample the anchor's plane", so a single
 * comparison routine can score all five identically. */
static void predict(const Sample *a,const Sample *t,int pred,uint8_t *out,
                    uint32_t *side_bytes){
    int y,x,r,regions;
    memset(out,0,(size_t)LH*LW);
    if(pred==PRED_P0){
        memcpy(out,a->plane,(size_t)LH*LW);
        *side_bytes=0u;
        return;
    }
    if(pred==PRED_P2){
        for(y=0;y<LH;++y){
            int tl=t->span_l[y],tr=t->span_r[y];
            int al=a->span_l[y],ar=a->span_r[y];
            int tspan,aspan;
            if(tl<0)continue;
            if(al<0)continue; /* anchor has nothing to stretch onto this row */
            tspan=tr-tl;aspan=ar-al;
            for(x=tl;x<=tr;++x){
                int sxp=tspan?al+(int)(((long)(x-tl)*aspan+tspan/2)/tspan):al;
                if(sxp<0)sxp=0;
                if(sxp>=LW)sxp=LW-1;
                out[(size_t)y*LW+x]=a->plane[(size_t)y*LW+sxp];
            }
        }
        *side_bytes=(uint32_t)t->occupied_rows*2u;
        return;
    }
    regions=(pred==PRED_P1)?1:((pred==PRED_P3)?2:4);
    *side_bytes=(uint32_t)regions*2u;
    for(r=0;r<regions;++r){
        int th=t->ly1-t->ly0+1,ah=a->ly1-a->ly0+1;
        int y0=t->ly0+(th*r)/regions;
        int y1=t->ly0+(th*(r+1))/regions-1;
        int ay0=a->ly0+(ah*r)/regions;
        int ay1=a->ly0+(ah*(r+1))/regions-1;
        int dx,dy;
        if(y1<y0)continue;
        best_shift(a,t,y0,y1,ay0,ay1,&dx,&dy);
        for(y=y0;y<=y1;++y){
            int ay=y-dy;
            if(ay<0||ay>=LH)continue;
            for(x=0;x<LW;++x){
                int ax=x-dx;
                if(ax<0||ax>=LW)continue;
                out[(size_t)y*LW+x]=a->plane[(size_t)ay*LW+ax];
            }
        }
    }
}

static void score(const Sample *t,const uint8_t *pred,Err *e){
    uint8_t dirty[TILE_COUNT],dirty_mask[TILE_COUNT];
    int y,x;
    memset(dirty,0,sizeof(dirty));
    memset(dirty_mask,0,sizeof(dirty_mask));
    e->mask_xor=0u;e->mask_union=0u;e->shade_bad=0u;e->overlap=0u;
    e->tiles=0u;e->tiles_mask=0u;
    for(y=0;y<LH;++y)for(x=0;x<LW;++x){
        uint8_t pv=pred[(size_t)y*LW+x],tv=t->plane[(size_t)y*LW+x];
        int sx,sy,tile;
        if(!pv&&!tv)continue;
        if(pv&&tv){
            ++e->overlap;
            if(pv!=tv)++e->shade_bad;
        }else{
            ++e->mask_xor;
        }
        ++e->mask_union;
        if(pv==tv)continue;
        sx=x-LOX+t->anchor_x;sy=y-LOY+t->anchor_y;
        if(sx<0||sx>=SCREEN_W||sy<0||sy>=SCREEN_H)continue;
        tile=(sy>>3)*TILE_COLS+(sx>>3);
        dirty[tile]=1u;
        /* Separating these two tells us whether the upload cost is the
         * outline moving or the interior re-shading. They need completely
         * different remedies. */
        if(!pv||!tv)dirty_mask[tile]=1u;
    }
    for(y=0;y<TILE_COUNT;++y){e->tiles+=dirty[y];e->tiles_mask+=dirty_mask[y];}
}

/* --------------------------------------------------------------------- */

static uint16_t rd_u16(const uint8_t *p){return (uint16_t)(p[0]|(p[1]<<8));}
static int16_t rd_i16(const uint8_t *p){return (int16_t)rd_u16(p);}
static uint32_t rd_u32(const uint8_t *p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|
           ((uint32_t)p[3]<<24);
}
static int32_t rd_i32(const uint8_t *p){return (int32_t)rd_u32(p);}

/* Byte-level scoring of one anchor assignment: shade, screen-space residual
 * tiles and side information are what a codec layout is budgeted against, so
 * the sparse silhouette matrix that drove selection is not enough on its own. */
static void report_assignment(const Sample *band0,unsigned angles,int pred,
                              const uint8_t *chosen,const uint16_t *from,
                              unsigned k,const char *label,unsigned band,
                              uint8_t *scratch){
    double mask_sum=0.0,mask_max=0.0,shade_sum=0.0,shade_max=0.0,side_sum=0.0;
    uint32_t tiles_sum=0u,tiles_max=0u,mtiles_sum=0u,worst_view=0u;
    unsigned predicted=0u,j;
    for(j=0u;j<angles;++j){
        Err e;
        uint32_t side=0u;
        double mv,sh;
        if(chosen[j])continue; /* stored outright, not predicted */
        ++predicted;
        predict(&band0[from[j]],&band0[j],pred,scratch,&side);
        score(&band0[j],scratch,&e);
        mv=pct(e.mask_xor,e.mask_union);
        sh=pct(e.shade_bad,e.overlap);
        mask_sum+=mv;shade_sum+=sh;side_sum+=side;
        if(mv>mask_max){mask_max=mv;worst_view=j;}
        if(sh>shade_max)shade_max=sh;
        tiles_sum+=e.tiles;mtiles_sum+=e.tiles_mask;
        if(e.tiles>tiles_max)tiles_max=e.tiles;
    }
    printf("band=%u predictor=%s placement=%s anchors=%u "
           "mask_pct_mean=%.3f max=%.3f worst_view=%u "
           "shade_pct_mean=%.3f max=%.3f "
           "residual_tiles_mean=%.2f max=%u silhouette_tiles_mean=%.2f "
           "side_bytes_mean=%.1f\n",
           band,k_pred_name[pred],label,k,
           predicted?mask_sum/predicted:0.0,mask_max,worst_view,
           predicted?shade_sum/predicted:0.0,shade_max,
           predicted?(double)tiles_sum/predicted:0.0,tiles_max,
           predicted?(double)mtiles_sum/predicted:0.0,
           predicted?side_sum/predicted:0.0);
}

static int iround_q8(int32_t q8){
    return (int)((q8>=0?q8+128:q8-128)/256);
}

int main(int argc,char **argv){
    FILE *f;
    long size;
    uint8_t *raw;
    const uint8_t *p;
    unsigned angles,bands,i,b,pred;
    unsigned anchor_sweep[11]={1,2,4,8,16,24,32,48,64,96,128};
    unsigned sweep_len=11u;
    Sample *s;
    uint8_t *scratch;
    double pivot_z,eye_z,radius[8];

    if(argc<2){fprintf(stderr,"usage: %s CORPUS.dhc\n",argv[0]);return 2;}
    f=fopen(argv[1],"rb");if(!f)die("cannot open corpus");
    fseek(f,0,SEEK_END);size=ftell(f);fseek(f,0,SEEK_SET);
    raw=(uint8_t*)malloc((size_t)size);if(!raw)die("out of memory");
    if(fread(raw,1,(size_t)size,f)!=(size_t)size)die("short read");
    fclose(f);

    if(size<62||memcmp(raw,"DHC1",4))die("not a DHC1 corpus");
    p=raw+4;
    if(rd_u16(p)!=1u)die("unsupported corpus version");
    p+=2;
    if(p[0]!=SCREEN_W||p[1]!=SCREEN_H)die("unexpected screen geometry");
    p+=2;
    angles=rd_u16(p);p+=2;
    bands=p[0];p+=2; /* bands, owner */
    p+=8;            /* pivot x,y */
    pivot_z=(double)rd_i32(p)/256.0;p+=4;
    eye_z=(double)rd_i32(p)/256.0;p+=4;
    p+=2;            /* focal */
    for(i=0u;i<8u;++i){radius[i]=(double)rd_u32(p)/256.0;p+=4;}
    if(!angles||!bands||bands>8u)die("corpus header out of range");

    s=(Sample*)calloc((size_t)angles*bands,sizeof(Sample));
    scratch=(uint8_t*)malloc((size_t)LH*LW);
    if(!s||!scratch)die("out of memory");

    for(i=0u;i<angles*bands;++i){
        Sample *sm=&s[i];
        int y,x;
        if((size_t)(p-raw)+22u>(size_t)size)die("corpus truncated");
        sm->angle=rd_u16(p);p+=2;
        sm->band=p[0];sm->yaw=p[1];p+=2;
        sm->cam_x_q8=rd_i32(p);p+=4;
        sm->cam_y_q8=rd_i32(p);p+=4;
        sm->anchor_x_q8=rd_i16(p);p+=2;
        sm->anchor_y_q8=rd_i16(p);p+=2;
        sm->sx0=p[0];sm->sy0=p[1];sm->sx1=p[2];sm->sy1=p[3];p+=4;
        sm->pixels=rd_u16(p);p+=2;
        sm->anchor_x=iround_q8(sm->anchor_x_q8);
        sm->anchor_y=iround_q8(sm->anchor_y_q8);
        sm->rows=(uint64_t(*)[LWORDS])calloc(LH,sizeof(uint64_t)*LWORDS);
        sm->plane=(uint8_t*)calloc((size_t)LH*LW,1u);
        sm->span_l=(int16_t*)malloc(sizeof(int16_t)*LH);
        sm->span_r=(int16_t*)malloc(sizeof(int16_t)*LH);
        if(!sm->rows||!sm->plane||!sm->span_l||!sm->span_r)die("out of memory");
        for(y=0;y<LH;++y){sm->span_l[y]=-1;sm->span_r[y]=-1;}
        sm->lx0=LW;sm->ly0=LH;sm->lx1=-1;sm->ly1=-1;
        for(y=(int)sm->sy0;y<=(int)sm->sy1;++y)
            for(x=(int)sm->sx0;x<=(int)sm->sx1;++x){
                uint8_t v=*p++;
                int ly=y-sm->anchor_y+LOY,lx=x-sm->anchor_x+LOX;
                if(!v)continue;
                if(ly<0||ly>=LH||lx<0||lx>=LW)
                    die("corpus sample escapes the local canvas");
                sm->plane[(size_t)ly*LW+lx]=v;
                sm->rows[ly][lx>>6]|=(uint64_t)1u<<(lx&63);
                if(sm->span_l[ly]<0||lx<sm->span_l[ly])sm->span_l[ly]=(int16_t)lx;
                if(lx>sm->span_r[ly])sm->span_r[ly]=(int16_t)lx;
                if(lx<sm->lx0)sm->lx0=lx;
                if(lx>sm->lx1)sm->lx1=lx;
                if(ly<sm->ly0)sm->ly0=ly;
                if(ly>sm->ly1)sm->ly1=ly;
            }
        for(y=0;y<LH;++y)if(sm->span_l[y]>=0)++sm->occupied_rows;
        if(sm->lx1<sm->lx0)die("corpus sample has no hero pixels");
    }

    printf("DOOM_DENSE_CORPUS v1\n");
    printf("samples=%u angles=%u bands=%u angular_step_deg=%.4f "
           "pivot_z=%.2f eye_z=%.2f\n",
           angles*bands,angles,bands,360.0/(double)angles,pivot_z,eye_z);

    for(b=0u;b<bands;++b){
        const Sample *band0=&s[(size_t)b*angles];
        uint32_t px_min=0xffffffffu,px_max=0u,px_sum=0u;
        int w_min=LW,w_max=0,h_min=LH,h_max=0;
        uint32_t rows_min=0xffffffffu,rows_max=0u;
        for(i=0u;i<angles;++i){
            const Sample *sm=&band0[i];
            int w=sm->lx1-sm->lx0+1,h=sm->ly1-sm->ly0+1;
            if(sm->pixels<px_min)px_min=sm->pixels;
            if(sm->pixels>px_max)px_max=sm->pixels;
            px_sum+=sm->pixels;
            if(w<w_min)w_min=w;
            if(w>w_max)w_max=w;
            if(h<h_min)h_min=h;
            if(h>h_max)h_max=h;
            if(sm->occupied_rows<rows_min)rows_min=sm->occupied_rows;
            if(sm->occupied_rows>rows_max)rows_max=sm->occupied_rows;
        }
        printf("band=%u radius=%.2f anchor_y=%.3f pixels=%u..%u mean=%u "
               "bbox_w=%d..%d bbox_h=%d..%d rows=%u..%u\n",
               b,radius[b],(double)band0[0].anchor_y_q8/256.0,
               px_min,px_max,px_sum/angles,w_min,w_max,h_min,h_max,
               rows_min,rows_max);
    }

    /* ---- intrinsic cost of one angular step, independent of any codec ---- */
    for(b=0u;b<bands;++b){
        const Sample *band0=&s[(size_t)b*angles];
        uint32_t tiles_sum=0u,tiles_max=0u,mtiles_sum=0u,mtiles_max=0u;
        uint32_t over_budget=0u;
        double mask_sum=0.0,mask_max=0.0,shade_sum=0.0;
        for(i=0u;i<angles;++i){
            const Sample *a=&band0[i],*t=&band0[(i+1u)%angles];
            Err e;
            double m;
            score(t,a->plane,&e);
            m=pct(e.mask_xor,e.mask_union);
            mask_sum+=m;
            shade_sum+=pct(e.shade_bad,e.overlap);
            if(m>mask_max)mask_max=m;
            tiles_sum+=e.tiles;
            mtiles_sum+=e.tiles_mask;
            if(e.tiles>tiles_max)tiles_max=e.tiles;
            if(e.tiles_mask>mtiles_max)mtiles_max=e.tiles_mask;
            if(e.tiles>VBLANK_TILE_BUDGET)++over_budget;
        }
        printf("band=%u adjacent_step mask_pct_mean=%.3f max=%.3f "
               "shade_pct_mean=%.3f changed_tiles_mean=%.2f max=%u "
               "silhouette_tiles_mean=%.2f max=%u "
               "steps_over_vblank_budget=%u/%u budget=%u\n",
               b,mask_sum/angles,mask_max,shade_sum/angles,
               (double)tiles_sum/angles,tiles_max,
               (double)mtiles_sum/angles,mtiles_max,
               over_budget,angles,(unsigned)VBLANK_TILE_BUDGET);
    }

    /* ---- predictor comparison on the hardest case: one angular step ---- */
    for(b=0u;b<bands;++b){
        Sample *band0=&s[(size_t)b*angles];
        for(pred=0u;pred<PRED_COUNT;++pred){
            double mask_sum=0.0,mask_max=0.0,shade_sum=0.0,shade_max=0.0;
            double side_sum=0.0;
            uint32_t tiles_sum=0u,tiles_max=0u,worst=0u,mtiles_sum=0u;
            for(i=0u;i<angles;++i){
                Sample *a=&band0[i],*t=&band0[(i+1u)%angles];
                Err e;
                uint32_t side=0u;
                double m,sh;
                predict(a,t,(int)pred,scratch,&side);
                score(t,scratch,&e);
                mtiles_sum+=e.tiles_mask;
                m=pct(e.mask_xor,e.mask_union);
                sh=pct(e.shade_bad,e.overlap);
                mask_sum+=m;shade_sum+=sh;side_sum+=side;
                if(m>mask_max){mask_max=m;worst=i;}
                if(sh>shade_max)shade_max=sh;
                tiles_sum+=e.tiles;
                if(e.tiles>tiles_max)tiles_max=e.tiles;
            }
            printf("band=%u predictor=%s adjacent mask_pct_mean=%.3f max=%.3f "
                   "worst_angle=%u shade_pct_mean=%.3f max=%.3f "
                   "residual_tiles_mean=%.2f max=%u silhouette_tiles_mean=%.2f "
                   "side_bytes_mean=%.1f (%s)\n",
                   b,k_pred_name[pred],mask_sum/angles,mask_max,worst,
                   shade_sum/angles,shade_max,(double)tiles_sum/angles,
                   tiles_max,(double)mtiles_sum/angles,side_sum/angles,
                   k_pred_side[pred]);
        }
    }

    /*
     * Greedy adaptive anchor selection.
     *
     * Uniform keyframes spend the same budget on the flat sides of the orbit
     * as on the quarter-turns where the arms and rifle swing across the body.
     * The selector below is the standard greedy set-cover step: repeatedly add
     * whichever view most reduces the WORST still-unpredicted view, tie-broken
     * by the mean. It is not optimal, but for this problem the curve it traces
     * is the honest answer to "what does one more anchor actually buy".
     */
    for(b=0u;b<bands;++b){
        Sample *band0=&s[(size_t)b*angles];
        for(pred=0u;pred<PRED_COUNT;++pred){
            uint16_t *m=(uint16_t*)malloc(sizeof(uint16_t)*angles*angles);
            uint16_t *best=(uint16_t*)malloc(sizeof(uint16_t)*angles);
            uint16_t *from=(uint16_t*)malloc(sizeof(uint16_t)*angles);
            uint8_t *chosen=(uint8_t*)calloc(angles,1u);
            unsigned k,step;
            if(!m||!best||!from||!chosen)die("out of memory");
            for(i=0u;i<angles;++i){
                unsigned j;
                for(j=0u;j<angles;++j)
                    m[i*angles+j]=predict_mask_cost(&band0[i],&band0[j],
                                                    (int)pred);
                best[i]=0xffffu;from[i]=0u;
            }
            step=0u;
            for(k=1u;k<=angles&&step<sweep_len;++k){
                unsigned pick=0u,j;
                uint64_t pick_cost=(uint64_t)-1;
                for(i=0u;i<angles;++i){
                    uint64_t worst=0u,sum=0u,cost;
                    if(chosen[i])continue;
                    for(j=0u;j<angles;++j){
                        uint32_t v=m[i*angles+j];
                        if(v>best[j])v=best[j];
                        if(v>worst)worst=v;
                        sum+=v;
                    }
                    cost=worst*(uint64_t)angles*4u+sum;
                    if(cost<pick_cost){pick_cost=cost;pick=i;}
                }
                chosen[pick]=1u;
                for(j=0u;j<angles;++j)
                    if(m[pick*angles+j]<best[j]){
                        best[j]=m[pick*angles+j];from[j]=(uint16_t)pick;
                    }
                if(k!=anchor_sweep[step])continue;
                ++step;
                /* A budget that stores every view predicts nothing, so it has
                 * no error to report -- printing zeros there would read as a
                 * perfect result rather than a degenerate one. */
                if(k<angles)
                    report_assignment(band0,angles,(int)pred,chosen,from,k,
                                      "greedy",b,scratch);
            }
            /* Uniform keyframes are the strategy the lab would otherwise have
             * defaulted to. Printing both makes the adaptive selector prove it
             * is worth the extra table rather than assuming it. */
            for(step=0u;step<sweep_len;++step){
                unsigned k=anchor_sweep[step],j;
                if(k>=angles)break;
                memset(chosen,0,angles);
                for(j=0u;j<k;++j)chosen[(j*angles)/k]=1u;
                for(j=0u;j<angles;++j){
                    unsigned i2;
                    best[j]=0xffffu;from[j]=0u;
                    for(i2=0u;i2<angles;++i2){
                        if(!chosen[i2])continue;
                        if(m[i2*angles+j]<best[j]){
                            best[j]=m[i2*angles+j];from[j]=(uint16_t)i2;
                        }
                    }
                }
                report_assignment(band0,angles,(int)pred,chosen,from,k,
                                  "uniform",b,scratch);
            }
            free(m);free(best);free(from);free(chosen);
        }
    }

    return 0;
}
