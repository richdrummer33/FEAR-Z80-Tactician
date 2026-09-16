#ifndef RUNG1_LOCAL_PROJECTION_HOST_H
#define RUNG1_LOCAL_PROJECTION_HOST_H
/* Host transcription of the ROM's baked local-projection evaluator.
 *
 * The bake is a per-coarse-cell, per-corner quadtree of planar fits. Its runtime
 * is Z80 assembly (src/tilesector_polar_projection_gg.s) plus the cell parse in
 * projection_load_cell() (src/tilesector_polar_renderer.c), so the host build
 * has never exercised it: the host computes exact bearings with bearing_q12 and
 * the __SDCC guard compiles the baked path out. Rung 2 asks whether the baked
 * field stays continuous when the player crosses from one coarse cell to the
 * next, which cannot be answered without evaluating it.
 *
 * Both halves are transcribed here, with the ROM's own arithmetic:
 *
 *   cell parse   a 2-byte corner mask, then per present corner one depth byte
 *                (0xff = exact fallback) followed by 4 << (2*depth) bytes of
 *                leaf records, four bytes each: base_q12 little endian, then
 *                signed sx, sy.
 *
 *   evaluation   leaf = (ly >> (6-depth)) * 2^depth + (lx >> (6-depth)), with
 *                local coordinates masked to the leaf and shift = 6 - depth;
 *                bearing = (base + mul(sx,localx) + mul(sy,localy)) & 0x0fff.
 *
 *   mul          pe_scaled_mul8: magnitude times local, right shift, then
 *                re-sign. That truncates toward zero, NOT floor, and the result
 *                is taken as a single signed byte. Both details matter: an
 *                arithmetic shift would round the wrong way for negative
 *                slopes, which is exactly the kind of one-LSB divergence that
 *                would show up as a phantom discontinuity.
 *
 * The bake bytes themselves come from proj_tables.h, lifted verbatim from the
 * generated bank units the ROM is built from, so this tests what ships.
 */
#include <stdint.h>
#include "proj_tables.h"

#define PROJ_ROWS_PER_BANK 4u
#define PROJ_GRID_W 48u

static uint8_t  g_pj_depth[14];
static const uint8_t *g_pj_leaf[14];
static uint16_t g_pj_fallback_mask;
static uint16_t g_pj_present_mask;
static uint16_t g_pj_cell_bytes;

/* Returns 0 if this coarse cell has no baked record at all. */
static uint8_t proj_host_load(unsigned gx,unsigned gy)
{
    unsigned bank=gy/PROJ_ROWS_PER_BANK, local=(gy%PROJ_ROWS_PER_BANK)*PROJ_GRID_W+gx;
    uint16_t a,b; const uint8_t *p; uint16_t mask; unsigned v;
    for(v=0;v<14u;++v){ g_pj_depth[v]=0xffu; g_pj_leaf[v]=0; }
    g_pj_fallback_mask=0u; g_pj_present_mask=0u; g_pj_cell_bytes=0u;
    if(bank>=PROJ_BANKS||local+1u>=PROJ_OFF_N) return 0u;
    a=k_proj_off_b[bank][local]; b=k_proj_off_b[bank][local+1u];
    if(b<=a) return 0u;
    g_pj_cell_bytes=(uint16_t)(b-a);
    p=&k_proj_data_b[bank][a];
    mask=(uint16_t)p[0]|((uint16_t)p[1]<<8); p+=2;
    g_pj_present_mask=mask;
    for(v=0;v<14u;++v){
        if(!(mask&k_corner_mask[v])) continue;
        {   uint8_t d=*p++;
            if(d==0xffu) g_pj_fallback_mask|=k_corner_mask[v];
            else { g_pj_depth[v]=d; g_pj_leaf[v]=p; p+=(unsigned)(4u<<(unsigned)(d+d)); } }
    }
    return 1u;
}

/* pe_scaled_mul8: truncate toward zero, result is one signed byte */
static int proj_scaled_mul8(int slope,unsigned local,unsigned shift)
{
    unsigned mag = slope<0 ? (unsigned)(-slope) : (unsigned)slope;
    unsigned prod = (mag*local)>>shift;
    int8_t r = (int8_t)(uint8_t)(prod&0xFFu);
    return slope<0 ? -(int)r : (int)r;
}

/* 1 = baked value written to *out; 0 = this corner needs the exact fallback */
static uint8_t proj_host_bearing(unsigned v,int16_t x_q4,int16_t y_q4,uint16_t *out)
{
    unsigned lx,ly,depth,shift,n,leafx,leafy;
    const uint8_t *r; int base,cx,cy;
    if(v>=14u||g_pj_depth[v]==0xffu) return 0u;
    lx=(unsigned)((uint16_t)x_q4&63u); ly=(unsigned)((uint16_t)y_q4&63u);
    depth=g_pj_depth[v]; shift=6u-depth; n=1u<<depth;
    leafx=lx>>shift; leafy=ly>>shift;
    r=g_pj_leaf[v]+4u*(leafy*n+leafx);
    base=(int)r[0]|((int)r[1]<<8);
    cx=proj_scaled_mul8((int)(int8_t)r[2],lx&((1u<<shift)-1u),shift);
    cy=proj_scaled_mul8((int)(int8_t)r[3],ly&((1u<<shift)-1u),shift);
    *out=(uint16_t)((base+cx+cy)&0x0FFFu);
    return 1u;
}
#endif
