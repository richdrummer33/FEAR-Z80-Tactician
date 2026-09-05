/*
 * Experimental screen-affine floor-light pass for Adaptive Polar.
 *
 * Static floor shadows are represented by eleven world-space half-planes.
 * For a horizontal receiver, each half-plane becomes an affine expression in
 * screen x/y after multiplying through the positive floor-ray denominator.
 * Runtime therefore needs no world-vertex transform, polygon clipping,
 * reciprocal, division, endpoint projection, or scanline intersection solve.
 *
 * Camera position remains full Q4 (1/16 world unit). Yaw uses a Q9 sine table.
 * Each line is seeded once at tile (0,9), then advanced across the 20x9 floor
 * tile region with signed 32-bit constant increments. The four internal cast
 * boundaries reuse the existing shade-0 edge vocabulary; their within-tile
 * intercepts are found with at most seven signed additions, not a divide.
 */
#if defined(__SDCC)

extern volatile uint8_t g_tspf_stage;
extern volatile uint8_t g_ts_render_stage;
#define TSP_FL_SET_STAGE(v) do { g_tspf_stage=(v); g_ts_render_stage=(v); } while(0)

#define TSP_FL_LINE_COUNT 11u
#define TSP_FL_Q9_SCALE 511L
#define TSP_FL_FOCAL 80L
#define TSP_FL_SHADOW_WORD ((uint16_t)(TSP_TILE_FLOOR|TSP_ATTR_PALETTE))

typedef struct TSPFloorLightLineDef {
    int16_t a,b;
    int32_t c;
    uint8_t h;
} TSPFloorLightLineDef;

/* Primitive Q4 world-line equations. Positive is the shadow-polygon interior.
 * Shared receiver boundaries are stored once. L2/L4/L7/L10 are cast edges. */
static const TSPFloorLightLineDef k_fl_line[TSP_FL_LINE_COUNT] = {
    {-1,  0,   1280L,16u}, /* L0  Room A x boundary, shared */
    { 0,  1,   -256L,16u}, /* L1 */
    {80,-69, -62656L,16u}, /* L2  cast A0 */
    { 0, -1,   1280L,16u}, /* L3 */
    {64, 55,-138240L,16u}, /* L4  cast A1 */
    { 1,  3,  -2464L,12u}, /* L5 */
    { 1,  0,  -1792L,12u}, /* L6  Room B x boundary, shared */
    {-23,-33,  60224L,12u}, /* L7  cast B0 */
    {-7,  9,  14368L,12u}, /* L8 */
    { 0, -1,   1344L,12u}, /* L9 */
    {-40,57,   13312L,12u} /* L10 cast B1 */
};

static uint16_t *g_fl_out_map;
static int32_t g_fl_row[TSP_FL_LINE_COUNT];
static int32_t g_fl_cur[TSP_FL_LINE_COUNT];
static int32_t g_fl_dx[TSP_FL_LINE_COUNT];
static int32_t g_fl_dy[TSP_FL_LINE_COUNT];

static int8_t fl_clamp_s8(int16_t v,int8_t lo,int8_t hi){if(v<lo)return lo;if(v>hi)return hi;return (int8_t)v;}
static int32_t fl_div8s32(int32_t v){return v>=0?(v>>3):-((-v)>>3);}

static uint8_t fl_cell_unclaimed(uint8_t row,uint8_t col){
    uint8_t ci=(uint8_t)(col+col+col+(row>>3));
    return (uint8_t)((g_polar_nt_cov_cur[ci]&k_nt_mask8[row&7u])==0u);
}

/* Shade-0 patterns are authored as two-colour floor boundaries by the lit ROM.
 * Palette 0 makes the upper side shadow; palette 1 makes the lower side shadow.
 * FLIPX handles a negative left-to-right threshold slope. */
static uint16_t fl_edge_word(int16_t local_left,int8_t slope,uint8_t shadow_below){
    uint16_t attr=shadow_below?TSP_ATTR_PALETTE:0u;uint8_t mag;int8_t off;
    if(slope<0){mag=(uint8_t)(-slope);local_left=(int16_t)(local_left-mag);attr|=TSP_ATTR_FLIPX;}else mag=(uint8_t)slope;
    if(mag>=TSP_EDGE_SLOPE_COUNT)mag=TSP_EDGE_SLOPE_COUNT-1u;
    off=fl_clamp_s8(local_left,TSP_EDGE_OFF_MIN,(int8_t)(TSP_EDGE_OFF_MIN+TSP_EDGE_OFF_COUNT-1));
    return (uint16_t)(TSP_TILE_EDGE(0u,(uint8_t)(off-TSP_EDGE_OFF_MIN),mag)|attr);
}

/* Return the integer y threshold 0..8 at one tile x edge. For dy>0 the
 * shadow side is below the line; for dy<0 it is above. Pixel-centre samples
 * are used, matching the host affine oracle. At most seven additions. */
static uint8_t fl_cross_threshold(int32_t e0,int32_t dy){
    uint8_t k;
    if(dy>0){
        if(e0>=0)return 0u;
        for(k=1u;k<8u;++k){e0+=dy;if(e0>=0)return k;}
        return 8u;
    }
    if(dy<0){
        if(e0<0)return 0u;
        for(k=1u;k<8u;++k){e0+=dy;if(e0<0)return k;}
        return 8u;
    }
    return e0>=0?8u:0u;
}

/* Build one existing shade-0 boundary tile from the cast half-plane value at
 * the tile centre. g_fl_dx/g_fl_dy are per-tile deltas; /8 gives the exact
 * per-pixel-centre affine deltas because both were constructed as multiples
 * of eight. A horizontal threshold is representable directly; near-vertical
 * cases saturate to the steepest existing pattern, matching the old bank's
 * deliberately coarse edge vocabulary. */
static uint8_t fl_cast_edge_word(uint8_t li,int32_t centre,uint16_t *word){
    int32_t dxp=fl_div8s32(g_fl_dx[li]),dyp=fl_div8s32(g_fl_dy[li]);
    int32_t e00,e70;uint8_t l0,l1;int16_t ds;
    if(!dyp)return 0u;
    /* Explicit multiplies avoid implementation-defined/undefined signed shifts
     * for negative affine values under SDCC. Constants are folded by compiler. */
    e00=centre-dxp*4L-dyp*4L;
    e70=e00+dxp*7L;
    l0=fl_cross_threshold(e00,dyp);l1=fl_cross_threshold(e70,dyp);
    if((l0==0u&&l1==0u)||(l0==8u&&l1==8u))return 0u;
    ds=(int16_t)l1-(int16_t)l0;if(ds<-7)ds=-7;if(ds>7)ds=7;
    *word=fl_edge_word((int16_t)l0,(int8_t)ds,(uint8_t)(dyp>0));
    return 1u;
}

static void fl_setup_line(uint8_t i,const TSPFloorLightLineDef *ld,const TSPState *s,int16_t sn,int16_t cs){
    int32_t lc,nf,ns,qlc;
    lc=(int32_t)ld->a*(int32_t)s->x_q4+(int32_t)ld->b*(int32_t)s->y_q4+ld->c;
    nf=(int32_t)ld->a*(int32_t)cs+(int32_t)ld->b*(int32_t)sn;
    ns=(int32_t)(-ld->a)*(int32_t)sn+(int32_t)ld->b*(int32_t)cs;
    /* qlc = 511*L(camera). Tile (0,9) has v2=9 and dx2=-151.
     * Multiplication is deliberate here: negative signed left-shift is not
     * portable C, and correctness is more valuable than hoping SDCC defines it. */
    qlc=lc*TSP_FL_Q9_SCALE;
    g_fl_row[i]=qlc*9L + nf*(32L*(int32_t)ld->h*TSP_FL_FOCAL) - ns*(16L*(int32_t)ld->h*151L);
    g_fl_dx[i]=ns*(256L*(int32_t)ld->h);
    g_fl_dy[i]=qlc*16L;
}

static void fl_setup(const TSPState *s){
    uint8_t i;int16_t sn=k_fl_sin_q9[s->yaw],cs=k_fl_sin_q9[(uint8_t)(s->yaw+64u)];
    for(i=0u;i<TSP_FL_LINE_COUNT;++i)fl_setup_line(i,&k_fl_line[i],s,sn,cs);
}

/* One pass handles both partial cast-edge ownership and solid shadow. It tests
 * only unclaimed geometry cells, preserving Polar's geometry-first semantics.
 * Polygon identities:
 *   P0 = L0 & L1 & L2       P1 = L3 & L0 & L4
 *   P2 = L5 & L6 & L7 & L8 P3 = L9 & L10 & L6
 * The non-cast tests also bound each cast edge to its authored receiver span,
 * so no separate Room-A/Room-B mask or projected endpoint clip is required. */
static void fl_materialize(void){
    uint8_t row,col,i;uint16_t ew;uint8_t o0,o1,o2,o3,p0,p1,p2,p3,edge;
    for(row=9u;row<TSP_ROWS;++row){
        for(i=0u;i<TSP_FL_LINE_COUNT;++i)g_fl_cur[i]=g_fl_row[i];
        for(col=0u;col<TSP_COLS;++col){
            if(fl_cell_unclaimed(row,col)){
                o0=(uint8_t)(g_fl_cur[0]>=0&&g_fl_cur[1]>=0);
                o1=(uint8_t)(g_fl_cur[3]>=0&&g_fl_cur[0]>=0);
                o2=(uint8_t)(g_fl_cur[5]>=0&&g_fl_cur[6]>=0&&g_fl_cur[8]>=0);
                o3=(uint8_t)(g_fl_cur[9]>=0&&g_fl_cur[6]>=0);
                p0=(uint8_t)(o0&&g_fl_cur[2]>=0);p1=(uint8_t)(o1&&g_fl_cur[4]>=0);
                p2=(uint8_t)(o2&&g_fl_cur[7]>=0);p3=(uint8_t)(o3&&g_fl_cur[10]>=0);
                edge=0u;
                if(o0&&fl_cast_edge_word(2u,g_fl_cur[2],&ew))edge=1u;
                else if(o1&&fl_cast_edge_word(4u,g_fl_cur[4],&ew))edge=1u;
                else if(o2&&fl_cast_edge_word(7u,g_fl_cur[7],&ew))edge=1u;
                else if(o3&&fl_cast_edge_word(10u,g_fl_cur[10],&ew))edge=1u;
                if(edge)put_cell(g_fl_out_map,row,col,ew);
                else if(p0||p1||p2||p3)put_cell(g_fl_out_map,row,col,TSP_FL_SHADOW_WORD);
            }
            for(i=0u;i<TSP_FL_LINE_COUNT;++i)g_fl_cur[i]+=g_fl_dx[i];
        }
        for(i=0u;i<TSP_FL_LINE_COUNT;++i)g_fl_row[i]+=g_fl_dy[i];
    }
}

static void tsp_polar_floor_light(const TSPState *s,uint16_t out_map[TSP_MAP_CELLS]){
    g_fl_out_map=out_map;
    TSP_FL_SET_STAGE(5u);fl_setup(s);
    TSP_FL_SET_STAGE(6u);fl_materialize();
    TSP_FL_SET_STAGE(7u);
}

#endif /* __SDCC */
