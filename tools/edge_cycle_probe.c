/*
 * EDGE_CYCLE: the dense per-class phase ring.
 *
 * A42 killed a compiled microsequence, but it stored one sequence per RUN-EDGE
 * keyed on the EXACT step (2,332 of them) and got 591 KiB with no reuse.  That
 * is not the strongest form of the idea.
 *
 * The stronger form: the renderer's own quantization is
 *     hl = clamp((iq+32)>>6, 255) >> 1        so 128 accumulator units = 1 pixel
 * and a tile row is 8 pixels, so ONE VERTICAL TILE PHASE IS 1024 UNITS.  Within
 * a known tile row the appearance therefore depends only on
 *     phase = (iq + 32) mod 1024
 * and the phase advances by `step` mod 1024.  That makes a class a DENSE RING
 * of 1024 entries, walked by pointer increment, not a per-run sequence.
 *
 * Note the identity with A42: 1024 = 64 * 16, so this phase is exactly A42's
 * (phase&63, inv&15) pair fused.  What is NEW is (a) keying the class on
 * step mod 1024 rather than the exact step, and (b) the dense ring layout.
 *
 * This probe measures reachable classes, phase coverage, cycle lengths,
 * deduplication and ROM.  usage: edge_cycle_probe [yaw_step]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64
#define PHASES 1024u
#define NPROF 4u

/* observed output per (class, phase); 0xffff = never observed */
typedef struct Cls {
    uint16_t out[PHASES];      /* packed tile code + row delta + nrows */
    uint8_t  seen[PHASES];
    uint32_t cells;
    uint32_t conflicts;
    uint8_t  used;
    int16_t  smod;             /* step mod 1024 */
    int8_t   whole;            /* step >> 10, saturated */
    uint8_t  prof;
} Cls;

#define MAXCLS 8192u
static Cls *g_cls;
static unsigned g_ncls;
static unsigned long s_cols, s_cells, s_conf_total;
static unsigned long s_cells_prof[NPROF];

static Cls *find_cls(int smod, int whole, unsigned prof) {
    unsigned i;
    for (i = 0; i < g_ncls; ++i)
        if (g_cls[i].smod == smod && g_cls[i].whole == whole && g_cls[i].prof == prof)
            return &g_cls[i];
    if (g_ncls >= MAXCLS) return 0;
    g_cls[g_ncls].used = 1;
    g_cls[g_ncls].smod = (int16_t)smod;
    g_cls[g_ncls].whole = (int8_t)whole;
    g_cls[g_ncls].prof = (uint8_t)prof;
    memset(g_cls[g_ncls].seen, 0, PHASES);
    return &g_cls[g_ncls++];
}

int main(int argc, char **argv) {
    unsigned yaw_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 8u;
    static const int8_t off[][2] = { {0,0},{7,3},{3,7},{11,5} };
    const unsigned n_off = sizeof off / sizeof off[0];
    TSPState s;
    unsigned gx, gy, yaw, oi, i, j;
    unsigned long poses = 0;

    g_cls = calloc(MAXCLS, sizeof(Cls));
    if (!g_cls) { fprintf(stderr, "oom\n"); return 1; }

    for (gy = 0; gy < GRID_H; ++gy)
    for (gx = 0; gx < GRID_W; ++gx) {
        int16_t px0 = (int16_t)(gx * CELL_Q4 + 32);
        int16_t py0 = (int16_t)(gy * CELL_Q4 + 32);
        uint16_t gi0 = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
        if (!tsp_is_walkable_q4(px0, py0)) continue;
        if (k_tspf_recipe_grid[gi0] == 0xffu) continue;
        for (oi = 0; oi < n_off; ++oi) {
            int16_t px = (int16_t)(px0 + off[oi][0]);
            int16_t py = (int16_t)(py0 + off[oi][1]);
            if (!tsp_is_walkable_q4(px, py)) continue;
            for (yaw = 0; yaw < 256u; yaw += yaw_step) {
                uint8_t ks[64], nk = 0, count = 0;
                uint8_t recipe, base_id, cond_count, lx, ly;
                uint16_t offs;
                const uint8_t *p, *b;
                memset(&s, 0, sizeof s);
                s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)yaw;
                recipe = k_tspf_recipe_grid[gi0];
                lx = (uint8_t)((uint16_t)px & 63u);
                ly = (uint8_t)((uint16_t)py & 63u);
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
                tsp_polar_renderer_reset();
                g_corner_bearing_valid = 0u;
                for (j = 0; j < nk; ++j) {
                    if (count >= TSPF_MAX_ACTIVE) break;
                    if (!project_key(ks[j], &s, &g_runs[count])) continue;
                    insert_run(count, &count);
                }
                if (!count) continue;
                ++poses;

                for (i = 0; i < count; ++i) {
                    const PolarRun *r = &g_runs[g_run_order[i]];
                    uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3), c;
                    uint8_t profile = k_tspf_profile[r->sid];
                    int16_t iq, step, n;
                    int smod, whole;
                    Cls *cl;
                    if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
                    if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
                    if (c1 < c0) continue;
                    n = (int16_t)(c1 - c0 + 1);
                    if (r->depth_plane) { c0 = r->c0; c1 = r->c1; n = (int16_t)(c1 - c0 + 1);
                                          iq = r->iq; step = r->step; }
                    else { iq = (int16_t)((int16_t)r->inv0 << 6);
                           step = (int16_t)(((int16_t)r->inv1 - (int16_t)r->inv0)
                                            * (int16_t)k_col_recip_q8[n]);
                           step = shr_signed(step, 2); }
                    smod = ((int)step) & 1023;
                    whole = ((int)step) >> 10;          /* arithmetic, signed */
                    if (whole < -2) whole = -2;
                    if (whole > 2) whole = 2;
                    if (profile >= NPROF) continue;
                    cl = find_cls(smod, whole, profile == 3u ? 1u : 0u);
                    if (!cl) continue;

                    for (c = c0; c <= c1; ++c) {
                        int a = (int)iq + 32;
                        int phase = a & 1023;
                        int32_t raw_l = (int32_t)a >> 6, raw_r = (int32_t)(a + step) >> 6;
                        uint8_t invl = (uint8_t)clamp_u8i((int16_t)raw_l, 255u);
                        uint8_t invr = (uint8_t)clamp_u8i((int16_t)raw_r, 255u);
                        uint8_t hl = (uint8_t)(invl >> 1), hr = (uint8_t)(invr >> 1);
                        int16_t tl = (int16_t)(TSPF_HORIZON - hl);
                        int16_t tr = (int16_t)(TSPF_HORIZON - hr);
                        int8_t slope = clamp_s8((int16_t)(tr - tl), -7, 7);
                        int8_t r0 = row_floor(tl < tr ? tl : tr);
                        int8_t r1 = row_floor(tl > tr ? tl : tr);
                        /* the sequence entry: the tile code for the FIRST row
                         * (shade factored out, as EDGELUT proved legal), how
                         * many rows this column spans, and how far the row
                         * moved from the previous column. */
                        uint16_t tile = edge_entry(0u, (int16_t)(tl - ((int16_t)r0 << 3)),
                                                   slope, 0u);
                        uint16_t code = (uint16_t)((tile - TSP_TILE_EDGE_BASE) & 0x7ff);
                        /* Position-INDEPENDENT entry.  The absolute tile row
                         * must NOT appear here: it is carried by the
                         * destination cursor, not by the ring.  Including it
                         * was a bug in the first draft and guaranteed
                         * conflicts wherever the same phase recurred at a
                         * different height. */
                        int hn;
                        int32_t raw_n = (int32_t)(a + step) >> 6;
                        uint8_t invn = (uint8_t)clamp_u8i((int16_t)raw_n, 255u);
                        uint16_t out;
                        hn = (int)(invn >> 1);
                        out = (uint16_t)(code
                            | ((uint16_t)((r1 - r0) & 3) << 11)
                            | ((uint16_t)((((hl >> 3) - (hn >> 3)) + 4) & 7) << 13));
                        ++s_cols; ++cl->cells; s_cells_prof[profile]++;
                        s_cells += (unsigned)(r1 - r0 + 1);
                        if (!cl->seen[phase]) { cl->seen[phase] = 1; cl->out[phase] = out; }
                        else if (cl->out[phase] != out) { ++cl->conflicts; ++s_conf_total; }
                        iq = (int16_t)(iq + step);
                    }
                }
            }
        }
    }

    /* ---------------- reporting ---------------- */
    {
        unsigned long phases_seen = 0, cls_conf = 0;
        unsigned k, kk;
        unsigned long cyc_hist[12]; memset(cyc_hist, 0, sizeof cyc_hist);
        unsigned nclasses_by_prof[NPROF]; memset(nclasses_by_prof, 0, sizeof nclasses_by_prof);
        unsigned long dup = 0;

        for (k = 0; k < g_ncls; ++k) {
            unsigned ps = 0;
            for (kk = 0; kk < PHASES; ++kk) if (g_cls[k].seen[kk]) ++ps;
            phases_seen += ps;
            if (g_cls[k].conflicts) ++cls_conf;
            if (g_cls[k].prof < NPROF) ++nclasses_by_prof[g_cls[k].prof];
        }
        /* exact duplicate rings (same observed entries on the shared support) */
        for (k = 0; k < g_ncls; ++k)
            for (kk = k + 1; kk < g_ncls; ++kk) {
                unsigned q, same = 1, shared = 0;
                for (q = 0; q < PHASES && same; ++q)
                    if (g_cls[k].seen[q] && g_cls[kk].seen[q]) {
                        ++shared;
                        if (g_cls[k].out[q] != g_cls[kk].out[q]) same = 0;
                    }
                if (same && shared >= 8) { ++dup; break; }
            }

        printf("=== EDGE_CYCLE: dense per-class phase rings ===\n");
        printf("poses %lu   columns %lu   edge cells %lu\n\n", poses, s_cols, s_cells);
        printf("cells by profile: FULL %lu  LINTEL %lu  RAISED %lu  RISER %lu\n",
               s_cells_prof[0], s_cells_prof[1], s_cells_prof[2], s_cells_prof[3]);
        printf("  FULL share %.1f%%\n\n", 100.0 * s_cells_prof[0] / s_cols);

        printf("REACHABLE CLASSES (step mod 1024, whole-tile part, top-edge family)\n");
        printf("  distinct classes                %u\n", g_ncls);
        for (k = 0; k < NPROF; ++k)
            if (nclasses_by_prof[k])
                printf("    family %u (%s)  %u classes\n", k, k ? "RISER" : "FULL/LINTEL/RAISED", nclasses_by_prof[k]);
        printf("  classes with a CONFLICT         %lu  (total conflicts %lu of %lu cols)\n",
               cls_conf, s_conf_total, s_cols);
        printf("  phase entries actually observed %lu of %u possible (%.2f%%)\n",
               phases_seen, g_ncls * PHASES, 100.0 * phases_seen / (g_ncls * (double)PHASES));
        printf("  mean phases observed per class  %.1f of 1024\n",
               (double)phases_seen / g_ncls);
        printf("  classes with an exact duplicate ring  %lu\n\n", dup);

        printf("ROM, DENSE rings of 1024 entries (what the pointer walk needs)\n");
        printf("  %-34s %10s %10s %10s\n", "", "1 B/entry", "2 B/entry", "3 B/entry");
        printf("  %-34s %9.1fK %9.1fK %9.1fK\n", "reachable classes only",
               g_ncls * 1024.0 / 1024.0, g_ncls * 2048.0 / 1024.0, g_ncls * 3072.0 / 1024.0);
        printf("  %-34s %9.1fK %9.1fK %9.1fK\n", "exhaustive 1024 x 4 profiles",
               4096 * 1024.0 / 1024.0, 4096 * 2048.0 / 1024.0, 4096 * 3072.0 / 1024.0);
        printf("  %-34s %9.1fK %9.1fK %9.1fK\n", "exhaustive 1024, FULL only",
               1024 * 1024.0 / 1024.0, 1024 * 2048.0 / 1024.0, 1024 * 3072.0 / 1024.0);
        printf("\n  16 KiB banks needed at 2 B/entry, reachable: %.1f\n",
               g_ncls * 2048.0 / 16384.0);
    }
    return 0;
}
