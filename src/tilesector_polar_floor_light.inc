/*
 * Experimental static floor-light semantic pass for Adaptive Polar.
 *
 * Static light + static occluders collapse to four convex world-space shadow
 * polygons. Runtime work is therefore only camera projection plus tile-space
 * materialization. Four internal cast edges reuse the existing shade-0 edge
 * vocabulary; solid shadow reuses the normal floor tile under palette 1.
 * Zero additional VRAM patterns are allocated.
 */
#if defined(__SDCC)

extern volatile uint8_t g_tspf_stage;
extern volatile uint8_t g_ts_render_stage;
#define TSP_FL_SET_STAGE(v) do { g_tspf_stage=(v); g_ts_render_stage=(v); } while(0)

typedef struct TSPFloorLightCamV { int16_t f_q2,s_q2; } TSPFloorLightCamV;
typedef struct TSPFloorLightScreenV { int16_t x,y; } TSPFloorLightScreenV;
typedef struct TSPFloorLightPolyDef { const int16_t *xy_q2;uint8_t count,floor_z; } TSPFloorLightPolyDef;
typedef struct TSPFloorLightEdgeDef { uint8_t va,vb,floor_z; } TSPFloorLightEdgeDef;

/* Oracle polygons, rounded to quarter-world-unit Q2. */
static const int16_t k_fl_poly_a0[] = {320,144,320,64,251,64};
static const int16_t k_fl_poly_a1[] = {265,320,320,320,320,256};
static const int16_t k_fl_poly_b0[] = {544,24,448,56,448,144,580,52};
static const int16_t k_fl_poly_b1[] = {448,336,562,336,448,256};
static const TSPFloorLightPolyDef k_fl_poly[4] = {
    {k_fl_poly_a0,3u,0u},{k_fl_poly_a1,3u,0u},
    {k_fl_poly_b0,4u,4u},{k_fl_poly_b1,3u,4u}
};
/* Only edges internal to the receiver need a partial light/shadow tile. */
static const TSPFloorLightEdgeDef k_fl_edge[4] = {
    {2u,0u,0u},{2u,0u,0u},{2u,3u,4u},{1u,2u,4u}
};

#define TSP_FL_MAX_VERTS 8u
#define TSP_FL_NEAR_Q2 4
#define TSP_FL_SHADOW_WORD ((uint16_t)(TSP_TILE_FLOOR|TSP_ATTR_PALETTE))

/* Static scratch avoids the SDCC 4.5 aggregate-stack ICE seen with a local
 * [4][8] projected-vertex array.  Total scratch is deliberately tiny. */
static uint16_t *g_fl_out_map;
static TSPFloorLightCamV g_fl_clip_a[TSP_FL_MAX_VERTS];
static TSPFloorLightCamV g_fl_clip_b[TSP_FL_MAX_VERTS];
static TSPFloorLightScreenV g_fl_sp0[TSP_FL_MAX_VERTS];
static TSPFloorLightScreenV g_fl_sp1[TSP_FL_MAX_VERTS];
static TSPFloorLightScreenV g_fl_sp2[TSP_FL_MAX_VERTS];
static TSPFloorLightScreenV g_fl_sp3[TSP_FL_MAX_VERTS];
static TSPFloorLightCamV g_fl_edge_ca[4];
static TSPFloorLightCamV g_fl_edge_cb[4];
static int16_t g_fl_cam_x_q2,g_fl_cam_y_q2;
static int8_t g_fl_cam_sn_q5,g_fl_cam_cs_q5;

static uint16_t fl_abs16(int16_t v){return (uint16_t)(v<0?-v:v);}

static uint8_t fl_ratio_q8(uint16_t n,uint16_t d){
    if(!d)return 0u;
    while(n>255u||d>255u){n=(uint16_t)((n+1u)>>1);d=(uint16_t)((d+1u)>>1);}
    if(n>d)n=d;
    return ratio_q8_exact((uint8_t)n,(uint8_t)d);
}

/* signed-16 * unsigned-Q8 without a 32-bit multiply. */
static int16_t fl_mul_s16_q8(int16_t v,uint8_t t){
    uint16_t a=fl_abs16(v),q;
    q=(uint16_t)((a>>8)*(uint16_t)t);
    q=(uint16_t)(q+((((a&255u)*(uint16_t)t)+128u)>>8));
    return v<0?-(int16_t)q:(int16_t)q;
}

static int16_t fl_plane_eval(const TSPFloorLightCamV *v,uint8_t plane){
    if(plane==0u)return (int16_t)(v->f_q2-TSP_FL_NEAR_Q2);
    if(plane==1u)return (int16_t)(v->s_q2+v->f_q2);
    return (int16_t)(v->f_q2-v->s_q2);
}

static TSPFloorLightCamV fl_lerp_cross(const TSPFloorLightCamV *a,const TSPFloorLightCamV *b,int16_t ea,int16_t eb){
    TSPFloorLightCamV o;uint8_t t;
    t=fl_ratio_q8(fl_abs16(ea),fl_abs16((int16_t)(ea-eb)));
    o.f_q2=(int16_t)(a->f_q2+fl_mul_s16_q8((int16_t)(b->f_q2-a->f_q2),t));
    o.s_q2=(int16_t)(a->s_q2+fl_mul_s16_q8((int16_t)(b->s_q2-a->s_q2),t));
    return o;
}

static uint8_t fl_clip_plane(const TSPFloorLightCamV *in,uint8_t n,TSPFloorLightCamV *out,uint8_t plane){
    uint8_t i,m=0u,ia,ib;TSPFloorLightCamV a,b;int16_t ea,eb;
    if(!n)return 0u;
    a=in[(uint8_t)(n-1u)];ea=fl_plane_eval(&a,plane);ia=(uint8_t)(ea>=0);
    for(i=0u;i<n;++i){
        b=in[i];eb=fl_plane_eval(&b,plane);ib=(uint8_t)(eb>=0);
        if(ia!=ib&&m<TSP_FL_MAX_VERTS)out[m++]=fl_lerp_cross(&a,&b,ea,eb);
        if(ib&&m<TSP_FL_MAX_VERTS)out[m++]=b;
        a=b;ea=eb;ia=ib;
    }
    return m;
}

static void fl_world_to_cam(int16_t x_q2,int16_t y_q2,TSPFloorLightCamV *o){
    int16_t dx=(int16_t)(x_q2-g_fl_cam_x_q2),dy=(int16_t)(y_q2-g_fl_cam_y_q2);
    o->f_q2=shr_signed((int16_t)((int16_t)(dx*g_fl_cam_cs_q5)+(int16_t)(dy*g_fl_cam_sn_q5)),5);
    o->s_q2=shr_signed((int16_t)((int16_t)(-dx*g_fl_cam_sn_q5)+(int16_t)(dy*g_fl_cam_cs_q5)),5);
}

static uint8_t fl_clip_poly(const TSPFloorLightPolyDef *pd,uint8_t edge_id){
    uint8_t i,n=pd->count;const TSPFloorLightEdgeDef *ed=&k_fl_edge[edge_id];
    for(i=0u;i<n;++i)fl_world_to_cam(pd->xy_q2[(uint8_t)(i+i)],pd->xy_q2[(uint8_t)(i+i+1u)],&g_fl_clip_a[i]);
    /* The cast edge is already a boundary of its owner polygon. Keep its raw
     * camera-space endpoints before Sutherland-Hodgman mutates the scratch, so
     * the edge pass does not transform the same world vertices a second time. */
    g_fl_edge_ca[edge_id]=g_fl_clip_a[ed->va];
    g_fl_edge_cb[edge_id]=g_fl_clip_a[ed->vb];
    n=fl_clip_plane(g_fl_clip_a,n,g_fl_clip_b,0u);if(!n)return 0u;
    n=fl_clip_plane(g_fl_clip_b,n,g_fl_clip_a,1u);if(!n)return 0u;
    n=fl_clip_plane(g_fl_clip_a,n,g_fl_clip_b,2u);if(!n)return 0u;
    return n;
}

static uint8_t fl_clip_line(TSPFloorLightCamV *a,TSPFloorLightCamV *b){
    uint8_t p,ia,ib;int16_t ea,eb;TSPFloorLightCamV x;
    for(p=0u;p<3u;++p){
        ea=fl_plane_eval(a,p);eb=fl_plane_eval(b,p);ia=(uint8_t)(ea>=0);ib=(uint8_t)(eb>=0);
        if(!ia&&!ib)return 0u;
        if(ia!=ib){x=fl_lerp_cross(a,b,ea,eb);if(!ia)*a=x;else *b=x;}
    }
    return 1u;
}

static TSPFloorLightScreenV fl_project(const TSPFloorLightCamV *v,uint8_t floor_z){
    TSPFloorLightScreenV o;uint16_t af=(uint16_t)v->f_q2,as=fl_abs16(v->s_q2);uint8_t r,inv;int16_t xo,yo;
    if(as>af)as=af;
    r=fl_ratio_q8(as,af);xo=(int16_t)(((uint16_t)r*80u+128u)>>8);
    o.x=v->s_q2<0?(int16_t)(80-xo):(int16_t)(80+xo);if(o.x>159)o.x=159;
    inv=inv_for_dq4((int16_t)(v->f_q2<<2));
    if(floor_z)yo=(int16_t)(((uint16_t)inv*3u+4u)>>3);else yo=(int16_t)((inv+1u)>>1);
    o.y=(int16_t)(72+yo);if(o.y>143)o.y=143;
    return o;
}

static uint8_t fl_project_poly(const TSPFloorLightPolyDef *pd,uint8_t edge_id,TSPFloorLightScreenV *dst){
    uint8_t i,n=fl_clip_poly(pd,edge_id);for(i=0u;i<n;++i)dst[i]=fl_project(&g_fl_clip_b[i],pd->floor_z);return n;
}

static uint8_t fl_cell_unclaimed(uint8_t row,uint8_t col){
    uint8_t ci=(uint8_t)(col+col+col+(row>>3));
    return (uint8_t)((g_polar_nt_cov_cur[ci]&k_nt_mask8[row&7u])==0u);
}

/* Shade-0 patterns are authored as two-colour floor boundaries by the lit ROM.
 * FLIPX gives signed slope; palette selection chooses which side is shadow. */
static uint16_t fl_edge_word(int16_t local_left,int8_t slope,uint8_t shadow_below){
    uint16_t attr=shadow_below?TSP_ATTR_PALETTE:0u;uint8_t mag;int8_t off;
    if(slope<0){mag=(uint8_t)(-slope);local_left=(int16_t)(local_left-mag);attr|=TSP_ATTR_FLIPX;}else mag=(uint8_t)slope;
    if(mag>=TSP_EDGE_SLOPE_COUNT)mag=TSP_EDGE_SLOPE_COUNT-1u;
    off=clamp_s8(local_left,TSP_EDGE_OFF_MIN,(int8_t)(TSP_EDGE_OFF_MIN+TSP_EDGE_OFF_COUNT-1));
    return (uint16_t)(TSP_TILE_EDGE(0u,(uint8_t)(off-TSP_EDGE_OFF_MIN),mag)|attr);
}

static int16_t fl_line_y_at(const TSPFloorLightScreenV *a,const TSPFloorLightScreenV *b,int16_t x){
    uint16_t dx=fl_abs16((int16_t)(b->x-a->x)),nx=fl_abs16((int16_t)(x-a->x));uint8_t t;
    if(!dx)return a->y;t=fl_ratio_q8(nx,dx);return (int16_t)(a->y+fl_mul_s16_q8((int16_t)(b->y-a->y),t));
}

static void fl_draw_cast_edge(uint8_t edge_id,const TSPFloorLightScreenV *poly,uint8_t pn){
    const TSPFloorLightEdgeDef *ed=&k_fl_edge[edge_id];TSPFloorLightCamV ca=g_fl_edge_ca[edge_id],cb=g_fl_edge_cb[edge_id];TSPFloorLightScreenV a,b,t;int16_t cross,dxh,dyh;uint8_t i,c,c0,c1,shadow_below=0u;
    if(!fl_clip_line(&ca,&cb))return;a=fl_project(&ca,ed->floor_z);b=fl_project(&cb,ed->floor_z);
    if(a.x>b.x){t=a;a=b;b=t;}if((int16_t)(b.x-a.x)<2)return;
    /* Find any clipped polygon vertex clearly off the cast line. Halving the
     * line vector keeps the sign test inside signed 16-bit arithmetic. */
    dxh=(int16_t)((b.x-a.x)>>1);dyh=shr_signed((int16_t)(b.y-a.y),1);
    for(i=0u;i<pn;++i){
        cross=(int16_t)(dxh*(int16_t)(poly[i].y-a.y)-dyh*(int16_t)(poly[i].x-a.x));
        if(cross>1){shadow_below=1u;break;}if(cross<-1){shadow_below=0u;break;}
    }
    c0=(uint8_t)(a.x>>3);c1=(uint8_t)(b.x>>3);if(c1>=TSP_COLS)c1=TSP_COLS-1u;
    for(c=c0;c<=c1;++c){
        int16_t xl=(int16_t)((int16_t)c<<3),xr=(int16_t)(xl+7),yl,yr,mid,local;int8_t row,slope;
        if(xl<a.x)xl=a.x;if(xr>b.x)xr=b.x;if(xr<xl)continue;
        yl=fl_line_y_at(&a,&b,xl);yr=fl_line_y_at(&a,&b,xr);mid=(int16_t)((yl+yr)>>1);row=row_floor(mid);
        if(row<9||row>17)continue;if(!fl_cell_unclaimed((uint8_t)row,c))continue;
        slope=clamp_s8((int16_t)(yr-yl),-7,7);local=(int16_t)(yl-((int16_t)row<<3));
        put_cell(g_fl_out_map,(uint8_t)row,c,fl_edge_word(local,slope,shadow_below));
    }
}

static int16_t fl_scan_x(const TSPFloorLightScreenV *a,const TSPFloorLightScreenV *b,int16_t y){
    uint16_t dy=fl_abs16((int16_t)(b->y-a->y)),ny=fl_abs16((int16_t)(y-a->y));uint8_t t;
    if(!dy)return a->x;t=fl_ratio_q8(ny,dy);return (int16_t)(a->x+fl_mul_s16_q8((int16_t)(b->x-a->x),t));
}

static void fl_fill_poly(const TSPFloorLightScreenV *p,uint8_t n){
    uint8_t row,i;if(n<3u)return;
    for(row=9u;row<TSP_ROWS;++row){
        int16_t y=(int16_t)(((int16_t)row<<3)+4),xmin=32767,xmax=-32767;uint8_t hits=0u;TSPFloorLightScreenV a=p[(uint8_t)(n-1u)],b;
        for(i=0u;i<n;++i){int16_t x;b=p[i];if((a.y<=y&&b.y>y)||(b.y<=y&&a.y>y)){x=fl_scan_x(&a,&b,y);if(x<xmin)xmin=x;if(x>xmax)xmax=x;++hits;}a=b;}
        if(hits>=2u&&xmax>=xmin){
            uint8_t c,c0=(uint8_t)((xmin<4?0:(xmin-4)>>3)),c1=(uint8_t)((xmax<4?0:(xmax-4)>>3));if(c1>=TSP_COLS)c1=TSP_COLS-1u;
            for(c=c0;c<=c1;++c){int16_t xc=(int16_t)(((int16_t)c<<3)+4);if(xc<xmin||xc>xmax)continue;if(fl_cell_unclaimed(row,c))put_cell(g_fl_out_map,row,c,TSP_FL_SHADOW_WORD);}
        }
    }
}

static void tsp_polar_floor_light(const TSPState *s,uint16_t out_map[TSP_MAP_CELLS]){
    uint8_t n0,n1,n2,n3;g_fl_out_map=out_map;
    /* Camera origin and yaw are frame constants. Cache them once instead of
     * repeating shifts and sine-table reads for every light vertex. */
    g_fl_cam_x_q2=shr_signed(s->x_q4,2);g_fl_cam_y_q2=shr_signed(s->y_q4,2);
    g_fl_cam_sn_q5=(int8_t)shr_signed((int16_t)k_tspf_sin_q7[s->yaw],2);
    g_fl_cam_cs_q5=(int8_t)shr_signed((int16_t)k_tspf_sin_q7[(uint8_t)(s->yaw+64u)],2);
    TSP_FL_SET_STAGE(5u);
    n0=fl_project_poly(&k_fl_poly[0],0u,g_fl_sp0);
    n1=fl_project_poly(&k_fl_poly[1],1u,g_fl_sp1);
    n2=fl_project_poly(&k_fl_poly[2],2u,g_fl_sp2);
    n3=fl_project_poly(&k_fl_poly[3],3u,g_fl_sp3);
    TSP_FL_SET_STAGE(6u);
    if(n0>=3u)fl_draw_cast_edge(0u,g_fl_sp0,n0);
    if(n1>=3u)fl_draw_cast_edge(1u,g_fl_sp1,n1);
    if(n2>=3u)fl_draw_cast_edge(2u,g_fl_sp2,n2);
    if(n3>=3u)fl_draw_cast_edge(3u,g_fl_sp3,n3);
    TSP_FL_SET_STAGE(7u);
    if(n0>=3u)fl_fill_poly(g_fl_sp0,n0);
    if(n1>=3u)fl_fill_poly(g_fl_sp1,n1);
    if(n2>=3u)fl_fill_poly(g_fl_sp2,n2);
    if(n3>=3u)fl_fill_poly(g_fl_sp3,n3);
}

#endif /* __SDCC */

