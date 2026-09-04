/*
 * Experimental static floor-light semantic pass for Adaptive Polar.
 *
 * The light and occluders are static, so the exact finite-height oracle reduces
 * to four small convex shadow polygons on the two receiver floor planes.  This
 * runtime pass projects those polygons from the live camera, materializes only
 * the four internal cast boundaries with the EXISTING shade-0 edge vocabulary,
 * then fills shadow interiors with the ordinary floor tile using palette 1.
 *
 * No new VRAM patterns are required.  Geometry is already rendered near->far;
 * this pass runs before tsp_polar_nt_end_frame() and writes only unclaimed
 * name-table cells.  Its writes therefore participate in the existing
 * persistent ownership/dirty/lifetime system and stale light cells disappear
 * automatically on the following frame.
 */
#if defined(__SDCC)

typedef struct TSPFloorLightCamV {
    int16_t f_q2;
    int16_t s_q2;
} TSPFloorLightCamV;

typedef struct TSPFloorLightScreenV {
    int16_t x;
    int16_t y;
} TSPFloorLightScreenV;

typedef struct TSPFloorLightPolyDef {
    const int16_t *xy_q2;
    uint8_t count;
    uint8_t floor_z;
} TSPFloorLightPolyDef;

typedef struct TSPFloorLightEdgeDef {
    int16_t ax_q2,ay_q2,bx_q2,by_q2;
    uint8_t floor_z;
    uint8_t owner_poly;
} TSPFloorLightEdgeDef;

/* Exact oracle polygons rounded to quarter-world-unit Q2.  Physical receiver
 * boundary edges are intentionally not emitted as light edges; geometry owns
 * those.  Only the internal cast boundary requires a partial shadow tile. */
static const int16_t k_fl_poly_a0[] = {320,144, 320,64, 251,64};
static const int16_t k_fl_poly_a1[] = {265,320, 320,320, 320,256};
static const int16_t k_fl_poly_b0[] = {544,24, 448,56, 448,144, 580,52};
static const int16_t k_fl_poly_b1[] = {448,336, 562,336, 448,256};
static const TSPFloorLightPolyDef k_fl_poly[4] = {
    {k_fl_poly_a0,3u,0u},{k_fl_poly_a1,3u,0u},
    {k_fl_poly_b0,4u,4u},{k_fl_poly_b1,3u,4u}
};
static const TSPFloorLightEdgeDef k_fl_edge[4] = {
    {251,64, 320,144,0u,0u},
    {320,256, 265,320,0u,1u},
    {448,144, 580,52,4u,2u},
    {562,336, 448,256,4u,3u}
};

#define TSP_FL_MAX_VERTS 8u
#define TSP_FL_NEAR_Q2 4
#define TSP_FL_SHADOW_WORD ((uint16_t)(TSP_TILE_FLOOR|TSP_ATTR_PALETTE))

static uint16_t fl_abs16(int16_t v){return (uint16_t)(v<0?-v:v);}

/* Ratio n/d as Q8 without 32-bit division.  Scale both operands together into
 * the byte domain and reuse the renderer's reciprocal helper. */
static uint8_t fl_ratio_q8(uint16_t n,uint16_t d){
    if(!d)return 0u;
    while(n>255u||d>255u){n=(uint16_t)((n+1u)>>1);d=(uint16_t)((d+1u)>>1);}
    if(n>d)n=d;
    return ratio_q8_exact((uint8_t)n,(uint8_t)d);
}

/* signed-16 * unsigned-Q8, still without a long multiply. */
static int16_t fl_mul_s16_q8(int16_t v,uint8_t t){
    uint16_t a=fl_abs16(v),q;
    q=(uint16_t)((a>>8)*(uint16_t)t);
    q=(uint16_t)(q+((((a&255u)*(uint16_t)t)+128u)>>8));
    return v<0?-(int16_t)q:(int16_t)q;
}

static int16_t fl_plane_eval(const TSPFloorLightCamV *v,uint8_t plane){
    if(plane==0u)return (int16_t)(v->f_q2-TSP_FL_NEAR_Q2);
    if(plane==1u)return (int16_t)(v->s_q2+v->f_q2); /* left:  side >= -forward */
    return (int16_t)(v->f_q2-v->s_q2);              /* right: side <= +forward */
}

static TSPFloorLightCamV fl_lerp_cross(const TSPFloorLightCamV *a,const TSPFloorLightCamV *b,int16_t ea,int16_t eb){
    TSPFloorLightCamV o;uint8_t t=fl_ratio_q8(fl_abs16(ea),fl_abs16((int16_t)(ea-eb)));
    o.f_q2=(int16_t)(a->f_q2+fl_mul_s16_q8((int16_t)(b->f_q2-a->f_q2),t));
    o.s_q2=(int16_t)(a->s_q2+fl_mul_s16_q8((int16_t)(b->s_q2-a->s_q2),t));
    return o;
}

static uint8_t fl_clip_plane(const TSPFloorLightCamV *in,uint8_t n,TSPFloorLightCamV *out,uint8_t plane){
    uint8_t i,m=0u;TSPFloorLightCamV a,b;int16_t ea,eb;uint8_t ia,ib;
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

static void fl_world_to_cam(int16_t x_q2,int16_t y_q2,const TSPState *s,TSPFloorLightCamV *o){
    int16_t cx=shr_signed(s->x_q4,2),cy=shr_signed(s->y_q4,2);
    int16_t dx=(int16_t)(x_q2-cx),dy=(int16_t)(y_q2-cy);
    int8_t sn=(int8_t)shr_signed((int16_t)k_tspf_sin_q7[s->yaw],2);
    int8_t cs=(int8_t)shr_signed((int16_t)k_tspf_sin_q7[(uint8_t)(s->yaw+64u)],2);
    o->f_q2=shr_signed((int16_t)((int16_t)(dx*cs)+(int16_t)(dy*sn)),5);
    o->s_q2=shr_signed((int16_t)((int16_t)(-dx*sn)+(int16_t)(dy*cs)),5);
}

static uint8_t fl_clip_poly(const TSPFloorLightPolyDef *pd,const TSPState *s,TSPFloorLightCamV *out){
    TSPFloorLightCamV a[TSP_FL_MAX_VERTS],b[TSP_FL_MAX_VERTS];uint8_t i,n=pd->count;
    for(i=0u;i<n;++i)fl_world_to_cam(pd->xy_q2[(uint8_t)(i+i)],pd->xy_q2[(uint8_t)(i+i+1u)],s,&a[i]);
    n=fl_clip_plane(a,n,b,0u);if(!n)return 0u;
    n=fl_clip_plane(b,n,a,1u);if(!n)return 0u;
    n=fl_clip_plane(a,n,b,2u);if(!n)return 0u;
    for(i=0u;i<n;++i)out[i]=b[i];
    return n;
}

static uint8_t fl_clip_line(TSPFloorLightCamV *a,TSPFloorLightCamV *b){
    uint8_t p;int16_t ea,eb;uint8_t ia,ib;TSPFloorLightCamV x;
    for(p=0u;p<3u;++p){
        ea=fl_plane_eval(a,p);eb=fl_plane_eval(b,p);ia=(uint8_t)(ea>=0);ib=(uint8_t)(eb>=0);
        if(!ia&&!ib)return 0u;
        if(ia!=ib){x=fl_lerp_cross(a,b,ea,eb);if(!ia)*a=x;else *b=x;}
    }
    return 1u;
}

static TSPFloorLightScreenV fl_project(const TSPFloorLightCamV *v,uint8_t floor_z){
    TSPFloorLightScreenV o;uint16_t af=(uint16_t)v->f_q2;uint16_t as=fl_abs16(v->s_q2);uint8_t r,inv;int16_t xo,yo;
    if(as>af)as=af;r=fl_ratio_q8(as,af);xo=(int16_t)(((uint16_t)r*80u+128u)>>8);
    o.x=v->s_q2<0?(int16_t)(80-xo):(int16_t)(80+xo);if(o.x<0)o.x=0;if(o.x>159)o.x=159;
    inv=inv_for_dq4((int16_t)(v->f_q2<<2));
    yo=floor_z?((int16_t)(((uint16_t)inv*3u+4u)>>3)):(int16_t)((inv+1u)>>1);
    o.y=(int16_t)(72+yo);if(o.y<72)o.y=72;if(o.y>143)o.y=143;
    return o;
}

static uint8_t fl_cell_unclaimed(uint8_t row,uint8_t col){
    uint8_t ci=(uint8_t)(col+col+col+(row>>3));
    return (uint8_t)((g_polar_nt_cov_cur[ci]&k_nt_mask8[row&7u])==0u);
}

/* Existing shade-0 edge pattern + FLIPX handles signed slope.  Palette 0 means
 * shadow-above; palette 1 means shadow-below.  The lit build authors shade-0
 * patterns specifically with floor/shadow colour indices, so this costs no new
 * tile patterns at all. */
static uint16_t fl_edge_word(int16_t local_left,int8_t slope,uint8_t shadow_below){
    uint16_t attr=shadow_below?TSP_ATTR_PALETTE:0u;uint8_t mag;int8_t off;
    if(slope<0){mag=(uint8_t)(-slope);local_left=(int16_t)(local_left-mag);attr|=TSP_ATTR_FLIPX;}
    else mag=(uint8_t)slope;
    if(mag>=TSP_EDGE_SLOPE_COUNT)mag=TSP_EDGE_SLOPE_COUNT-1u;
    off=clamp_s8(local_left,TSP_EDGE_OFF_MIN,(int8_t)(TSP_EDGE_OFF_MIN+TSP_EDGE_OFF_COUNT-1));
    return (uint16_t)(TSP_TILE_EDGE(0u,(uint8_t)(off-TSP_EDGE_OFF_MIN),mag)|attr);
}

static int16_t fl_line_y_at(const TSPFloorLightScreenV *a,const TSPFloorLightScreenV *b,int16_t x){
    uint16_t dx=fl_abs16((int16_t)(b->x-a->x)),nx=fl_abs16((int16_t)(x-a->x));uint8_t t;
    if(!dx)return a->y;t=fl_ratio_q8(nx,dx);
    return (int16_t)(a->y+fl_mul_s16_q8((int16_t)(b->y-a->y),t));
}

static void fl_draw_cast_edge(const TSPFloorLightEdgeDef *ed,const TSPFloorLightScreenV *poly,uint8_t pn,const TSPState *s){
    TSPFloorLightCamV ca,cb;TSPFloorLightScreenV a,b,t;int16_t cx=0,cy=0,cross;uint8_t i,c,c0,c1,shadow_below;
    fl_world_to_cam(ed->ax_q2,ed->ay_q2,s,&ca);fl_world_to_cam(ed->bx_q2,ed->by_q2,s,&cb);
    if(!fl_clip_line(&ca,&cb))return;a=fl_project(&ca,ed->floor_z);b=fl_project(&cb,ed->floor_z);
    if(a.x>b.x){t=a;a=b;b=t;}if((int16_t)(b.x-a.x)<2)return;
    for(i=0u;i<pn;++i){cx=(int16_t)(cx+poly[i].x);cy=(int16_t)(cy+poly[i].y);}cx=(int16_t)(cx/(int16_t)pn);cy=(int16_t)(cy/(int16_t)pn);
    cross=(int16_t)((int16_t)(b.x-a.x)*(int16_t)(cy-a.y)-(int16_t)(b.y-a.y)*(int16_t)(cx-a.x));
    shadow_below=(uint8_t)(cross>0);
    c0=(uint8_t)(a.x>>3);c1=(uint8_t)(b.x>>3);if(c1>=TSP_COLS)c1=TSP_COLS-1u;
    for(c=c0;c<=c1;++c){
        int16_t xl=(int16_t)((int16_t)c<<3),xr=(int16_t)(xl+7),yl,yr,mid,local;int8_t row,slope;
        if(xl<a.x)xl=a.x;if(xr>b.x)xr=b.x;if(xr<xl)continue;
        yl=fl_line_y_at(&a,&b,xl);yr=fl_line_y_at(&a,&b,xr);mid=(int16_t)((yl+yr)>>1);row=row_floor(mid);
        if(row<9||row>17)continue;if(!fl_cell_unclaimed((uint8_t)row,c))continue;
        slope=clamp_s8((int16_t)(yr-yl),-7,7);local=(int16_t)(yl-((int16_t)row<<3));
        put_cell(g_map,(uint8_t)row,c,fl_edge_word(local,slope,shadow_below));
    }
}

static int16_t fl_scan_x(const TSPFloorLightScreenV *a,const TSPFloorLightScreenV *b,int16_t y){
    uint16_t dy=fl_abs16((int16_t)(b->y-a->y)),ny=fl_abs16((int16_t)(y-a->y));uint8_t t;
    if(!dy)return a->x;t=fl_ratio_q8(ny,dy);
    return (int16_t)(a->x+fl_mul_s16_q8((int16_t)(b->x-a->x),t));
}

static void fl_fill_poly(const TSPFloorLightScreenV *p,uint8_t n){
    uint8_t row,i;if(n<3u)return;
    for(row=9u;row<TSP_ROWS;++row){
        int16_t y=(int16_t)(((int16_t)row<<3)+4),xmin=32767,xmax=-32767;uint8_t hits=0u;
        TSPFloorLightScreenV a=p[(uint8_t)(n-1u)],b;
        for(i=0u;i<n;++i){
            int16_t x;b=p[i];
            if((a.y<=y&&b.y>y)||(b.y<=y&&a.y>y)){x=fl_scan_x(&a,&b,y);if(x<xmin)xmin=x;if(x>xmax)xmax=x;++hits;}
            a=b;
        }
        if(hits>=2u&&xmax>=xmin){
            uint8_t c,c0=(uint8_t)((xmin<4?0:(xmin-4)>>3)),c1=(uint8_t)((xmax<4?0:(xmax-4)>>3));
            if(c1>=TSP_COLS)c1=TSP_COLS-1u;
            for(c=c0;c<=c1;++c){int16_t xc=(int16_t)(((int16_t)c<<3)+4);if(xc<xmin||xc>xmax)continue;if(fl_cell_unclaimed(row,c))put_cell(g_map,row,c,TSP_FL_SHADOW_WORD);}
        }
    }
}

static void tsp_polar_floor_light(const TSPState *s,uint16_t out_map[TSP_MAP_CELLS]){
    TSPFloorLightCamV cp[TSP_FL_MAX_VERTS];TSPFloorLightScreenV sp[4][TSP_FL_MAX_VERTS];uint8_t pn[4],i,j;
    (void)out_map;
    for(i=0u;i<4u;++i){pn[i]=fl_clip_poly(&k_fl_poly[i],s,cp);for(j=0u;j<pn[i];++j)sp[i][j]=fl_project(&cp[j],k_fl_poly[i].floor_z);}
    /* Partial boundaries first: their coverage bits protect them from the full
     * shadow fill that follows, exactly like nearer geometry protects itself. */
    for(i=0u;i<4u;++i)if(pn[k_fl_edge[i].owner_poly]>=3u)fl_draw_cast_edge(&k_fl_edge[i],sp[k_fl_edge[i].owner_poly],pn[k_fl_edge[i].owner_poly],s);
    for(i=0u;i<4u;++i)if(pn[i]>=3u)fl_fill_poly(sp[i],pn[i]);
}

#endif /* __SDCC */
