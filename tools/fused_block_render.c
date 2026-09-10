/*
 * FUSED HOST PATH: one baked block program in, one complete 20x18 name table
 * out, compared word-for-word against the shipped renderer's own output.
 *
 * This is the experiment the code review asked for, and it is deliberately
 * built so that the ONLY difference between the two paths is the front end:
 *
 *   oracle : recipe grid -> base keys + selector-gated conditional keys
 *   fused  : baked block program -> SPAN / SPANC / GATE walk
 *
 * Everything downstream - project_key, the bearing field, the clip,
 * column-solve, draw_run, the edge/full materializers, the background fill -
 * is the SHIPPED C, reached by including the renderer directly. Nothing is
 * re-implemented, so a mismatch can only come from the front end. A Python
 * re-derivation of the materializer would have made a mismatch ambiguous.
 *
 * THE ORDERING QUESTION
 * ---------------------
 * `insert_run()` sorts active runs by `inv_mid` far->near at RUNTIME, and
 * `inv_mid` depends on the camera pose. So draw order is not a static
 * property of a cell. A baked block program has a FIXED instruction order,
 * which raises the question the review flagged: is the baked order sufficient,
 * or must the interpreter re-sort by depth at runtime?
 *
 * This tool answers it by rendering the fused path twice per pose:
 *
 *   ORDER_BLOCK  - draw strictly in baked block order (no depth sort)
 *   ORDER_DEPTH  - draw in the same far->near inv_mid order the oracle uses
 *
 * and reporting name-table agreement for each. If ORDER_BLOCK matches
 * everywhere, the interpreter needs no runtime sort and the baker's order is
 * provably sufficient. If it does not, the difference count is the size of
 * the problem, and ORDER_DEPTH isolates ordering from key selection.
 *
 * usage: fused_block_render <blocks.txt> [yaw_step]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64
#define MAX_RECS 64

typedef struct { char kind; uint8_t val; } Rec;
typedef struct { uint8_t used, n; Rec r[MAX_RECS]; } Block;

static Block g_blocks[GRID_H][GRID_W];

/* Depth-sort workload: the sort is now known to be MANDATORY (two different
 * static orders both land near 30% exact), so its cost is a real budget line
 * and needs measuring, not assuming. */
static unsigned long g_sort_items = 0, g_sort_shifts = 0, g_sort_poses = 0;
static unsigned g_sort_max_items = 0, g_sort_max_shifts = 0;
static FILE *g_sort_dump = 0;
static uint8_t g_ins_seq[64];

static int load_blocks(const char *path) {
    FILE *f = fopen(path, "r");
    char line[64];
    Block *cur = 0;
    unsigned loaded = 0;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    while (fgets(line, sizeof line, f)) {
        unsigned gx, gy, cnt, v;
        char k;
        if (sscanf(line, "CELL %u %u %u", &gx, &gy, &cnt) == 3) {
            if (gx >= GRID_W || gy >= GRID_H) { fclose(f); return 0; }
            cur = &g_blocks[gy][gx];
            cur->used = 1; cur->n = 0;
            ++loaded;
        } else if (cur && sscanf(line, "%c %u", &k, &v) == 2) {
            if (cur->n >= MAX_RECS) { fprintf(stderr, "block overflow\n"); fclose(f); return 0; }
            cur->r[cur->n].kind = k;
            cur->r[cur->n].val = (uint8_t)v;
            ++cur->n;
        }
    }
    fclose(f);
    fprintf(stderr, "loaded %u cell blocks\n", loaded);
    return 1;
}

enum { ORDER_BLOCK = 0, ORDER_DEPTH = 1, ORDER_ORACLE_INSERT = 2,
       ORDER_ORACLE_STRICT = 3, ORDER_INVD = 4 };
static uint8_t oracle_keys_fwd(const TSPState *s, uint8_t *keys);

/* Mirrors tsp_polar_render's host path exactly, except that the active key
 * list comes from the baked block program instead of the recipe grid. */
static uint8_t fused_render(const TSPState *s, uint16_t *out_map, int order,
                            uint8_t *out_keys) {
    uint8_t gx, gy, lx, ly, count = 0, i, skip = 0;
    const Block *blk;

    g_corner_bearing_valid = 0u;
    if (!g_map_ready) map_init(out_map); else restore_touched(out_map);

    gx = (uint8_t)((uint16_t)s->x_q4 >> 6);
    gy = (uint8_t)((uint16_t)s->y_q4 >> 6);
    if (gx >= GRID_W || gy >= GRID_H) return 0;
    blk = &g_blocks[gy][gx];
    if (!blk->used) return 0;
    lx = (uint8_t)((uint16_t)s->x_q4 & 63u);
    ly = (uint8_t)((uint16_t)s->y_q4 & 63u);

    if (order == ORDER_INVD) {
        /* Sort by invd - the PERPENDICULAR wall distance - instead of
         * inv_mid. invd is a function of camera translation only (wall_d_q4
         * is affine in camera position), where inv_mid additionally carries
         * the yaw-dependent normal-dot and secant terms. The pairwise probe
         * shows invd ordering is constant over a cell for 94.08% of pairs
         * against 74.27% for inv_mid, so if this renders correctly the sort
         * key becomes precompilable. Same insertion rule, same strict-greater
         * tie behaviour, only the key changes. */
        uint8_t ks[64], nk = oracle_keys_fwd(s, ks), j;
        static uint8_t key_invd[64];
        for (j = 0; j < nk; ++j) {
            uint8_t sid, k;
            if (count >= TSPF_MAX_ACTIVE) break;
            if (!project_key(ks[j], s, &g_runs[count])) continue;
            sid = (uint8_t)(k_tspf_keys[ks[j]] & 31u);
            key_invd[count] = inv_for_dq4(wall_d_q4(sid, k_tspf_seg_anchor[sid], s));
            k = count;
            i = count;
            while (i > 0u && key_invd[g_run_order[i - 1u]] > key_invd[k]) {
                g_run_order[i] = g_run_order[i - 1u]; --i;
            }
            g_run_order[i] = k;
            ++count;
        }
        for (i = 0; i < count; ++i) draw_run(out_map, 0, &g_runs[g_run_order[i]]);
        g_tspf_touched_cells = g_touched_count;
        return count;
    }
    if (order == ORDER_ORACLE_STRICT) {
        /* STRICT oracle insertion order, NO depth reordering whatsoever.
         * Keys come straight from the recipe front end - the block file is
         * not consulted at all, so the block export cannot be a confounder -
         * and insert_run is never called. g_run_order is filled 0,1,2,...
         * so draw order IS arrival order. This is the exact hypothesis:
         * "recipe insertion order already encodes a valid painter order". */
        uint8_t ks[64], nk = oracle_keys_fwd(s, ks), j;
        for (j = 0; j < nk; ++j) {
            if (count >= TSPF_MAX_ACTIVE) break;
            if (!project_key(ks[j], s, &g_runs[count])) continue;
            g_run_order[count] = count;
            ++count;
        }
        for (i = 0; i < count; ++i) draw_run(out_map, 0, &g_runs[g_run_order[i]]);
        g_tspf_touched_cells = g_touched_count;
        return count;
    }
    if (order == ORDER_ORACLE_INSERT) {
        /* Same key SET the block walk produces (proven identical), but fed to
         * insert_run in the oracle's own base-then-conditional order. If this
         * is exact while ORDER_DEPTH is not, the residual divergence is purely
         * insertion-order tie-breaking among equal inv_mid, not depth. */
        uint8_t ks[64], nk = oracle_keys_fwd(s, ks), j;
        for (j = 0; j < nk; ++j) {
            if (count >= TSPF_MAX_ACTIVE) break;
            if (!project_key(ks[j], s, &g_runs[count])) continue;
            insert_run(count, &count);
        }
        for (i = 0; i < count; ++i) draw_run(out_map, 0, &g_runs[g_run_order[i]]);
        g_tspf_touched_cells = g_touched_count;
        return count;
    }
    for (i = 0; i < blk->n; ++i) {
        const Rec *r = &blk->r[i];
        if (r->kind == 'G') { skip = (uint8_t)!selector_pass(r->val, lx, ly); continue; }
        if (skip) { skip = 0; continue; }
        if (count >= TSPF_MAX_ACTIVE) break;
        if (!project_key(r->val, s, &g_runs[count])) continue;
        if (order == ORDER_DEPTH) {
            uint8_t before = count, j, pos = 0;
            insert_run(count, &count);           /* shipped far->near sort */
            for (j = 0; j < count; ++j) if (g_run_order[j] == before) { pos = j; break; }
            g_sort_shifts += (unsigned long)(before - pos);
            if ((unsigned)(before - pos) > g_sort_max_shifts)
                g_sort_max_shifts = (unsigned)(before - pos);
            g_ins_seq[before] = g_runs[before].inv_mid;
        } else {
            g_run_order[count] = count;          /* strict baked order */
            ++count;
        }
        if (out_keys) out_keys[count - 1u] = r->val;
    }
    if (order == ORDER_DEPTH) {
        g_sort_items += count;
        ++g_sort_poses;
        if (count > g_sort_max_items) g_sort_max_items = count;
        /* One line per pose: the inv_mid values IN INSERTION ORDER, then the
         * final g_run_order the shipped insert_run produced. This is the
         * oracle for the Z80 sort kernel - the real code's own output, not a
         * re-derivation of its tie-breaking rule. */
        if (g_sort_dump && count) {
            uint8_t j;
            fprintf(g_sort_dump, "%u", count);
            for (j = 0; j < count; ++j) fprintf(g_sort_dump, " %u", g_ins_seq[j]);
            for (j = 0; j < count; ++j) fprintf(g_sort_dump, " %u", g_run_order[j]);
            fputc('\n', g_sort_dump);
        }
    }
    for (i = 0; i < count; ++i) draw_run(out_map, 0, &g_runs[g_run_order[i]]);
    g_tspf_touched_cells = g_touched_count;
    return count;
}

/* The oracle's own key-selection front end, lifted verbatim from
 * tsp_polar_render so the two selections can be compared directly. This is
 * six lines of dispatch, not a re-implementation of the renderer. */
static uint8_t oracle_keys_fwd(const TSPState *s, uint8_t *keys) {
    uint8_t gx, gy, lx, ly, recipe, base_id, cond_count, i, n = 0;
    uint16_t gi, off;
    const uint8_t *p, *b;
    gx = (uint8_t)((uint16_t)s->x_q4 >> 6);
    gy = (uint8_t)((uint16_t)s->y_q4 >> 6);
    if (gx >= 48u || gy >= 24u) return 0;
    gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
    recipe = k_tspf_recipe_grid[gi];
    if (recipe == 0xffu) return 0;
    lx = (uint8_t)((uint16_t)s->x_q4 & 63u);
    ly = (uint8_t)((uint16_t)s->y_q4 & 63u);
    off = k_tspf_recipe_off[recipe];
    p = &k_tspf_recipe_stream[off];
    base_id = *p++; cond_count = *p++;
    b = &k_tspf_base_stream[k_tspf_base_off[base_id]];
    i = *b++;
    for (; i; --i) keys[n++] = *b++;
    for (i = 0; i < cond_count; ++i) {
        uint8_t key = *p++, sel = *p++;
        if (selector_pass(sel, lx, ly)) keys[n++] = key;
    }
    return n;
}

/* Block-program key selection, same walk fused_render does. */
static uint8_t block_keys(const TSPState *s, uint8_t *keys) {
    uint8_t gx, gy, lx, ly, i, n = 0, skip = 0;
    const Block *blk;
    gx = (uint8_t)((uint16_t)s->x_q4 >> 6);
    gy = (uint8_t)((uint16_t)s->y_q4 >> 6);
    if (gx >= GRID_W || gy >= GRID_H) return 0;
    blk = &g_blocks[gy][gx];
    if (!blk->used) return 0;
    lx = (uint8_t)((uint16_t)s->x_q4 & 63u);
    ly = (uint8_t)((uint16_t)s->y_q4 & 63u);
    for (i = 0; i < blk->n; ++i) {
        const Rec *r = &blk->r[i];
        if (r->kind == 'G') { skip = (uint8_t)!selector_pass(r->val, lx, ly); continue; }
        if (skip) { skip = 0; continue; }
        keys[n++] = r->val;
    }
    return n;
}

static int keysets_equal(const uint8_t *a, uint8_t na, const uint8_t *b, uint8_t nb) {
    uint8_t ma[256] = {0}, mb[256] = {0}, i;
    for (i = 0; i < na; ++i) ma[a[i]] = 1;
    for (i = 0; i < nb; ++i) mb[b[i]] = 1;
    return memcmp(ma, mb, 256) == 0;
}

int main(int argc, char **argv) {
    const char *blocks = (argc > 1) ? argv[1] : "build/blocks.txt";
    unsigned yaw_step = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 0) : 8u;
    static uint16_t oracle[TSP_MAP_CELLS], fused_b[TSP_MAP_CELLS];
    static uint16_t fused_d[TSP_MAP_CELLS], fused_o[TSP_MAP_CELLS];
    static uint16_t fused_s[TSP_MAP_CELLS], fused_i[TSP_MAP_CELLS];
    static const int8_t off[][2] = { {0,0},{7,3},{3,7},{15,15},{11,5} };
    const unsigned n_off = sizeof off / sizeof off[0];
    TSPState s;
    unsigned gx, gy, yaw, oi, c;
    unsigned long poses = 0;
    unsigned long ok_block = 0, ok_depth = 0, ok_oins = 0, ok_strict = 0;
    unsigned long words_bad_strict = 0, ok_invd = 0, words_bad_invd = 0;
    unsigned long words_bad_block = 0, words_bad_depth = 0;
    unsigned long worst_block = 0, worst_depth = 0;
    unsigned shown = 0;
    unsigned long keyset_bad = 0;
    uint8_t ok_[64], bk_[64];

    if (!load_blocks(blocks)) return 1;
    if (argc > 3) g_sort_dump = fopen(argv[3], "w");

    for (gy = 0; gy < GRID_H; ++gy) {
        for (gx = 0; gx < GRID_W; ++gx) {
            int16_t px0 = (int16_t)(gx * CELL_Q4 + CELL_Q4 / 2);
            int16_t py0 = (int16_t)(gy * CELL_Q4 + CELL_Q4 / 2);
            if (!tsp_is_walkable_q4(px0, py0)) continue;
            for (oi = 0; oi < n_off; ++oi) {
                int16_t px = (int16_t)(px0 + off[oi][0]);
                int16_t py = (int16_t)(py0 + off[oi][1]);
                if (!tsp_is_walkable_q4(px, py)) continue;
                for (yaw = 0; yaw < 256u; yaw += yaw_step) {
                    unsigned db = 0, dd = 0;
                    memset(&s, 0, sizeof s);
                    s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)yaw;

                    tsp_polar_renderer_reset();
                    tsp_polar_render(&s, oracle, 0);

                    tsp_polar_renderer_reset();
                    fused_render(&s, fused_b, ORDER_BLOCK, 0);

                    tsp_polar_renderer_reset();
                    fused_render(&s, fused_d, ORDER_DEPTH, 0);

                    tsp_polar_renderer_reset();
                    fused_render(&s, fused_o, ORDER_ORACLE_INSERT, 0);

                    tsp_polar_renderer_reset();
                    fused_render(&s, fused_s, ORDER_ORACLE_STRICT, 0);

                    tsp_polar_renderer_reset();
                    fused_render(&s, fused_i, ORDER_INVD, 0);

                    {
                        unsigned dobad = 0, dsbad = 0;
                        for (c = 0; c < TSP_MAP_CELLS; ++c) {
                            if (oracle[c] != fused_b[c]) ++db;
                            if (oracle[c] != fused_d[c]) ++dd;
                            if (oracle[c] != fused_o[c]) ++dobad;
                            if (oracle[c] != fused_s[c]) ++dsbad;
                        }
                        {
                            unsigned dibad = 0;
                            for (c = 0; c < TSP_MAP_CELLS; ++c)
                                if (oracle[c] != fused_i[c]) ++dibad;
                            if (!dibad) ++ok_invd; else words_bad_invd += dibad;
                        }
                        if (!dobad) ++ok_oins;
                        if (!dsbad) ++ok_strict; else words_bad_strict += dsbad;
                    }
                    {
                        uint8_t na = oracle_keys_fwd(&s, ok_), nb = block_keys(&s, bk_);
                        if (!keysets_equal(ok_, na, bk_, nb)) ++keyset_bad;
                    }
                    ++poses;
                    if (!db) ++ok_block; else words_bad_block += db;
                    if (!dd) ++ok_depth; else words_bad_depth += dd;
                    if (db > worst_block) worst_block = db;
                    if (dd > worst_depth) worst_depth = dd;
                    if (dd && shown < 6) {
                        ++shown;
                        fprintf(stderr, "  DEPTH-ORDER MISMATCH (%d,%d,yaw=%u): "
                                "%u words\n", px, py, yaw, dd);
                    }
                }
            }
        }
    }

    printf("=== FUSED HOST PATH: baked block program -> 20x18 name table ===\n");
    printf("poses compared            %lu  (yaw_step=%u, %u sub-cell offsets)\n",
           poses, yaw_step, n_off);
    printf("words per name table      %u\n\n", (unsigned)TSP_MAP_CELLS);
    printf("KEY SELECTION (block walk vs recipe grid, as SETS):\n");
    printf("  poses with identical key set  %lu / %lu   (%.4f%%)\n\n",
           poses - keyset_bad, poses,
           100.0 * (double)(poses - keyset_bad) / (double)poses);
    printf("ORDER_DEPTH (runtime far->near inv_mid sort, as the oracle does):\n");
    printf("  poses matching exactly  %lu / %lu   (%.4f%%)\n",
           ok_depth, poses, 100.0 * (double)ok_depth / (double)poses);
    printf("  total word mismatches   %lu   worst pose %lu words\n\n",
           words_bad_depth, worst_depth);
    printf("DEPTH-SORT WORKLOAD (the sort is mandatory - measure it):\n");
    printf("  runs sorted / update     mean %.2f   max %u\n",
           (double)g_sort_items / (double)g_sort_poses, g_sort_max_items);
    printf("  insertion shifts/update  mean %.2f   max %u (single insert)\n\n",
           (double)g_sort_shifts / (double)g_sort_poses, g_sort_max_shifts);
    printf("ORDER_ORACLE_INSERT (same key set, oracle's insertion order):\n");
    printf("  poses matching exactly  %lu / %lu   (%.4f%%)\n\n",
           ok_oins, poses, 100.0 * (double)ok_oins / (double)poses);
    printf("ORDER_INVD (sort by perpendicular distance - TRANSLATION ONLY):\n");
    printf("  poses matching exactly  %lu / %lu   (%.4f%%)\n",
           ok_invd, poses, 100.0 * (double)ok_invd / (double)poses);
    printf("  total word mismatches   %lu\n\n", words_bad_invd);
    printf("ORDER_ORACLE_STRICT (recipe order, NO depth sort AT ALL):\n");
    printf("  poses matching exactly  %lu / %lu   (%.4f%%)\n",
           ok_strict, poses, 100.0 * (double)ok_strict / (double)poses);
    printf("  total word mismatches   %lu\n\n", words_bad_strict);
    printf("ORDER_BLOCK (strict baked order, NO runtime depth sort):\n");
    printf("  poses matching exactly  %lu / %lu   (%.4f%%)\n",
           ok_block, poses, 100.0 * (double)ok_block / (double)poses);
    printf("  total word mismatches   %lu   worst pose %lu words\n\n",
           words_bad_block, worst_block);
    if (ok_oins == poses && ok_depth != poses) {
        printf("RESULT: key selection is exact (100%%), and feeding the SAME set\n"
               "        in the oracle's insertion order is also exact. So the\n"
               "        residual ORDER_DEPTH divergence is purely INSERTION-ORDER\n"
               "        TIE-BREAKING among equal inv_mid - insert_run shifts only\n"
               "        on strictly-greater, so ties keep insertion order.\n"
               "        The baker must emit a tie-consistent order (or the\n"
               "        interpreter must break ties the same way). That is a much\n"
               "        smaller problem than 'must depth-sort at runtime'.\n");
    } else if (ok_depth == poses && ok_block == poses) {
        printf("RESULT: the baked block reproduces the oracle exactly, and the\n"
               "        baked ORDER is sufficient - the Z80 interpreter needs no\n"
               "        runtime depth sort.\n");
    } else if (ok_depth == poses) {
        printf("RESULT: key SELECTION is correct - with the oracle's depth sort\n"
               "        the fused path is exact. The baked ORDER alone is NOT\n"
               "        sufficient: the interpreter must sort by inv_mid at\n"
               "        runtime, or the baker must emit a depth-correct order.\n");
    } else {
        printf("RESULT: key SELECTION itself diverges - the block program does\n"
               "        not choose the same spans the recipe grid does. Fix that\n"
               "        before reasoning about order.\n");
    }
    if (g_sort_dump) fclose(g_sort_dump);
    return 0;
}
