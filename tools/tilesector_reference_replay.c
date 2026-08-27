#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "tilesector_core.h"

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t rds16(const uint8_t *p) {
    return (int16_t)rd16(p);
}

int main(int argc,char **argv) {
    if(argc!=3) {
        fprintf(stderr,"usage: %s states.bin reference-maps.bin\n",argv[0]);
        return 2;
    }
    FILE *in=fopen(argv[1],"rb");
    FILE *out=fopen(argv[2],"wb");
    if(!in||!out) {
        fprintf(stderr,"open failed\n");
        return 2;
    }

    uint8_t raw[16];
    uint16_t map[TS_MAP_CELLS];
    TSColumn cols[TS_COLS];
    unsigned frames=0;

    while(fread(raw,1,sizeof(raw),in)==sizeof(raw)) {
        TSState s;
        s.x_q4=rds16(raw+0);
        s.y_q4=rds16(raw+2);
        s.yaw=raw[4];
        s.speed_q4=rds16(raw+5);
        s.strafe_q4=rds16(raw+7);
        s.turn_q4=rds16(raw+9);
        s.speed_scale=raw[11];
        s.manual=raw[12];
        s.demo_phase=raw[13];
        s.demo_ticks=rd16(raw+14);

        ts_build_tilemap(&s,map,cols);

        for(unsigned i=0;i<TS_MAP_CELLS;++i) {
            const uint16_t w=map[i];
            fputc((int)(w&0xffu),out);
            fputc((int)(w>>8),out);
        }
        ++frames;
    }

    if(!feof(in)) {
        fprintf(stderr,"state dump read error\n");
        return 3;
    }
    fclose(in);
    fclose(out);
    printf("reference replay frames=%u\n",frames);
    return 0;
}
