#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "tilesector_core.h"

typedef struct { uint8_t r,g,b; } RGB8;
static const RGB8 k_col[] = {
    {0,0,0}, {18,20,30}, {35,34,38}, {62,68,82}, {104,108,118}, {166,170,180}
};

static uint8_t wall_color(uint8_t shade) { return (uint8_t)(3u+shade); }

static int8_t edge_delta(uint8_t slope_index,uint8_t x) {
    static const int8_t lut[5][8] = {
        { 0, 0,-1,-1,-1,-1,-2,-2 },
        { 0, 0, 0, 0,-1,-1,-1,-1 },
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 1, 1, 1, 1 },
        { 0, 0, 1, 1, 1, 1, 2, 2 }
    };
    return lut[slope_index][x];
}

static uint8_t sample_tile(uint16_t entry,uint8_t x,uint8_t y) {
    uint16_t tile=(uint16_t)(entry&TS_TILE_ID_MASK);
    uint8_t palette1=(entry&TS_ATTR_PALETTE)?1u:0u;
    if(entry&TS_ATTR_FLIPX) x=(uint8_t)(7u-x);
    if(entry&TS_ATTR_FLIPY) y=(uint8_t)(7u-y);

    if(tile==TS_TILE_CEILING) return 1u;
    if(tile==TS_TILE_FLOOR) return 2u;
    if(tile==TS_TILE_HORIZON) return y==0u?0u:2u;

    if(tile>=TS_TILE_FULL_BASE&&tile<TS_TILE_EDGE_BASE) {
        uint16_t q=(uint16_t)(tile-TS_TILE_FULL_BASE);
        uint8_t border=(uint8_t)(q%TS_BORDER_COUNT); q/=TS_BORDER_COUNT;
        {
            uint8_t cap=(uint8_t)(q%TS_CAP_COUNT);
            uint8_t shade=(uint8_t)(q/TS_CAP_COUNT);
            if(((border&1u)&&x==0u)||((border&2u)&&x==7u)) return 0u;
            if((cap==TS_CAP_TOP&&y==0u)||(cap==TS_CAP_BOTTOM&&y==7u)) return 0u;
            return wall_color(shade);
        }
    }

    {
        uint16_t q=(uint16_t)(tile-TS_TILE_EDGE_BASE);
        uint8_t slope_index=(uint8_t)(q%TS_EDGE_SLOPE_COUNT); q/=TS_EDGE_SLOPE_COUNT;
        {
            uint8_t off_index=(uint8_t)(q%TS_EDGE_OFF_COUNT);
            uint8_t shade=(uint8_t)(q/TS_EDGE_OFF_COUNT);
            int16_t line=(int16_t)TS_EDGE_OFF_MIN+off_index+edge_delta(slope_index,x);
            if((int16_t)y<line) return palette1?2u:1u;
            if((int16_t)y==line) return 0u;
            return wall_color(shade);
        }
    }
}

int main(int argc,char **argv) {
    TSState s;
    TSColumn cols[TS_COLS];
    uint16_t map[TS_MAP_CELLS];
    unsigned frames=argc>1?(unsigned)atoi(argv[1]):0u;
    const char *out=argc>2?argv[2]:"tilesector.ppm";
    unsigned f,x,y;
    FILE *fp;
    ts_reset(&s);
    for(f=0;f<frames;++f) ts_step(&s,0u);
    ts_build_tilemap(&s,map,cols);
    fp=fopen(out,"wb"); if(!fp) return 2;
    fprintf(fp,"P6\n160 144\n255\n");
    for(y=0;y<144;++y) for(x=0;x<160;++x) {
        uint16_t entry=map[(y>>3)*TS_COLS+(x>>3)];
        RGB8 c=k_col[sample_tile(entry,(uint8_t)(x&7u),(uint8_t)(y&7u))];
        fputc(c.r,fp); fputc(c.g,fp); fputc(c.b,fp);
    }
    fclose(fp);
    printf("frame=%u pos=(%.2f,%.2f) yaw=%u manual=%u phase=%u speed=%ux\n",
           frames,s.x_q4/16.0,s.y_q4/16.0,s.yaw,s.manual,s.demo_phase,s.speed_scale);
    return 0;
}
