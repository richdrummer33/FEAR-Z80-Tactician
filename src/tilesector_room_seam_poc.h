#ifndef TILESECTOR_ROOM_SEAM_POC_H
#define TILESECTOR_ROOM_SEAM_POC_H

#include <stdint.h>

typedef struct TSPSeamPocAperture {
    int16_t x_q4;
    int16_t y0_q4;
    int16_t y1_q4;
} TSPSeamPocAperture;

typedef struct TSPSeamPocVerticalWall {
    int16_t x_q4;
    int16_t y0_q4;
    int16_t y1_q4;
} TSPSeamPocVerticalWall;

typedef struct TSPSeamPocContract {
    TSPSeamPocAperture old_aperture;
    TSPSeamPocAperture new_aperture;
    TSPSeamPocVerticalWall old_occluder;
    TSPSeamPocVerticalWall new_occluder;

    /* Central vertical leg where neither unique room is visible. */
    int16_t safe_x0_q4,safe_x1_q4;
    int16_t retire_old_y_q4;
    int16_t reveal_new_y_q4;
} TSPSeamPocContract;

const TSPSeamPocContract *tsp_seam_poc_contract(void);

/* Exact Q4 line test against one vertical opaque wall. */
uint8_t tsp_seam_poc_ray_blocked(int16_t cx_q4,int16_t cy_q4,
                                 int16_t tx_q4,int16_t ty_q4,
                                 const TSPSeamPocVerticalWall *wall);

#endif
