/*
 * MICRO_BOUNDARY_A: the boundary-state census.
 *
 * A36 closed TEMP_BOUNDARY_A RED. The failure was not "dirty column -> redraw
 * column": the union already localises changes to cells. The failure was that
 * repairing one cell still ran column-oriented geometry machinery, and under
 * rotation the DDA_G materializer alone cost 154,047 T against a full render's
 * 153,450 T.
 *
 * So the question is now: can a dirty boundary cell's FINAL 8x8 result be
 * NAMED cheaply from retained state, instead of reconstructed?
 *
 * THE FIRST THING THE CENSUS HAS TO SETTLE
 * ----------------------------------------
 * The premise imagines building a new prebaked micro-boundary vocabulary. This
 * renderer already has one. `edge_entry` does not rasterize anything - it
 * SELECTS from a baked set:
 *
 *   TSP_TILE_EDGE(shade, off_index, slope_index)
 *     = 3 shades x 16 offsets x 8 slopes            = 384 tiles
 *   TSP_TILE_FULL(shade, cap, border)
 *     = 3 shades x 3 caps x 4 borders               =  36 tiles
 *   ceiling, floor, horizon                         =   3 tiles
 *                                                     ---
 *                                                     423 tiles
 *
 * and the selection is a pure function of four small inputs:
 *
 *   edge_entry(shade, local_left, slope, bottom) -> tile id | HFLIP | VFLIP
 *
 * So the vocabulary question is already answered by construction and the
 * census's job is different: measure how much of that vocabulary is actually
 * REACHED, how the observations distribute over it, and what the old->new
 * transitions look like at the pixel level - the quantity that decides whether
 * temporal deferral has any mass.
 *
 * PIXEL PATTERNS ARE RECONSTRUCTED EXACTLY from the shipped generator
 * (`main_tilesector_polar_gg.c`: emit_edge / emit_full / emit_solid /
 * emit_horizon), so pixel deltas are the real ones, not a model.
 *
 * usage: micro_boundary_probe [U] [frames] [yaw_step] [regime]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define MAXSPAN TSPF_MAX_ACTIVE
#define NTILE   TSP_GENERATED_TILE_COUNT

/* ---- exact pixel reconstruction, from the shipped tile generator ---- */
#define C_BLACK 0u
#define C_OUT   1u
#define C_FLOOR 2u
#define C_FAR   3u
#define C_MID   4u
#define C_NEAR  5u
static const int8_t k_edge_px[8][8] = {
    {0,0,0,0,0,0,0,0},{0,0,0,0,1,1,1,1},{0,0,1,1,1,1,2,2},{0,0,1,1,2,2,3,3},
    {0,1,1,2,2,3,3,4},{0,1,1,2,3,4,4,5},{0,1,2,3,3,4,5,6},{0,1,2,3,4,5,6,7}
};
static uint8_t g_px[NTILE][8][8];          /* palette index per pixel */

static uint8_t shade_col(uint8_t s) { return s == 0 ? C_FAR : (s == 1 ? C_MID : C_NEAR); }

static void build_patterns(void)
{
    uint8_t s, c, b, o, m, x, y;
    for (y = 0; y < 8; ++y) for (x = 0; x < 8; ++x) {
        g_px[TSP_TILE_CEILING][y][x] = C_OUT;
        g_px[TSP_TILE_FLOOR][y][x] = C_FLOOR;
        g_px[TSP_TILE_HORIZON][y][x] = (y == 0) ? C_BLACK : C_FLOOR;
    }
    for (s = 0; s < TSP_SHADE_COUNT; ++s)
      for (c = 0; c < TSP_CAP_COUNT; ++c)
        for (b = 0; b < TSP_BORDER_COUNT; ++b) {
            uint16_t id = TSP_TILE_FULL(s, c, b);
            for (y = 0; y < 8; ++y) for (x = 0; x < 8; ++x) {
                uint8_t blk = (uint8_t)((((b & 1u) && x == 0u)
                                      || ((b & 2u) && x == 7u)) ? 1u : 0u);
                if (c == TSP_CAP_TOP && y == 0u) blk = 1u;
                if (c == TSP_CAP_BOTTOM && y == 7u) blk = 1u;
                g_px[id][y][x] = blk ? C_BLACK : shade_col(s);
            }
        }
    for (s = 0; s < TSP_SHADE_COUNT; ++s)
      for (o = 0; o < TSP_EDGE_OFF_COUNT; ++o)
        for (m = 0; m < TSP_EDGE_SLOPE_COUNT; ++m) {
            uint16_t id = TSP_TILE_EDGE(s, o, m);
            int8_t off = (int8_t)(TSP_EDGE_OFF_MIN + (int8_t)o);
            for (y = 0; y < 8; ++y) for (x = 0; x < 8; ++x) {
                int8_t line = (int8_t)(off + k_edge_px[m][x]);
                g_px[id][y][x] = (int8_t)y < line ? C_OUT
                               : ((int8_t)y == line ? C_BLACK : shade_col(s));
            }
        }
}

/* A name-table word -> its 8x8 pixels, honouring the flip bits the renderer
 * actually sets. PALETTE selects a bank, not a pattern, so it does not change
 * the index grid. */
static void word_pixels(uint16_t w, uint8_t out[8][8])
{
    uint16_t id = (uint16_t)(w & TSP_TILE_ID_MASK);
    uint8_t x, y;
    if (id >= NTILE) { memset(out, 0, 64); return; }
    for (y = 0; y < 8; ++y) for (x = 0; x < 8; ++x) {
        uint8_t sy = (w & TSP_ATTR_FLIPY) ? (uint8_t)(7 - y) : y;
        uint8_t sx = (w & TSP_ATTR_FLIPX) ? (uint8_t)(7 - x) : x;
        out[y][x] = g_px[id][sy][sx];
    }
}

static int pixel_delta(uint16_t a, uint16_t b)
{
    uint8_t pa[8][8], pb[8][8];
    int x, y, n = 0;
    if (a == b) return 0;
    word_pixels(a, pa);
    word_pixels(b, pb);
    for (y = 0; y < 8; ++y) for (x = 0; x < 8; ++x)
        if (pa[y][x] != pb[y][x]) ++n;
    return n;
}

/* ---- census accumulators ---- */
static unsigned long w_count[1u << 13];        /* observations per name word */
static unsigned long n_obs, n_cells, n_frames;
static unsigned long n_edge_obs, n_full_obs, n_bg_obs;
static unsigned char seen_id[NTILE];
/* canonicalization: a word's pixel grid, hashed, under each flip group */
static unsigned long uniq_raw, uniq_h, uniq_v, uniq_hv;
/* transitions */
static unsigned long t_count, t_hist[65], t_pixels_total;
static unsigned long t_by_class[8];
static unsigned long t_same_id_diff_attr, t_diff_id;
static unsigned long t_edge_to_edge, t_edge_to_full, t_full_to_edge, t_other;
/* the runtime key: is the word a pure function of edge_entry's four inputs? */
static unsigned long key_obs, key_conflict;
static uint16_t key_word[2][2][16][8];         /* [bottom][shade>0][off][mag] */
static unsigned char key_set[2][2][16][8];

static uint16_t g_prev_map[TSP_MAP_CELLS];
static int g_have_prev;

typedef struct { uint16_t map[TSP_MAP_CELLS]; } Frame;
static Frame g_fa, g_fb;

static void census_frame(const uint16_t *map)
{
    unsigned i;
    ++n_frames;
    for (i = 0; i < TSP_MAP_CELLS; ++i) {
        uint16_t w = map[i];
        uint16_t id = (uint16_t)(w & TSP_TILE_ID_MASK);
        ++n_obs;
        if (w < (1u << 13)) ++w_count[w];
        if (id < NTILE) seen_id[id] = 1;
        if (id >= TSP_TILE_EDGE_BASE) ++n_edge_obs;
        else if (id >= TSP_TILE_FULL_BASE) ++n_full_obs;
        else ++n_bg_obs;
    }
    if (g_have_prev) {
        for (i = 0; i < TSP_MAP_CELLS; ++i) {
            uint16_t a = g_prev_map[i], b = map[i];
            int d;
            if (a == b) continue;
            ++t_count;
            d = pixel_delta(a, b);
            t_pixels_total += (unsigned long)d;
            ++t_hist[d > 64 ? 64 : d];
            if ((a & TSP_TILE_ID_MASK) == (b & TSP_TILE_ID_MASK))
                ++t_same_id_diff_attr;
            else ++t_diff_id;
            {
                uint16_t ia = a & TSP_TILE_ID_MASK, ib = b & TSP_TILE_ID_MASK;
                int ea = ia >= TSP_TILE_EDGE_BASE, eb = ib >= TSP_TILE_EDGE_BASE;
                if (ea && eb) ++t_edge_to_edge;
                else if (ea && !eb) ++t_edge_to_full;
                else if (!ea && eb) ++t_full_to_edge;
                else ++t_other;
            }
            /* deferability classes, derived from the measured pixel delta */
            if (d == 0) ++t_by_class[0];
            else if (d <= 2) ++t_by_class[1];
            else if (d <= 4) ++t_by_class[2];
            else if (d <= 8) ++t_by_class[3];
            else if (d <= 16) ++t_by_class[4];
            else ++t_by_class[5];
        }
    }
    memcpy(g_prev_map, map, sizeof g_prev_map);
    g_have_prev = 1;
}

/* How big is the lookup that replaces the derivation?
 *
 * edge_entry IS the map from local boundary state to final word. The first
 * version of this check indexed by the tile fields it DERIVES, which of course
 * collided; the question is the size of its input domain and output range.
 * Enumerated, not reasoned about. */
static unsigned long lut_domain, lut_distinct;
static unsigned long lut_domain_m0, lut_distinct_m0;

static void verify_key(void)
{
    int ll, sl, bot, sh;
    static unsigned char seen[1u << 13], seen0[1u << 13];
    for (bot = 0; bot < 2; ++bot)
      for (sh = 0; sh < (int)TSP_SHADE_COUNT; ++sh)
        for (ll = -16; ll <= 16; ++ll)
          for (sl = -7; sl <= 7; ++sl) {
              uint16_t w = edge_entry((uint8_t)sh, (int16_t)ll, (int8_t)sl,
                                      (uint8_t)bot);
              ++lut_domain;
              if (w < (1u << 13) && !seen[w]) { seen[w] = 1; ++lut_distinct; }
              if (sh == 1) {
                  ++lut_domain_m0;
                  if (w < (1u << 13) && !seen0[w]) { seen0[w] = 1; ++lut_distinct_m0; }
              }
          }
}

/* The domain the corpus actually visits, recorded at the real call sites. */
static unsigned long obs_key;
static unsigned char obs_seen[2][33][15];
static unsigned long obs_pairs;
static int obs_ll_min = 127, obs_ll_max = -128;
static int obs_sl_min = 127, obs_sl_max = -128;

static void note_key(int local_left, int slope, int bottom)
{
    int li = local_left + 16, si = slope + 7;
    ++obs_key;
    if (local_left < obs_ll_min) obs_ll_min = local_left;
    if (local_left > obs_ll_max) obs_ll_max = local_left;
    if (slope < obs_sl_min) obs_sl_min = slope;
    if (slope > obs_sl_max) obs_sl_max = slope;
    if (li < 0 || li > 32 || si < 0 || si > 14) return;
    if (!obs_seen[bottom][li][si]) { obs_seen[bottom][li][si] = 1; ++obs_pairs; }
}

/* Replay one column exactly as draw_run does, recording every edge_entry call
 * site so the observed domain is the real one. */
static void replay_col_keys(int16_t yl, int16_t yr, uint8_t bottom)
{
    int8_t slope = clamp_s8((int16_t)(yr - yl), -7, 7);
    int8_t r0 = row_floor(yl < yr ? yl : yr), r1 = row_floor(yl > yr ? yl : yr), r;
    if (r0 < 0) r0 = 0;
    if (r1 >= (int8_t)TSP_ROWS) r1 = (int8_t)(TSP_ROWS - 1u);
    for (r = r0; r <= r1; ++r)
        note_key((int)(yl - ((int16_t)r << 3)), slope, bottom);
}

/* Flip-canonical pattern counting over the words actually observed. */
static void count_canonical(void)
{
    static uint8_t pat[1u << 13][8][8];
    static unsigned char have[1u << 13];
    unsigned w, i, j;
    unsigned nobs = 0;
    static unsigned obs[1u << 13];
    for (w = 0; w < (1u << 13); ++w)
        if (w_count[w]) { obs[nobs++] = w; word_pixels((uint16_t)w, pat[w]); have[w] = 1; }
    for (i = 0; i < nobs; ++i) {
        unsigned a = obs[i];
        int dup_raw = 0, dup_h = 0, dup_v = 0, dup_hv = 0;
        for (j = 0; j < i; ++j) {
            unsigned b = obs[j];
            int x, y, eq = 1, eh = 1, ev = 1, ehv = 1;
            for (y = 0; y < 8 && (eq || eh || ev || ehv); ++y)
              for (x = 0; x < 8; ++x) {
                  uint8_t p = pat[a][y][x];
                  if (p != pat[b][y][x]) eq = 0;
                  if (p != pat[b][y][7 - x]) eh = 0;
                  if (p != pat[b][7 - y][x]) ev = 0;
                  if (p != pat[b][7 - y][7 - x]) ehv = 0;
              }
            if (eq) dup_raw = 1;
            if (eq || eh) dup_h = 1;
            if (eq || ev) dup_v = 1;
            if (eq || eh || ev || ehv) dup_hv = 1;
        }
        if (!dup_raw) ++uniq_raw;
        if (!dup_h) ++uniq_h;
        if (!dup_v) ++uniq_v;
        if (!dup_hv) ++uniq_hv;
    }
    (void)have;
}

/* ---- corpus driver, identical to A29/A30/A34 ---- */
typedef struct { const char *name; uint8_t manual; uint8_t inp[8]; } Regime;
#define I_U TSP_INPUT_UP
#define I_L TSP_INPUT_LEFT
#define I_R TSP_INPUT_RIGHT
#define I_S TSP_INPUT_STRAFE_LEFT
static const Regime k_regime[] = {
    { "stand still",      1u, {0,0,0,0,0,0,0,0} },
    { "walk forward",     1u, {I_U,I_U,I_U,I_U,I_U,I_U,I_U,I_U} },
    { "turn in place",    1u, {I_L,I_L,I_L,I_L,I_L,I_L,I_L,I_L} },
    { "walk + turn",      1u, {I_U|I_L,I_U|I_L,I_U|I_L,I_U|I_L,
                               I_U|I_L,I_U|I_L,I_U|I_L,I_U|I_L} },
    { "strafe",           1u, {I_S,I_S,I_S,I_S,I_S,I_S,I_S,I_S} },
    { "walk, nudge turn", 1u, {I_U,I_U,I_U,I_U|I_L,I_U,I_U,I_U,I_U|I_R} },
    { "walk, rare nudge", 1u, {I_U,I_U,I_U,I_U,I_U,I_U,I_U,I_U|I_L} },
    { "demo path",        0u, {0,0,0,0,0,0,0,0} },
};
#define NREG (sizeof k_regime / sizeof k_regime[0])

static void render_pose(const TSPState *st, uint16_t *out)
{
    uint8_t ks[64], nk = 0, count = 0, j;
    uint8_t recipe, base_id, cond_count, lx, ly, gx, gy;
    uint16_t gi, offs, i;
    const uint8_t *pp, *b;
    map_init(out);
    gx = (uint8_t)((uint16_t)st->x_q4 >> 6);
    gy = (uint8_t)((uint16_t)st->y_q4 >> 6);
    if (gx >= 48u || gy >= 24u) return;
    gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
    recipe = k_tspf_recipe_grid[gi];
    if (recipe == 0xffu) return;
    lx = (uint8_t)((uint16_t)st->x_q4 & 63u);
    ly = (uint8_t)((uint16_t)st->y_q4 & 63u);
    offs = k_tspf_recipe_off[recipe];
    pp = &k_tspf_recipe_stream[offs];
    base_id = *pp++; cond_count = *pp++;
    b = &k_tspf_base_stream[k_tspf_base_off[base_id]];
    i = *b++;
    for (; i; --i) { ks[nk] = *b++; ++nk; }
    for (i = 0; i < cond_count; ++i) {
        uint8_t key = *pp++, sel = *pp++;
        if (selector_pass(sel, lx, ly)) { ks[nk] = key; ++nk; }
    }
    tsp_polar_renderer_reset();
    g_corner_bearing_valid = 0u;
    for (j = 0; j < nk; ++j) {
        if (count >= MAXSPAN) break;
        if (!project_key(ks[j], st, &g_runs[count])) continue;
        insert_run(count, &count);
    }
    for (i = 0; i < count; ++i) {
        const PolarRun *r = &g_runs[g_run_order[i]];
        uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3), n, c;
        uint8_t profile = k_tspf_profile[r->sid];
        int16_t iq, step;
        draw_run(out, NULL, r);
        if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
        if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
        if (c1 < c0) continue;
        n = (uint8_t)(c1 - c0 + 1u);
        iq = (int16_t)((int16_t)r->inv0 << 6);
        step = (int16_t)(((int16_t)r->inv1 - (int16_t)r->inv0)
                         * (int16_t)k_col_recip_q8[n]);
        step = shr_signed(step, 2);
        for (c = c0; c <= c1; ++c) {
            uint8_t il = (uint8_t)clamp_u8i((int16_t)((iq + 32) >> 6), 255u);
            uint8_t ir = (uint8_t)clamp_u8i((int16_t)((iq + step + 32) >> 6), 255u);
            uint8_t hl = (uint8_t)(il >> 1), hr = (uint8_t)(ir >> 1);
            int16_t tl = (int16_t)(TSPF_HORIZON - hl), tr = (int16_t)(TSPF_HORIZON - hr);
            int16_t bl = (int16_t)(TSPF_HORIZON + hl), br = (int16_t)(TSPF_HORIZON + hr);
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
            replay_col_keys(tl, tr, 0u);
            replay_col_keys(bl, br, 1u);
            iq = (int16_t)(iq + step);
        }
    }
}

int main(int argc, char **argv)
{
    unsigned U = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 1u;
    unsigned frames = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 0) : 120u;
    unsigned yaw_step = (argc > 3) ? (unsigned)strtoul(argv[3], 0, 0) : 64u;
    int only = (argc > 4) ? (int)strtol(argv[4], 0, 0) : -1;
    unsigned t, f, gx, gy, yy, i;
    unsigned long ids = 0;

    if (!U) U = 1;
    if (!yaw_step) yaw_step = 64u;
    g_tspf_appearance_mode = 0u;
    build_patterns();
    verify_key();

    for (t = 0; t < NREG; ++t) {
        if (only >= 0 && (int)t != only) continue;
        for (gy = 0; gy < 24u; ++gy) for (gx = 0; gx < 48u; ++gx) {
            int16_t px = (int16_t)(gx * 64 + 32), py = (int16_t)(gy * 64 + 32);
            uint16_t gi;
            if (!tsp_is_walkable_q4(px, py)) continue;
            gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
            if (k_tspf_recipe_grid[gi] == 0xffu) continue;
            for (yy = 0; yy < 256u; yy += yaw_step) {
                TSPState st;
                tsp_reset(&st);
                st.x_q4 = px; st.y_q4 = py;
                st.yaw = (uint8_t)yy; st.manual = k_regime[t].manual;
                g_have_prev = 0;
                for (f = 0; f < frames; ++f) {
                    tsp_step(&st, k_regime[t].inp[(f / 30u) % 8u]);
                    if ((f % U) != U - 1) continue;
                    render_pose(&st, g_fa.map);
                    census_frame(g_fa.map);
                }
            }
        }
    }
    count_canonical();
    for (i = 0; i < NTILE; ++i) if (seen_id[i]) ++ids;

    printf("=== MICRO_BOUNDARY_A: the boundary-state census ===\n");
    printf("U=%u  regime %s  frames %lu  cell observations %lu\n\n",
           U, only >= 0 ? k_regime[only].name : "ALL", n_frames, n_obs);

    printf("THE VOCABULARY ALREADY EXISTS, AND IT IS BAKED\n");
    printf("  edge tiles   3 shades x %u offsets x %u slopes = %u\n",
           TSP_EDGE_OFF_COUNT, TSP_EDGE_SLOPE_COUNT,
           TSP_SHADE_COUNT * TSP_EDGE_OFF_COUNT * TSP_EDGE_SLOPE_COUNT);
    printf("  FULL tiles   3 shades x %u caps x %u borders     = %u\n",
           TSP_CAP_COUNT, TSP_BORDER_COUNT,
           TSP_SHADE_COUNT * TSP_CAP_COUNT * TSP_BORDER_COUNT);
    printf("  background   ceiling, floor, horizon            = 3\n");
    printf("  TOTAL BAKED VOCABULARY                          = %u tiles\n",
           NTILE);
    printf("  at 32 bytes of 4bpp pattern each                = %u bytes VRAM\n",
           NTILE * 32u);
    printf("  tile IDs actually REACHED by the corpus         = %lu (%.1f%%)\n",
           ids, 100.0 * ids / NTILE);
    printf("  distinct name-table WORDS observed              = %lu\n", uniq_raw);

    printf("\nFLIP CANONICALIZATION of the observed patterns\n");
    printf("  raw distinct 8x8 index grids     %lu\n", uniq_raw);
    printf("  after HFLIP                      %lu\n", uniq_h);
    printf("  after VFLIP                      %lu\n", uniq_v);
    printf("  after both                       %lu\n", uniq_hv);

    printf("\nOBSERVATION MIX\n");
    printf("  edge tiles       %6.2f%%\n", 100.0 * n_edge_obs / n_obs);
    printf("  FULL interior    %6.2f%%\n", 100.0 * n_full_obs / n_obs);
    printf("  background       %6.2f%%\n", 100.0 * n_bg_obs / n_obs);

    printf("\nTHE LOOKUP THAT WOULD REPLACE THE DERIVATION\n");
    printf("  edge_entry(shade, local_left, slope, bottom) -> word\n");
    printf("  full enumerated domain   %lu entries -> %lu distinct words\n",
           lut_domain, lut_distinct);
    printf("  appearance mode 0 only   %lu entries -> %lu distinct words"
           "   = %lu bytes at 2 bytes/entry\n",
           lut_domain_m0, lut_distinct_m0, lut_domain_m0 * 2);
    printf("  domain the corpus ACTUALLY visits:\n");
    printf("    local_left %d..%d   slope %d..%d   distinct (ll,slope,bottom)"
           " %lu   calls %lu\n",
           obs_ll_min, obs_ll_max, obs_sl_min, obs_sl_max, obs_pairs, obs_key);
    printf("    a table over the visited domain is %lu bytes\n", obs_pairs * 2);

    printf("\nOLD->NEW TRANSITIONS, BY PIXEL DELTA\n");
    printf("  changed cells %lu over %lu frames = %.2f/update\n",
           t_count, n_frames, (double)t_count / n_frames);
    printf("  mean wrong pixels if a change is left undrawn: %.2f of 64\n",
           t_count ? (double)t_pixels_total / t_count : 0.0);
    {
        static const char *cn[6] = { "0 pixels", "1-2", "3-4", "5-8",
                                     "9-16", "17+" };
        unsigned long k;
        for (k = 0; k < 6; ++k)
            printf("    %-9s %10lu   %5.1f%% of changes\n", cn[k], t_by_class[k],
                   t_count ? 100.0 * t_by_class[k] / t_count : 0.0);
    }
    printf("  transition shape: same tile id, different flips %5.1f%%"
           "   different id %5.1f%%\n",
           t_count ? 100.0 * t_same_id_diff_attr / t_count : 0.0,
           t_count ? 100.0 * t_diff_id / t_count : 0.0);
    printf("  edge->edge %5.1f%%   edge->FULL %5.1f%%   FULL->edge %5.1f%%"
           "   other %5.1f%%\n",
           t_count ? 100.0 * t_edge_to_edge / t_count : 0.0,
           t_count ? 100.0 * t_edge_to_full / t_count : 0.0,
           t_count ? 100.0 * t_full_to_edge / t_count : 0.0,
           t_count ? 100.0 * t_other / t_count : 0.0);
    return 0;
}
