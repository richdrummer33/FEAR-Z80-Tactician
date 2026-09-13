/*
 * TARGET_SOLVE_CENSUS: the precision-survival census on the GAME GEAR path.
 *
 * A48's 255 -> 16 iq collapse came from the HOST fallback path, where
 * iq = inv0 << 6.  The shipped GG build enables TSPF_SCREEN_DEPTH_PLANE, and
 * that changes the question completely: when screen_depth_plane() succeeds,
 * project_key RETURNS BEFORE inv_at_invd is ever called.  So the first thing
 * to measure is not how to make inv_at_invd cheaper - it is how often it runs
 * at all on target.
 *
 * Equivalence: screen_depth_plane is reproduced here VERBATIM from
 * src/tilesector_polar_renderer.c (the copy is diffed against the source by
 * tools/target_solve_equiv.py, which fails the build if they drift), and the
 * coefficient tables are the generated ones the GG build links.
 *
 * usage: target_solve_census [yaw_step]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"
#include "tilesector_polar_depthplane_lut.h"

/* the generated coefficient tables, pulled in directly */
#define k_depth_nf_q7 k_depth_nf_q7_gen
#define k_depth_stepfac_q4 k_depth_stepfac_q4_gen
#define tsp_polar_depthplane_load tsp_polar_depthplane_load_gen
#include "tilesector_polar_depthplane.c"
#undef k_depth_nf_q7
#undef k_depth_stepfac_q4
#undef tsp_polar_depthplane_load

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64

static unsigned long g_dp_iters;
static int8_t g_nf[TSPF_DEPTH_NORMAL_CLASS_COUNT];
static int8_t g_sf[TSPF_DEPTH_NORMAL_CLASS_COUNT];

/* ---- VERBATIM from renderer lines 393-412, tables swapped for the loaded
 * per-yaw arrays exactly as the shipped code does. ---- */
static uint8_t dp_solve(uint8_t sid, uint8_t invd, uint8_t c0, uint8_t c1,
                        PolarRun *r) {
    uint8_t cls = k_tspf_depth_normal_class[sid];
    int8_t nf = g_nf[cls], sf = g_sf[cls];
    int16_t iq = shr_signed((int16_t)((int16_t)invd * (int16_t)nf), 1);
    int16_t step = shr_signed((int16_t)((int16_t)invd * (int16_t)sf), 4);
    int16_t endq = iq, midq;
    uint8_t i, n = (uint8_t)(c1 - c0 + 1u);
    if (c0 < 10u) { for (i = c0; i < 10u; ++i) { iq = (int16_t)(iq - step); ++g_dp_iters; } }
    else { for (i = 10u; i < c0; ++i) { iq = (int16_t)(iq + step); ++g_dp_iters; } }
    endq = iq;
    for (i = 0u; i < n; ++i) { endq = (int16_t)(endq + step); ++g_dp_iters; }
    if ((iq < 0 && endq > 0) || (iq > 0 && endq < 0)) return 0u;
    if (iq < 0 || endq < 0) { iq = (int16_t)-iq; endq = (int16_t)-endq;
                              step = (int16_t)-step; }
    midq = (int16_t)((iq + endq) >> 1);
    r->c0 = c0; r->c1 = c1; r->iq = iq; r->step = step; r->depth_plane = 1u;
    r->inv_mid = clamp_u8i((int16_t)((midq + 32) >> 6), 255u);
    return 1u;
}

#define HN (1u << 22)
static uint32_t *g_set; static unsigned long g_setn;
static void sadd(uint64_t h) {
    unsigned i = (unsigned)((h * 1181783497276652981ull) >> 42) & (HN - 1u);
    uint32_t v = (uint32_t)(h ? h : 1);
    for (;;) {
        if (!g_set[i]) { g_set[i] = v; ++g_setn; return; }
        if (g_set[i] == v) return;
        i = (i + 1u) & (HN - 1u);
    }
}
static void sreset(void) { memset(g_set, 0, HN * sizeof(uint32_t)); g_setn = 0; }

static unsigned long s_runs, s_dp_ok, s_dp_fail;
static uint8_t seen_iq[65536], seen_step[65536], seen_phase[4096];
static uint8_t seen_invd[256], seen_mid[256], seen_c0c1[512];
static unsigned long s_mul_dp, s_mul_fb;

int main(int argc, char **argv) {
    unsigned yaw_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 16u;
    static const int8_t off[][2] = { {0,0},{7,3},{3,7},{11,5} };
    TSPState s; unsigned gx, gy, yaw, oi, i, j;
    unsigned long poses = 0;
    g_set = calloc(HN, sizeof(uint32_t));
    if (!g_set) return 1;

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
                ++poses;
                /* the GG build refreshes the per-yaw coefficients once */
                tsp_polar_depthplane_load_gen((uint8_t)yaw, g_nf, g_sf);

                for (i = 0; i < count; ++i) {
                    PolarRun *rr = &g_runs[g_run_order[i]];
                    uint8_t c0 = (uint8_t)(rr->x0 >> 3), c1 = (uint8_t)(rr->x1 >> 3);
                    uint8_t invd;
                    PolarRun t = *rr;
                    if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
                    if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
                    if (c1 < c0) continue;
                    ++s_runs;
                    invd = inv_for_dq4(wall_d_q4(rr->sid, k_tspf_seg_anchor[rr->sid], &s));
                    seen_invd[invd] = 1;
                    if (dp_solve(rr->sid, invd, c0, c1, &t)) {
                        int a;
                        ++s_dp_ok;
                        s_mul_dp += 2;                 /* invd*nf, invd*sf */
                        seen_iq[(uint16_t)t.iq] = 1;
                        seen_step[(uint16_t)t.step] = 1;
                        seen_mid[t.inv_mid] = 1;
                        seen_c0c1[(unsigned)c0 * 20u + c1] = 1;
                        a = (int)t.iq + 32;
                        seen_phase[((a % 1024) + 1024) % 1024] = 1;
                        sadd(((uint64_t)(uint16_t)t.step << 16)
                             | (uint32_t)(((a % 1024) + 1024) % 1024));
                    } else {
                        ++s_dp_fail;
                        s_mul_fb += 4;                 /* two inv_at_invd calls */
                    }
                }
            }
        }
    }

    {
        unsigned long niq = 0, nstep = 0, nph = 0, nid = 0, nmid = 0, ncc = 0;
        unsigned k;
        for (k = 0; k < 65536; ++k) { niq += seen_iq[k]; nstep += seen_step[k]; }
        for (k = 0; k < 1024; ++k) nph += seen_phase[k];
        for (k = 0; k < 256; ++k) { nid += seen_invd[k]; nmid += seen_mid[k]; }
        for (k = 0; k < 400; ++k) ncc += seen_c0c1[k];

        printf("=== TARGET_SOLVE_CENSUS (Game Gear depth-plane path) ===\n");
        printf("poses %lu   visible runs %lu\n\n", poses, s_runs);
        printf("DEPTH-PLANE SUCCESS RATE\n");
        printf("  screen_depth_plane succeeded  %lu  %.2f%%\n",
               s_dp_ok, 100.0 * s_dp_ok / s_runs);
        printf("  fell back to inv_at_invd x2   %lu  %.2f%%\n",
               s_dp_fail, 100.0 * s_dp_fail / s_runs);
        printf("\n  => inv_at_invd runs on %.2f%% of runs on TARGET.\n",
               100.0 * s_dp_fail / s_runs);
        printf("\n  dp_solve 16-bit add iterations: %.2f per run "
               "(%.2f per successful solve)\n",
               (double)g_dp_iters / s_runs, (double)g_dp_iters / s_dp_ok);
        printf("\nMULTIPLIES PER RUN, target path\n");
        printf("  depth-plane (invd*nf, invd*sf)  %.3f\n", (double)s_mul_dp / s_runs);
        printf("  fallback (2 x inv_at_invd)      %.3f\n", (double)s_mul_fb / s_runs);
        printf("  wall_d_q4 + inv_for_dq4          (unchanged, always run)\n");
        printf("\nDISTINCT VALUES on the depth-plane path\n");
        printf("  invd                 %6lu of 256\n", nid);
        printf("  iq                   %6lu\n", niq);
        printf("  step                 %6lu\n", nstep);
        printf("  phase (iq+32 mod 1024) %4lu of 1024\n", nph);
        printf("  inv_mid              %6lu of 256\n", nmid);
        printf("  (c0,c1) pairs        %6lu of 400\n", ncc);
        printf("  distinct (step,phase) A46 keys  %lu\n", g_setn);
    }
    return 0;
}
