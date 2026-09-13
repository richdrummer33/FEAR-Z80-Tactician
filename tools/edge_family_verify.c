/*
 * EDGE_FAMILY_VERIFY: check the phase model against the RENDERER, and settle
 * whether the FULL interior's rows fall out of the two edge cursors.
 *
 * A44 reported "100.0000% exact" for the phase->tile model.  That check was
 * WRONG: it computed the expected tile with the same formula the model used,
 * so it verified self-consistency and almost nothing else.  Reading draw_run
 * properly shows why it mattered - `if(profile==FULL){tl--;tr--;}` means FULL's
 * top edge is 71-h, not 72-h, and the model never saw that.
 *
 * This recomputes the expected value with the RENDERER's own per-column code
 * (renderer lines 475-511, profile branches included) and compares.
 *
 * It also enumerates the real endpoint families.  There are five, not two, and
 * they have different phase periods because of the shifts:
 *      71 - h              FULL top                        period 1024
 *      72 - h              LINTEL/RAISED top               period 1024
 *      72 + h              FULL bottom, RISER bottom       period 1024
 *      72 - (h>>1)         LINTEL bottom                   period 2048
 *      72 + h - (h>>2)     RAISED bottom, RISER top        period 4096
 *
 * usage: edge_family_verify [yaw_step]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64
#define NFAM 5
static const char *k_famname[NFAM] = {
    "71-h      (FULL top)", "72-h      (LINTEL/RAISED top)",
    "72+h      (FULL bot, RISER bot)", "72-(h>>1) (LINTEL bot)",
    "72+h-h>>2 (RAISED bot, RISER top)"
};
static const int k_famperiod[NFAM] = { 1024, 1024, 1024, 2048, 4096 };

static int endpoint(int fam, int h) {
    switch (fam) {
    case 0: return TSPF_HORIZON - 1 - h;
    case 1: return TSPF_HORIZON - h;
    case 2: return TSPF_HORIZON + h;
    case 3: return TSPF_HORIZON - (h >> 1);
    default: return TSPF_HORIZON + h - (h >> 2);
    }
}

/* the model: predict the tile code and row advance from (fam, q, r, phase) */
static uint16_t predict(int fam, int phase, int r, int q) {
    int a = 8 * 1024 + phase, step = q * 1024 + r;
    uint8_t invl = (uint8_t)clamp_u8i((int16_t)((int32_t)a >> 6), 255u);
    uint8_t invr = (uint8_t)clamp_u8i((int16_t)((int32_t)(a + step) >> 6), 255u);
    int hl = invl >> 1, hr = invr >> 1;
    int16_t tl = (int16_t)endpoint(fam, hl), tr = (int16_t)endpoint(fam, hr);
    int8_t slope = clamp_s8((int16_t)(tr - tl), -7, 7);
    int8_t r0 = row_floor(tl < tr ? tl : tr);
    int8_t r1 = row_floor(tl > tr ? tl : tr);
    uint16_t tile = edge_entry(0u, (int16_t)(tl - ((int16_t)r0 << 3)), slope, 0u);
    return (uint16_t)(((tile - TSP_TILE_EDGE_BASE) & 0x7f)
         | ((uint16_t)((r1 - r0) & 3) << 7)
         | ((uint16_t)((((hl >> 3) - (hr >> 3)) + 8) & 15) << 9));
}

static unsigned long s_n[NFAM], s_ok[NFAM], s_clamp;
static unsigned long s_int_n, s_int_ok_first, s_int_ok_last;
static unsigned long s_fullbias_ok, s_fullbias_n;

int main(int argc, char **argv) {
    unsigned yaw_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 16u;
    static const int8_t off[][2] = { {0,0},{7,3},{3,7},{11,5} };
    TSPState s; unsigned gx, gy, yaw, oi, i, j;

    /* Is FULL's top ring just LINTEL/RAISED's rotated by one pixel (128 units)? */
    {
        int r, p, q, ok = 1;
        for (q = -2; q <= 1 && ok; ++q)
            for (r = 0; r < 1024 && ok; r += 3)
                for (p = 0; p < 1024; p += 7) {
                    ++s_fullbias_n;
                    if (predict(0, p, r, q) == predict(1, (p + 128) & 1023, r, q))
                        ++s_fullbias_ok;
                    else ok = 0;
                }
    }

    for (gy = 0; gy < GRID_H; ++gy)
    for (gx = 0; gx < GRID_W; ++gx) {
        int16_t px0 = (int16_t)(gx * CELL_Q4 + 32), py0 = (int16_t)(gy * CELL_Q4 + 32);
        uint16_t gi0 = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
        if (!tsp_is_walkable_q4(px0, py0)) continue;
        if (k_tspf_recipe_grid[gi0] == 0xffu) continue;
        for (oi = 0; oi < 4; ++oi) {
            int16_t px = (int16_t)(px0 + off[oi][0]), py = (int16_t)(py0 + off[oi][1]);
            if (!tsp_is_walkable_q4(px, py)) continue;
            for (yaw = 0; yaw < 256u; yaw += yaw_step) {
                uint8_t ks[64], nk = 0, count = 0, recipe, base_id, cond_count, lx, ly;
                uint16_t offs; const uint8_t *p, *b;
                memset(&s, 0, sizeof s);
                s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)yaw;
                recipe = k_tspf_recipe_grid[gi0];
                lx = (uint8_t)((uint16_t)px & 63u); ly = (uint8_t)((uint16_t)py & 63u);
                offs = k_tspf_recipe_off[recipe];
                p = &k_tspf_recipe_stream[offs];
                base_id = *p++; cond_count = *p++;
                b = &k_tspf_base_stream[k_tspf_base_off[base_id]];
                i = *b++;
                for (; i; --i) ks[nk++] = *b++;
                for (i = 0; i < cond_count; ++i) {
                    uint8_t key = *p++, sel = *p++;
                    if (selector_pass(sel, lx, ly)) ks[nk++] = key;
                }
                tsp_polar_renderer_reset(); g_corner_bearing_valid = 0u;
                for (j = 0; j < nk; ++j) {
                    if (count >= TSPF_MAX_ACTIVE) break;
                    if (!project_key(ks[j], &s, &g_runs[count])) continue;
                    insert_run(count, &count);
                }
                if (!count) continue;

                for (i = 0; i < count; ++i) {
                    const PolarRun *rr = &g_runs[g_run_order[i]];
                    uint8_t c0 = (uint8_t)(rr->x0 >> 3), c1 = (uint8_t)(rr->x1 >> 3), c;
                    uint8_t profile = k_tspf_profile[rr->sid];
                    int16_t iq, step, n; int q, r, ftop, fbot;
                    if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
                    if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
                    if (c1 < c0) continue;
                    n = (int16_t)(c1 - c0 + 1);
                    if (rr->depth_plane) { c0 = rr->c0; c1 = rr->c1;
                                           n = (int16_t)(c1 - c0 + 1);
                                           iq = rr->iq; step = rr->step; }
                    else { iq = (int16_t)((int16_t)rr->inv0 << 6);
                           step = (int16_t)(((int16_t)rr->inv1 - (int16_t)rr->inv0)
                                            * (int16_t)k_col_recip_q8[n]);
                           step = shr_signed(step, 2); }
                    q = ((int)step) >> 10; r = ((int)step) & 1023;
                    /* families, exactly as draw_run assigns them */
                    ftop = (profile == TSP_PROFILE_FULL) ? 0
                         : (profile == TSP_PROFILE_RISER) ? 4 : 1;
                    fbot = (profile == TSP_PROFILE_LINTEL) ? 3
                         : (profile == TSP_PROFILE_RAISED) ? 4 : 2;

                    for (c = c0; c <= c1; ++c) {
                        int a = (int)iq + 32;
                        int32_t rl = (int32_t)a >> 6, rw = (int32_t)(a + step) >> 6;
                        uint8_t invl = (uint8_t)clamp_u8i((int16_t)rl, 255u);
                        uint8_t invr = (uint8_t)clamp_u8i((int16_t)rw, 255u);
                        int hl = invl >> 1, hr = invr >> 1;
                        int16_t tl, tr, bl, br;
                        int fam, per, phase;
                        uint16_t want, got;
                        int8_t w0, w1, ifirst, ilast;
                        if (rl != (int32_t)invl || rw != (int32_t)invr) {
                            ++s_clamp; iq = (int16_t)(iq + step); continue;
                        }
                        /* the RENDERER's own per-column endpoints */
                        tl = (int16_t)(TSPF_HORIZON - hl); tr = (int16_t)(TSPF_HORIZON - hr);
                        bl = (int16_t)(TSPF_HORIZON + hl); br = (int16_t)(TSPF_HORIZON + hr);
                        if (profile == TSP_PROFILE_FULL) { tl--; tr--; }
                        if (profile == TSP_PROFILE_LINTEL) {
                            bl = (int16_t)(TSPF_HORIZON - (hl >> 1));
                            br = (int16_t)(TSPF_HORIZON - (hr >> 1));
                        } else if (profile == TSP_PROFILE_RAISED) {
                            bl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
                            br = (int16_t)(TSPF_HORIZON + hr - (hr >> 2));
                        } else if (profile == TSP_PROFILE_RISER) {
                            tl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
                            tr = (int16_t)(TSPF_HORIZON + hr - (hr >> 2));
                        }
                        /* --- top edge --- */
                        fam = ftop; per = k_famperiod[fam];
                        phase = ((a % per) + per) % per;
                        {
                            int8_t sl = clamp_s8((int16_t)(tr - tl), -7, 7);
                            int8_t q0 = row_floor(tl < tr ? tl : tr);
                            int8_t q1 = row_floor(tl > tr ? tl : tr);
                            uint16_t t = edge_entry(0u, (int16_t)(tl - ((int16_t)q0 << 3)),
                                                    sl, 0u);
                            want = (uint16_t)(((t - TSP_TILE_EDGE_BASE) & 0x7f)
                                 | ((uint16_t)((q1 - q0) & 3) << 7)
                                 | ((uint16_t)((((hl >> 3) - (hr >> 3)) + 8) & 15) << 9));
                            w0 = q0; w1 = q1;
                        }
                        got = predict(fam, phase, r, q);
                        ++s_n[fam]; if (got == want) ++s_ok[fam];
                        /* --- bottom edge --- */
                        fam = fbot; per = k_famperiod[fam];
                        phase = ((a % per) + per) % per;
                        {
                            int8_t sl = clamp_s8((int16_t)(br - bl), -7, 7);
                            int8_t q0 = row_floor(bl < br ? bl : br);
                            int8_t q1 = row_floor(bl > br ? bl : br);
                            uint16_t t = edge_entry(0u, (int16_t)(bl - ((int16_t)q0 << 3)),
                                                    sl, 0u);
                            uint16_t w = (uint16_t)(((t - TSP_TILE_EDGE_BASE) & 0x7f)
                                 | ((uint16_t)((q1 - q0) & 3) << 7)
                                 | ((uint16_t)((((hl >> 3) - (hr >> 3)) + 8) & 15) << 9));
                            ++s_n[fam];
                            if (predict(fam, phase, r, q) == w) ++s_ok[fam];
                            /* --- THE INTERIOR IDENTITY --- */
                            ifirst = (int8_t)(row_floor(tl > tr ? tl : tr) + 1);
                            ilast  = (int8_t)(row_floor(bl < br ? bl : br) - 1);
                            ++s_int_n;
                            if (ifirst == (int8_t)(w1 + 1)) ++s_int_ok_first;
                            if (ilast == (int8_t)(q0 - 1)) ++s_int_ok_last;
                        }
                        iq = (int16_t)(iq + step);
                    }
                }
            }
        }
    }

    printf("=== EDGE_FAMILY_VERIFY: model vs the RENDERER ===\n\n");
    printf("FULL top is LINTEL/RAISED top rotated by 128 units: %lu/%lu  %s\n\n",
           s_fullbias_ok, s_fullbias_n,
           s_fullbias_ok == s_fullbias_n ? "YES - one ring serves both" : "NO");
    printf("%-36s %12s %12s %9s %s\n", "endpoint family", "columns", "exact",
           "period", "");
    for (int f = 0; f < NFAM; ++f)
        if (s_n[f])
            printf("%-36s %12lu %12lu %9d %s\n", k_famname[f], s_n[f], s_ok[f],
                   k_famperiod[f],
                   s_ok[f] == s_n[f] ? "EXACT" : "*** MISMATCH ***");
    printf("\ninverse-depth clamped (separate path) %lu\n", s_clamp);

    printf("\n=== the interior identity ===\n");
    printf("  interior FIRST row == top-edge last row + 1    %lu / %lu  %s\n",
           s_int_ok_first, s_int_n,
           s_int_ok_first == s_int_n ? "EXACT" : "*** FAILS ***");
    printf("  interior LAST  row == bottom-edge first row - 1 %lu / %lu  %s\n",
           s_int_ok_last, s_int_n,
           s_int_ok_last == s_int_n ? "EXACT" : "*** FAILS ***");
    return 0;
}
