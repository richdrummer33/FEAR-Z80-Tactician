/*
 * VRAM_UPLOAD_CENSUS: the one pipeline cost that has never been in any budget.
 *
 * Every whole-update figure on this branch sums six Z80 kernels that between
 * them turn a camera pose into a finished 20x18 name table IN RAM.  Moving
 * that table into video memory is a separate cost and appears in none of them.
 *
 * The shipped uploader (src/tilesector_polar_ntupload_raw_gg.s) walks 18 rows
 * and, for each row whose cells CHANGED since the previous update, sets a VDP
 * address and streams the dirty interval with `otir`.  So its cost depends on
 * a pose SEQUENCE, not a single pose: how many rows changed, and how wide the
 * changed interval is on each.
 *
 * This measures those two things on real camera motion, then prices the
 * routine from its own instruction sequence.
 *
 * usage: vram_upload_census [mode]   0 = pure yaw, 1 = forward walk, 2 = both
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64

/* ---- cycle model of the shipped uploader, from its instruction sequence ----
 * Documented Z80 timings.  Straight-line code with known iteration counts, so
 * this is exact for the CPU side.  It does NOT model VDP wait states. */
#define UP_PROLOGUE   181     /* 6 push, init row/ix/iy, 6 pop, ret */
#define UP_PER_ROW    103     /* head test + loop tail, paid by all 18 rows */
#define UP_PER_DIRTY  446     /* extent read/reset, map ptr, VDP addr, port setup */
#define UP_PER_BYTE    21     /* otir */

static double upload_cost(unsigned dirty_rows, unsigned total_bytes) {
    return UP_PROLOGUE + 18.0 * UP_PER_ROW
         + (double)dirty_rows * UP_PER_DIRTY
         + (double)total_bytes * UP_PER_BYTE;
}

static uint16_t g_prev[TSP_MAP_CELLS];
static int g_have_prev;
static unsigned long s_updates, s_dirty_rows, s_bytes, s_changed_cells;
static unsigned long s_hist_rows[19];
static double s_cost_sum, s_cost_max;

static void account(const uint16_t *cur) {
    unsigned r, c, dirty = 0, bytes = 0;
    if (!g_have_prev) {
        memcpy(g_prev, cur, sizeof g_prev);
        g_have_prev = 1;
        return;                      /* first frame is a full redraw, excluded */
    }
    for (r = 0; r < TSP_ROWS; ++r) {
        int lo = -1, hi = -1;
        for (c = 0; c < TSP_COLS; ++c) {
            unsigned i = r * TSP_COLS + c;
            if (cur[i] != g_prev[i]) {
                ++s_changed_cells;
                if (lo < 0) lo = (int)c;
                hi = (int)c;
            }
        }
        if (lo >= 0) { ++dirty; bytes += (unsigned)(hi - lo + 1) * 2u; }
    }
    ++s_updates;
    s_dirty_rows += dirty;
    s_bytes += bytes;
    if (dirty < 19) ++s_hist_rows[dirty];
    {
        double t = upload_cost(dirty, bytes);
        s_cost_sum += t;
        if (t > s_cost_max) s_cost_max = t;
    }
    memcpy(g_prev, cur, sizeof g_prev);
}

static uint16_t g_map_out[TSP_MAP_CELLS];

static void render(int16_t px, int16_t py, uint8_t yaw) {
    TSPState s; uint8_t ks[64], nk = 0, count = 0;
    uint8_t recipe, base_id, cond_count, lx, ly; uint16_t gi, offs;
    const uint8_t *p, *b; unsigned i, j, gx, gy;
    memset(&s, 0, sizeof s);
    s.x_q4 = px; s.y_q4 = py; s.yaw = yaw;
    gx = (unsigned)((uint16_t)px >> 6); gy = (unsigned)((uint16_t)py >> 6);
    if (gx >= GRID_W || gy >= GRID_H) return;
    gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
    recipe = k_tspf_recipe_grid[gi];
    if (recipe == 0xffu) return;
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
    map_init(g_map_out);
    /* far->near, last writer wins, exactly as the host path draws */
    for (i = 0; i < count; ++i) draw_run(g_map_out, 0, &g_runs[g_run_order[i]]);
    account(g_map_out);
}

int main(int argc, char **argv) {
    int mode = (argc > 1) ? atoi(argv[1]) : 2;
    int16_t px, py; unsigned yaw, k;

    if (mode == 0 || mode == 2) {
        /* pure yaw, one step at a time, from several walkable positions */
        unsigned placed = 0;
        for (py = 352; py < 1400 && placed < 24; py += 64)
        for (px = 352; px < 1400 && placed < 24; px += 64) {
            if (!tsp_is_walkable_q4(px, py)) continue;
            ++placed;
            g_have_prev = 0;
            for (yaw = 0; yaw < 256u; ++yaw) render(px, py, (uint8_t)yaw);
        }
        printf("=== VRAM UPLOAD, pure yaw (1 unit per update, %u positions) ===\n",
               placed);
    }
    if (mode == 1) {
        unsigned placed = 0;
        for (yaw = 0; yaw < 256u; yaw += 32) {
            for (py = 352; py < 1400 && placed < 24; py += 128) {
                g_have_prev = 0; ++placed;
                for (px = 352; px < 1400; px += 4)
                    if (tsp_is_walkable_q4(px, py)) render(px, py, (uint8_t)yaw);
            }
        }
        printf("=== VRAM UPLOAD, forward walk (4 q4 units per update) ===\n");
    }

    if (!s_updates) { printf("no updates\n"); return 1; }
    printf("updates measured            %lu\n", s_updates);
    printf("changed cells per update    %.2f of %u\n",
           (double)s_changed_cells / s_updates, TSP_MAP_CELLS);
    printf("dirty ROWS per update       %.2f of 18\n",
           (double)s_dirty_rows / s_updates);
    printf("bytes streamed per update   %.2f  (of 720 for a full table)\n",
           (double)s_bytes / s_updates);
    printf("\ndirty-row histogram:");
    for (k = 0; k < 19; ++k)
        if (s_hist_rows[k])
            printf(" %u:%.0f%%", k, 100.0 * s_hist_rows[k] / s_updates);
    printf("\n\nUPLOAD COST, from the shipped instruction sequence\n");
    printf("  mean   %9.0f T/update\n", s_cost_sum / s_updates);
    printf("  worst  %9.0f T/update\n", s_cost_max);
    printf("  a FULL 720-byte table would be %9.0f T\n", upload_cost(18, 720));
    printf("  floor (nothing dirty at all)   %9.0f T\n", upload_cost(0, 0));
    printf("\n  NOTE: CPU cycles only.  VDP wait states during active display\n");
    printf("  are NOT modelled; otir at 21 T/byte is faster than the VDP\n");
    printf("  accepts outside VBlank, so a real machine either runs this in\n");
    printf("  VBlank or pays more.\n");
    return 0;
}
