#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "tilesector_core.h"

typedef struct { uint8_t r,g,b; } RGB8;
static const RGB8 k_col[] = {
    {0,0,0}, {18,20,30}, {35,34,38}, {62,68,82}, {104,108,118}, {166,170,180}
};

static uint8_t wall_color(uint8_t shade) { return (uint8_t)(3u + shade); }

static uint8_t sample_tile(uint8_t tile, uint8_t x, uint8_t y) {
    if (tile == TS_TILE_CEILING) return 1u;
    if (tile == TS_TILE_FLOOR) return 2u;
    if (tile == TS_TILE_HORIZON) return y == 0u ? 0u : 2u;
    if (tile >= TS_TILE_WALL_FULL_BASE && tile < TS_TILE_WALL_TOP_BASE) {
        uint8_t q = (uint8_t)(tile - TS_TILE_WALL_FULL_BASE);
        uint8_t shade = (uint8_t)(q / TS_BORDER_COUNT);
        uint8_t b = (uint8_t)(q % TS_BORDER_COUNT);
        if (((b & 1u) && x == 0u) || ((b & 2u) && x == 7u)) return 0u;
        return wall_color(shade);
    }
    if (tile >= TS_TILE_WALL_TOP_BASE && tile < TS_TILE_WALL_BOTTOM_BASE) {
        uint8_t q = (uint8_t)(tile - TS_TILE_WALL_TOP_BASE);
        uint8_t b = (uint8_t)(q % TS_BORDER_COUNT); q /= TS_BORDER_COUNT;
        {
            uint8_t off = (uint8_t)(q & 7u), shade = (uint8_t)(q >> 3);
            if (y < off) return 1u;
            if (y == off) return 0u;
            if (((b & 1u) && x == 0u) || ((b & 2u) && x == 7u)) return 0u;
            return wall_color(shade);
        }
    }
    {
        uint8_t q = (uint8_t)(tile - TS_TILE_WALL_BOTTOM_BASE);
        uint8_t b = (uint8_t)(q % TS_BORDER_COUNT); q /= TS_BORDER_COUNT;
        {
            uint8_t off = (uint8_t)(q & 7u), shade = (uint8_t)(q >> 3);
            if (y > off) return 2u;
            if (y == off) return 0u;
            if (((b & 1u) && x == 0u) || ((b & 2u) && x == 7u)) return 0u;
            return wall_color(shade);
        }
    }
}

int main(int argc, char **argv) {
    TSState s;
    TSColumn cols[TS_COLS];
    uint16_t map[TS_MAP_CELLS];
    unsigned frames = argc > 1 ? (unsigned)atoi(argv[1]) : 0u;
    const char *out = argc > 2 ? argv[2] : "tilesector.ppm";
    unsigned f,x,y;
    FILE *fp;
    ts_reset(&s);
    for (f=0; f<frames; ++f) ts_step(&s,0);
    ts_build_tilemap(&s,map,cols);
    fp=fopen(out,"wb"); if(!fp) return 2;
    fprintf(fp,"P6\n160 144\n255\n");
    for(y=0;y<144;++y) for(x=0;x<160;++x) {
        uint8_t tile=(uint8_t)map[(y>>3)*TS_COLS+(x>>3)];
        RGB8 c=k_col[sample_tile(tile,(uint8_t)(x&7u),(uint8_t)(y&7u))];
        fputc(c.r,fp); fputc(c.g,fp); fputc(c.b,fp);
    }
    fclose(fp);
    printf("frame=%u pos=(%.2f,%.2f) yaw=%u manual=%u phase=%u\n",frames,s.x_q8/256.0,s.y_q8/256.0,s.yaw,s.manual,s.demo_phase);
    return 0;
}
