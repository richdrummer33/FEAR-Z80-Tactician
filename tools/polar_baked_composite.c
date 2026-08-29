/*
 * Host-side 8x8 semantic compositor for baked Polar name-table states.
 *
 * Runtime stays deliberately stupid: this file resolves partial edge coverage
 * on the PC, interns the final 8x8 patterns, and emits banked Game Gear tile
 * data. The Z80 later receives only final tile IDs + name-table flips.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "polar_baked_composite.h"

#define MAX_HW_TILES 512u
#define PIXELS_PER_TILE 64u
#define BYTES_PER_TILE 32u
#define TILE_BANKS 4u
#define TILES_PER_BANK 128u

enum {
    SEM_BLACK=0u,
    SEM_CEILING=1u,
    SEM_FLOOR=2u,
    SEM_FAR=3u,
    SEM_MID=4u,
    SEM_NEAR=5u
};

static const int8_t k_edge_lut[8][8] = {
    {0,0,0,0,0,0,0,0},{0,0,0,0,1,1,1,1},{0,0,1,1,1,1,2,2},{0,0,1,1,2,2,3,3},
    {0,1,1,2,2,3,3,4},{0,1,1,2,3,4,4,5},{0,1,2,3,3,4,5,6},{0,1,2,3,4,5,6,7}
};

static uint8_t g_cells[TSP_MAP_CELLS][PIXELS_PER_TILE];
static uint8_t g_tiles[MAX_HW_TILES][PIXELS_PER_TILE];
static uint16_t g_tile_count;
static uint8_t g_ready;

static void die(const char *msg){
    fprintf(stderr,"fatal: %s\n",msg);
    exit(2);
}

static void tile_fill(uint8_t p[PIXELS_PER_TILE],uint8_t c){
    memset(p,c,PIXELS_PER_TILE);
}
static void make_horizon(uint8_t p[PIXELS_PER_TILE]){
    uint8_t x,y;
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x)
        p[(uint16_t)y*8u+x]=(y==0u)?SEM_BLACK:SEM_FLOOR;
}
static void ensure_init(void){
    if(g_ready)return;
    tile_fill(g_tiles[0],SEM_CEILING);
    tile_fill(g_tiles[1],SEM_FLOOR);
    make_horizon(g_tiles[2]);
    g_tile_count=3u;
    g_ready=1u;
}

static uint8_t shade_sem(uint8_t shade){
    return (uint8_t)(SEM_FAR + (shade>2u?2u:shade));
}

static void generic_unflipped_indices(uint16_t id,uint8_t out[PIXELS_PER_TILE]){
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
            out[(uint16_t)y*8u+x]=black?0u:color;
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
    die("renderer emitted non-generic tile before baked export");
}

static void decode_word(uint16_t word,uint8_t sem[PIXELS_PER_TILE],uint8_t mask[PIXELS_PER_TILE]){
    uint16_t id=(uint16_t)(word&TSP_TILE_ID_MASK);
    uint8_t raw[PIXELS_PER_TILE];
    uint8_t x,y,fx=(uint8_t)((word&TSP_ATTR_FLIPX)!=0u);
    uint8_t fy=(uint8_t)((word&TSP_ATTR_FLIPY)!=0u);
    uint8_t pal=(uint8_t)((word&TSP_ATTR_PALETTE)!=0u);
    uint8_t is_edge=(uint8_t)(id>=TSP_TILE_EDGE_BASE && id<TSP_GENERATED_TILE_COUNT);
    generic_unflipped_indices(id,raw);
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){
        uint8_t sx=fx?(uint8_t)(7u-x):x;
        uint8_t sy=fy?(uint8_t)(7u-y):y;
        uint8_t v=raw[(uint16_t)sy*8u+sx];
        uint16_t i=(uint16_t)y*8u+x;
        mask[i]=(uint8_t)(!is_edge || v!=1u);
        /* Palette 1 exists only to reinterpret edge "outside" as floor.
         * Store semantic colors instead; baked tiles later use palette 0 and
         * encode floor explicitly as color index 2. */
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
    uint8_t sem[PIXELS_PER_TILE],mask[PIXELS_PER_TILE];
    uint8_t *dst;
    uint8_t i;
    if(row>=TSP_ROWS||col>=TSP_COLS)die("compositor cell out of range");
    decode_word(word,sem,mask);
    dst=g_cells[(uint16_t)row*TSP_COLS+col];
    for(i=0u;i<PIXELS_PER_TILE;++i)if(mask[i])dst[i]=sem[i];
}

static void flip_pattern(const uint8_t src[PIXELS_PER_TILE],uint8_t dst[PIXELS_PER_TILE],
                         uint8_t fx,uint8_t fy){
    uint8_t x,y;
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){
        uint8_t sx=fx?(uint8_t)(7u-x):x;
        uint8_t sy=fy?(uint8_t)(7u-y):y;
        dst[(uint16_t)y*8u+x]=src[(uint16_t)sy*8u+sx];
    }
}
static int pattern_cmp(const uint8_t *a,const uint8_t *b){
    return memcmp(a,b,PIXELS_PER_TILE);
}

static uint16_t intern_pattern(const uint8_t orig[PIXELS_PER_TILE]){
    uint8_t cand[4][PIXELS_PER_TILE];
    uint8_t best=0u,k;
    uint16_t id;
    flip_pattern(orig,cand[0],0u,0u);
    flip_pattern(orig,cand[1],1u,0u);
    flip_pattern(orig,cand[2],0u,1u);
    flip_pattern(orig,cand[3],1u,1u);
    for(k=1u;k<4u;++k)if(pattern_cmp(cand[k],cand[best])<0)best=k;
    for(id=0u;id<g_tile_count;++id)
        if(pattern_cmp(g_tiles[id],cand[best])==0)break;
    if(id==g_tile_count){
        if(g_tile_count>=MAX_HW_TILES)die("baked composite tile dictionary exceeded 512 hardware tiles");
        memcpy(g_tiles[g_tile_count],cand[best],PIXELS_PER_TILE);
        ++g_tile_count;
    }
    return (uint16_t)(id |
        ((best&1u)?TSP_ATTR_FLIPX:0u) |
        ((best&2u)?TSP_ATTR_FLIPY:0u));
}

void tsp_host_composite_export(uint16_t out[TSP_MAP_CELLS]){
    uint16_t i;
    ensure_init();
    for(i=0u;i<TSP_MAP_CELLS;++i)out[i]=intern_pattern(g_cells[i]);
}

uint16_t tsp_host_composite_tile_count(void){
    ensure_init();
    return g_tile_count;
}

static void encode_4bpp(const uint8_t sem[PIXELS_PER_TILE],uint8_t out[BYTES_PER_TILE]){
    uint8_t x,y,p;
    memset(out,0,BYTES_PER_TILE);
    for(y=0u;y<8u;++y)for(x=0u;x<8u;++x){
        uint8_t c=sem[(uint16_t)y*8u+x];
        uint8_t bit=(uint8_t)(0x80u>>x);
        for(p=0u;p<4u;++p)
            if(c&(uint8_t)(1u<<p))out[(uint16_t)y*4u+p]|=bit;
    }
}

static void emit_bank(const char *dir,unsigned bank,uint16_t first,uint16_t count){
    char path[512];
    FILE *f;
    uint16_t i;
    int n=snprintf(path,sizeof(path),"%s/polar_demo_tile_bank%u.c",dir,bank);
    if(n<0||(size_t)n>=sizeof(path))die("tile bank output path too long");
    f=fopen(path,"w");
    if(!f){fprintf(stderr,"cannot write %s: %s\n",path,strerror(errno));exit(2);}
    fprintf(f,
        "/* GENERATED baked-composite Game Gear tile bank %u. */\n"
        "#include <stdint.h>\n#include <gbdk/platform.h>\n"
        "#pragma bank 255\nBANKREF(polar_demo_tile_bank%u)\n\n"
        "static const uint8_t k_tiles[%uu] = {\n",
        bank,bank,(unsigned)(count?count*BYTES_PER_TILE:1u));
    if(!count)fprintf(f,"0,\n");
    for(i=0u;i<count;++i){
        uint8_t bytes[BYTES_PER_TILE],j;
        encode_4bpp(g_tiles[(uint16_t)(first+i)],bytes);
        for(j=0u;j<BYTES_PER_TILE;++j){
            uint32_t flat=(uint32_t)i*BYTES_PER_TILE+j;
            if((flat&15u)==0u)fprintf(f,"    ");
            fprintf(f,"%u,",(unsigned)bytes[j]);
            if((flat&15u)==15u)fprintf(f,"\n");else fputc(' ',f);
        }
    }
    fprintf(f,
        "};\n\n"
        "void tsp_polar_demo_tile_bank%u(void) BANKED {\n"
        "    if(%uu)set_bkg_4bpp_data(%uu,%uu,k_tiles);\n"
        "}\n",
        bank,(unsigned)count,(unsigned)first,(unsigned)count);
    fclose(f);
}

int tsp_host_composite_emit_tiles(const char *dir){
    char path[512];
    FILE *f;
    unsigned bank;
    ensure_init();
    for(bank=0u;bank<TILE_BANKS;++bank){
        uint16_t first=(uint16_t)(bank*TILES_PER_BANK);
        uint16_t count=0u;
        if(first<g_tile_count){
            uint16_t remain=(uint16_t)(g_tile_count-first);
            count=remain>TILES_PER_BANK?TILES_PER_BANK:remain;
        }
        emit_bank(dir,bank,first,count);
    }

    if(snprintf(path,sizeof(path),"%s/polar_demo_tiles_dispatch.c",dir)<0)
        die("tile dispatch path error");
    f=fopen(path,"w");
    if(!f)die("cannot write tile dispatcher");
    fprintf(f,
        "/* GENERATED baked-composite tile loader. */\n"
        "#include <stdint.h>\n#include <gbdk/platform.h>\n\n");
    for(bank=0u;bank<TILE_BANKS;++bank)
        fprintf(f,"void tsp_polar_demo_tile_bank%u(void) BANKED;\n",bank);
    fprintf(f,"\nvoid tsp_polar_demo_tiles_init(void){\n");
    for(bank=0u;bank<TILE_BANKS;++bank)
        fprintf(f,"    tsp_polar_demo_tile_bank%u();\n",bank);
    fprintf(f,"}\n");
    fclose(f);

    if(snprintf(path,sizeof(path),"%s/polar_demo_tiles_meta.h",dir)<0)
        die("tile meta path error");
    f=fopen(path,"w");
    if(!f)die("cannot write tile metadata");
    fprintf(f,
        "/* GENERATED baked-composite tile metadata. */\n"
        "#ifndef POLAR_DEMO_TILES_META_H\n#define POLAR_DEMO_TILES_META_H\n"
        "#define POLAR_DEMO_TILE_COUNT %uu\n"
        "#define POLAR_DEMO_TILE_BANK_COUNT %uu\n"
        "#endif\n",
        (unsigned)g_tile_count,TILE_BANKS);
    fclose(f);
    return 1;
}
