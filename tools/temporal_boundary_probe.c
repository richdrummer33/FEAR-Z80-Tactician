/*
 * Temporal BOUNDARY events - the finer-grained question A28 did not ask.
 *
 * A28 measured whether a whole (run, column) work key survived an update and
 * found 0.1% under rotation. That is the right answer to the wrong question.
 * It also found 70.8% of final cells unchanged under the same rotation, and
 * 59% of row-writes storing a word that was already there. The gap between
 * those two numbers is what this probe is for.
 *
 * THE HYPOTHESIS
 * --------------
 * Under yaw the projected geometry changes almost everywhere - il, ir, the
 * endpoints, the slope, the screen X all move. For a flat-colour wall, almost
 * none of that reaches the screen. A wall column is three things: a top edge
 * (a few cells), a bottom edge (a few cells), and an interior of identical
 * FULL tiles. Move the geometry by a pixel and the interior tiles are
 * bit-identical; only the boundary cells can differ. So cost should scale with
 * BOUNDARY LENGTH, not with polygon AREA.
 *
 * WHAT IS RETAINED, AND WHY THAT EXACT SET
 * ----------------------------------------
 * The renderer's per-column output is a pure function of six values -
 * (tl, tr, bl, br, shade, border) - through draw_edge/draw_edge/draw_full in
 * that order, interior last so it wins on overlap. So that sextuple IS the
 * retained boundary state: nothing else about the projection can reach a cell.
 * From it, a cell's contribution is:
 *
 *     row in interior [row_floor(max tl,tr)+1 .. row_floor(min bl,br)-1]
 *        -> TSP_TILE_FULL(shade, CAP_NONE, border)
 *     else row in bottom edge -> edge_entry(shade, bl-8r, slope_b, 1)
 *     else row in top edge    -> edge_entry(shade, tl-8r, slope_t, 0)
 *     else                    -> no contribution
 *
 * THE CLASSIFIER IS CHEAP BY CONSTRUCTION
 * ---------------------------------------
 * It reads ONLY retained state and new state. It never renders a cell and
 * compares - that would only prove stores are skippable, which A28 already
 * showed and which is not the point. It emits a DIRTY SET from boundary
 * events alone:
 *
 *   - column present in one pose only     -> V_COLUMN_SHIFT, all its rows
 *   - shade or border changed             -> whole column, no boundary shortcut
 *   - top (tl,tr) changed                 -> union of old and new top edge rows
 *   - bottom (bl,br) changed              -> union of old and new bottom rows
 *   - interior range changed              -> SYMMETRIC DIFFERENCE only
 *   - relative draw order of two spans flipped -> both spans, all rows
 *
 * The interior line is the whole hypothesis: rows that were interior and still
 * are, at the same shade and border, are NOT dirty however far the geometry
 * moved.
 *
 * THE GO/NO-GO
 * ------------
 * Every cell OUTSIDE the dirty set must already hold the correct new word.
 * The probe checks that against a fresh render of the new pose, on every pose
 * pair, and reports the first divergence with the state that produced it. A
 * dirty set that is not a superset of the true change set is a wrong
 * representation, not a fast one.
 *
 * usage: temporal_boundary_probe [frames_per_update] [frames] [yaw_step] [regime]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define MAXSPAN TSPF_MAX_ACTIVE

typedef struct {
    uint8_t present;
    int16_t tl, tr, bl, br;
    uint8_t shade, border;
} ColState;

typedef struct {
    uint8_t keyid;                 /* persistent span identity across poses */
    uint8_t rank;                  /* far->near draw index */
    uint8_t x0, x1;                /* screen PIXEL extent, for sub-cell motion */
    uint8_t c0, c1;
    int16_t iq, step;              /* run params, for the sequence oracle */
    uint8_t profile, left_real, right_real, shade_run;
    ColState col[TSP_COLS];
} SpanState;

typedef struct {
    uint16_t map[TSP_MAP_CELLS];
    SpanState sp[MAXSPAN];
    uint8_t nsp;
    int16_t x_q4, y_q4;
    uint8_t yaw;
} Pose;

static Pose g_p[2];

/* Pose-SEQUENCE oracle for the Z80 A/B. The masked/DDA benches verify one pose
 * against `coverage_pose_oracle.txt`; a temporal kernel carries state ACROSS
 * poses, so the unit of verification has to be a consecutive sequence. Same
 * 8-field run format as that oracle so the baseline kernel needs no changes,
 * plus a 9th field - the key id - because a temporal kernel has to match a
 * span to its retained state and run order alone does not identify it.
 *
 *   line := <new_trajectory 0|1> <nruns> [iq stp c0 c1 prof lr rr sh keyid]*n
 *           <360 final words>
 *
 * Runs are dumped near->far, matching coverage_pose_oracle.txt, so
 * `run_pose(..., near_first=False)` reverses them into the far->near draw
 * order the shipped path uses. */
static FILE *g_seq = NULL;
static unsigned long g_seq_stride = 1, g_seq_seen = 0, g_seq_emitted = 0;
static unsigned long g_seq_traj = 0;
static int g_seq_run = 0;

static void dump_pose(const Pose *p, int new_traj)
{
    unsigned i, k;
    if (!g_seq) return;

    fprintf(g_seq, "%d %u", new_traj, p->nsp);
    for (i = p->nsp; i-- > 0; ) {          /* near->far */
        const SpanState *s = &p->sp[i];
        int n = (int)s->c1 - (int)s->c0 + 1;
        int iq = 0, stp = 0;
        /* Re-derive iq/step exactly as draw_run does, from the retained
         * per-column state's own inverse-depth endpoints is NOT possible -
         * they are quantized. Carry the run's own values instead. */
        iq = s->iq; stp = s->step;
        fprintf(g_seq, " %d %d %u %u %u %u %u %u %u",
                iq, stp, s->c0, s->c1, s->profile,
                s->left_real, s->right_real, s->shade_run, s->keyid);
        (void)n;
    }
    for (k = 0; k < TSP_MAP_CELLS; ++k) fprintf(g_seq, " %u", p->map[k]);
    fputc('\n', g_seq);
    ++g_seq_emitted;
}

/* ---- exact re-derivations of the renderer's own row ranges ---- */

static void top_rows(const ColState *s, int *r0, int *r1)
{
    int a = row_floor(s->tl < s->tr ? s->tl : s->tr);
    int b = row_floor(s->tl > s->tr ? s->tl : s->tr);
    if (a < 0) a = 0;
    if (b >= (int)TSP_ROWS) b = (int)TSP_ROWS - 1;
    *r0 = a; *r1 = b;
}
static void bot_rows(const ColState *s, int *r0, int *r1)
{
    int a = row_floor(s->bl < s->br ? s->bl : s->br);
    int b = row_floor(s->bl > s->br ? s->bl : s->br);
    if (a < 0) a = 0;
    if (b >= (int)TSP_ROWS) b = (int)TSP_ROWS - 1;
    *r0 = a; *r1 = b;
}
static void int_rows(const ColState *s, int *r0, int *r1)
{
    int a = row_floor(s->tl > s->tr ? s->tl : s->tr) + 1;
    int b = row_floor(s->bl < s->br ? s->bl : s->br) - 1;
    if (a < 0) a = 0;
    if (b >= (int)TSP_ROWS) b = (int)TSP_ROWS - 1;
    *r0 = a; *r1 = b;
}

/* This span's contribution to one cell, or 0 if it writes nothing there.
 * Mirrors draw_run's within-column order: top, then bottom, then interior. */
static int contrib(const ColState *s, int row, uint16_t *w)
{
    int a, b;
    if (!s->present) return 0;
    int_rows(s, &a, &b);
    if (a <= b && row >= a && row <= b) {
        *w = TSP_TILE_FULL(s->shade, TSP_CAP_NONE, s->border);
        return 1;
    }
    bot_rows(s, &a, &b);
    if (row >= a && row <= b) {
        *w = edge_entry(s->shade, (int16_t)(s->bl - (row << 3)),
                        clamp_s8((int16_t)(s->br - s->bl), -7, 7), 1u);
        return 1;
    }
    top_rows(s, &a, &b);
    if (row >= a && row <= b) {
        *w = edge_entry(s->shade, (int16_t)(s->tl - (row << 3)),
                        clamp_s8((int16_t)(s->tr - s->tl), -7, 7), 0u);
        return 1;
    }
    return 0;
}

static int col_same(const ColState *a, const ColState *b)
{
    if (a->present != b->present) return 0;
    if (!a->present) return 1;
    return a->tl == b->tl && a->tr == b->tr && a->bl == b->bl
        && a->br == b->br && a->shade == b->shade && a->border == b->border;
}

/* ---- pose construction: same run list and draw order as tsp_polar_render ---- */

static void build_pose(const TSPState *st, Pose *p)
{
    uint8_t ks[64], nk = 0, count = 0, j;
    uint8_t kid[64];
    uint8_t recipe, base_id, cond_count, lx, ly, gx, gy;
    uint16_t gi, offs, i;
    const uint8_t *pp, *b;
    uint8_t run_key[MAXSPAN];

    p->nsp = 0;
    p->x_q4 = st->x_q4; p->y_q4 = st->y_q4; p->yaw = st->yaw;
    map_init(p->map);

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
    for (; i; --i) { ks[nk] = *b++; kid[nk] = ks[nk]; ++nk; }
    for (i = 0; i < cond_count; ++i) {
        uint8_t key = *pp++, sel = *pp++;
        if (selector_pass(sel, lx, ly)) { ks[nk] = key; kid[nk] = key; ++nk; }
    }

    tsp_polar_renderer_reset();
    g_corner_bearing_valid = 0u;
    for (j = 0; j < nk; ++j) {
        if (count >= MAXSPAN) break;
        if (!project_key(ks[j], st, &g_runs[count])) continue;
        run_key[count] = kid[j];
        insert_run(count, &count);
    }

    for (i = 0; i < count; ++i) {
        uint8_t ri = g_run_order[i];
        const PolarRun *r = &g_runs[ri];
        SpanState *s = &p->sp[p->nsp];
        uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3), n, c;
        uint8_t profile = k_tspf_profile[r->sid];
        int16_t iq, step, jq;

        draw_run(p->map, NULL, r);

        if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
        if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
        if (c1 < c0) continue;
        memset(s, 0, sizeof *s);
        s->keyid = run_key[ri];
        s->rank = (uint8_t)i;
        s->x0 = r->x0; s->x1 = r->x1;
        s->c0 = c0; s->c1 = c1;
        s->profile = profile;
        s->left_real = r->left_real; s->right_real = r->right_real;
        s->shade_run = 1u;                 /* appearance mode 0 */
        n = (uint8_t)(c1 - c0 + 1u);
        iq = (int16_t)((int16_t)r->inv0 << 6);
        step = (int16_t)(((int16_t)r->inv1 - (int16_t)r->inv0)
                         * (int16_t)k_col_recip_q8[n]);
        step = shr_signed(step, 2);
        s->iq = iq; s->step = step;
        jq = iq;
        for (c = c0; c <= c1; ++c) {
            uint8_t il = (uint8_t)clamp_u8i((int16_t)((jq + 32) >> 6), 255u);
            uint8_t ir = (uint8_t)clamp_u8i((int16_t)((jq + step + 32) >> 6), 255u);
            uint8_t hl = (uint8_t)(il >> 1), hr = (uint8_t)(ir >> 1);
            ColState *cs = &s->col[c];
            cs->present = 1u;
            cs->tl = (int16_t)(TSPF_HORIZON - hl);
            cs->tr = (int16_t)(TSPF_HORIZON - hr);
            cs->bl = (int16_t)(TSPF_HORIZON + hl);
            cs->br = (int16_t)(TSPF_HORIZON + hr);
            if (profile == TSP_PROFILE_FULL) { cs->tl--; cs->tr--; }
            if (profile == TSP_PROFILE_LINTEL) {
                cs->bl = (int16_t)(TSPF_HORIZON - (hl >> 1));
                cs->br = (int16_t)(TSPF_HORIZON - (hr >> 1));
            } else if (profile == TSP_PROFILE_RAISED) {
                cs->bl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
                cs->br = (int16_t)(TSPF_HORIZON + hr - (hr >> 2));
            } else if (profile == TSP_PROFILE_RISER) {
                cs->tl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
                cs->tr = (int16_t)(TSPF_HORIZON + hr - (hr >> 2));
            }
            cs->shade = g_tspf_appearance_mode
                      ? shade_for((uint8_t)(((uint16_t)il + ir) >> 1),
                                  k_tspf_shade_bias[r->sid]) : 1u;
            cs->border = (uint8_t)(((c == c0 && r->left_real) ? 1u : 0u)
                                 | ((c == c1 && r->right_real) ? 2u : 0u));
            jq = (int16_t)(jq + step);
        }
        ++p->nsp;
    }
}

/* ---- classifier: dirty set from retained boundary state alone ---- */

enum { OP_NO_CHANGE = 0, OP_PHASE_SHIFT, OP_PHASE_RAMP, OP_EDGE_CROSS,
       OP_SLOPE_CHANGE, OP_V_GROW, OP_V_SHRINK, OP_V_COLUMN_SHIFT,
       OP_FALLBACK, OP_COUNT };
static const char *k_op_name[OP_COUNT] = {
    "NO_CHANGE", "PHASE_SHIFT", "PHASE_RAMP", "EDGE_CROSS",
    "SLOPE_CHANGE", "V_GROW(sole)", "V_SHRINK(sole)", "V_COLUMN_SHIFT",
    "FALLBACK"
};

/* Cause tags on a dirty cell, for the per-cause breakdown. */
enum { CZ_TOP = 0, CZ_BOT, CZ_INTERIOR, CZ_COLSHIFT, CZ_SHADE, CZ_TOPO,
       CZ_COUNT };
static const char *k_cz_name[CZ_COUNT] = {
    "top/sloped edge", "bottom/sloped edge", "interior enter/leave",
    "vertical column shift", "shade/border", "visibility/ownership"
};

static uint8_t g_dirty[TSP_ROWS][TSP_COLS];
static uint8_t g_cause[TSP_ROWS][TSP_COLS];

static void mark(int r0, int r1, int c, int cause)
{
    int r;
    if (r0 < 0) r0 = 0;
    if (r1 >= (int)TSP_ROWS) r1 = (int)TSP_ROWS - 1;
    for (r = r0; r <= r1; ++r) {
        if (!g_dirty[r][c]) { g_dirty[r][c] = 1u; g_cause[r][c] = (uint8_t)cause; }
    }
}

/* ---- accumulators ---- */
static unsigned long n_pairs;
static unsigned long n_cells_same, n_cells_true_changed, n_dirty;
static unsigned long n_cause[CZ_COUNT];
static unsigned long n_op[OP_COUNT];
static unsigned long n_spans, n_span_ops, n_rowwrites_full;
static unsigned long n_phase_only;
static unsigned long verify_fail, verify_cells_missed;
static unsigned long n_vacated;
static unsigned long n_rank_flip, n_span_appear, n_span_vanish;
/* Column-materializations, the unit A24/A27 priced the kernel in. A dirty-cell
 * count alone misleads: A27's profile puts the interior fill at ~8%, so a
 * newly swept column is not as expensive as its cell count suggests, while a
 * column needing full derivation is. */
static unsigned long n_colmat, n_col_skip, n_col_edge_only, n_col_full;
static unsigned long n_interior_preserved, n_interior_swapped;
static unsigned long n_grow, n_shrink;

/* Bucketed by |dyaw|, the physically meaningful variable. The cadence U only
 * matters through the pose delta it produces, so report both. */
#define NBK 6
static unsigned bk;
static const char *k_bk_name[NBK] = { "0", "1-2", "3-4", "5-8", "9-16", ">16" };
static unsigned long b_pairs[NBK], b_rowwr[NBK], b_dirty[NBK], b_true[NBK];
static unsigned long b_colmat[NBK], b_skip[NBK], b_edge[NBK], b_full[NBK];

/* How far does a span slide per update, in PIXELS and in COLUMNS? The question
 * is whether V_COLUMN_SHIFT collapses into sub-cell phase motion as cadence
 * rises. A shift of 0 columns with non-zero pixels is exactly that collapse. */
static unsigned long n_slide, n_slide_px[17], n_slide_col[9];
static unsigned long n_slide_subcell;   /* moved pixels, crossed no column */

/* Can a whole span be advanced by ONE shared delta, or does every column need
 * its own? This decides whether advancing retained state is O(1) per span or
 * O(columns) - i.e. whether an edge-only column escapes its geometry
 * derivation or merely its interior writes. */
static unsigned long n_span_delta_checked, n_span_delta_uniform;
static unsigned long n_span_delta_uniform_top;

/* What actually goes back into a vacated cell? This is the restoration
 * question, and it decides whether a baked restore token is 2 bits or a
 * visibility query. */
enum { RS_BG = 0, RS_KNOWN_SPAN, RS_NEW_SPAN, RS_COUNT };
static const char *k_rs_name[RS_COUNT] = {
    "background (ceiling/floor/horizon)",
    "a span ALREADY in retained state",
    "a span that appeared this update"
};
static unsigned long n_restore[RS_COUNT], n_restore_tot;
/* Of the known-span restores, how often was that span ALREADY contributing
 * the very same word to this cell last update, hidden underneath? If that is
 * high, a one-deep "second owner" underlay is the restore token and no
 * visibility query is needed. */
static unsigned long n_restore_underlay_hit;
/* How expensive is "which retained span owns this vacated cell?" There are
 * only a couple of visible spans, so the honest cost model is a near->far scan
 * over retained state, not a coverage bitmap - A25/A26 already ruled those
 * out. Measure the scan DEPTH, and how often the answer is simply the span
 * immediately behind in draw order, which a one-word "next owner" field in
 * retained state would answer for free. */
static unsigned long n_scan_depth[8], n_scan_tot, n_scan_adjacent;
static unsigned long n_naive_bad, n_naive_bad_cells, n_naive_cells;

/* Is the edge LUT already a temporal state machine?
 *   TSP_TILE_EDGE = BASE + ((shade*16 + off_index)*8) + slope_index
 * so within-cell vertical phase has stride 8 and quantized slope has stride 1,
 * both constant. If an edge cell's new word is the old word plus a constant,
 * the runtime move is `tile_id += delta` rather than a LUT address rebuild.
 * Measured, not assumed: the attribute bits (FLIPX/FLIPY/PALETTE) are set from
 * the sign of the slope, and both off and mag clamp, so the increment is only
 * valid away from those. */
static unsigned long n_edge_cmp, n_edge_same_attr, n_edge_delta8[9];
static unsigned long n_edge_incrementable;

static void edge_delta(uint16_t ow, uint16_t nw)
{
    int d;
    ++n_edge_cmp;
    if ((ow & (uint16_t)~TSP_TILE_ID_MASK) != (nw & (uint16_t)~TSP_TILE_ID_MASK))
        return;
    ++n_edge_same_attr;
    d = (int)(nw & TSP_TILE_ID_MASK) - (int)(ow & TSP_TILE_ID_MASK);
    /* phase steps are multiples of 8, slope steps are +/-1 */
    if (d % 8 == 0) {
        int k = d / 8;
        if (k >= -4 && k <= 4) { ++n_edge_delta8[k + 4]; ++n_edge_incrementable; }
    } else if (d >= -3 && d <= 3) {
        ++n_edge_incrementable;
    }
}
static unsigned long hist_dirty[TSP_MAP_CELLS + 1];
static unsigned long hist_ops[64];
static int reported_divergence = 0;

/* Baked-transition entropy: how many DISTINCT per-column boundary deltas are
 * there? A small alphabet is what makes a transition table bakeable. */
#define TRHASH 65536u
static uint8_t g_trseen[TRHASH];
static unsigned long n_tr_distinct, n_tr_total;

static void note_transition(const ColState *o, const ColState *n)
{
    int dtl = n->tl - o->tl, dtr = n->tr - o->tr;
    int dbl = n->bl - o->bl, dbr = n->br - o->br;
    unsigned h;
    if (dtl < -8) dtl = -8;
    if (dtl > 8) dtl = 8;
    if (dtr < -8) dtr = -8;
    if (dtr > 8) dtr = 8;
    if (dbl < -8) dbl = -8;
    if (dbl > 8) dbl = 8;
    if (dbr < -8) dbr = -8;
    if (dbr > 8) dbr = 8;
    h = (unsigned)((dtl + 8) | ((dtr + 8) << 5) | ((dbl + 8) << 10)
                 | ((dbr + 8) << 15));
    h &= TRHASH - 1u;
    ++n_tr_total;
    if (!g_trseen[h]) { g_trseen[h] = 1u; ++n_tr_distinct; }
}

static const SpanState *find_span(const Pose *p, uint8_t keyid)
{
    uint8_t i;
    for (i = 0; i < p->nsp; ++i)
        if (p->sp[i].keyid == keyid) return &p->sp[i];
    return NULL;
}

/* Self-check: the retained sextuple really is a complete description of what
 * a span writes. Re-derive the whole name table from span state alone and
 * require it to equal the renderer's own output. If this fails, the retained
 * state is missing an input and every number below is meaningless. */
static unsigned long selfcheck_fail;
static void selfcheck(const Pose *p)
{
    int r, c; uint8_t i;
    for (r = 0; r < (int)TSP_ROWS; ++r)
        for (c = 0; c < (int)TSP_COLS; ++c) {
            uint16_t w = base_word((uint8_t)r);
            for (i = 0; i < p->nsp; ++i) {
                uint16_t cw;
                if (contrib(&p->sp[i].col[c], r, &cw)) w = cw;
            }
            if (w != p->map[k_row_base[r] + c]) ++selfcheck_fail;
        }
}

/* Index of the span that WINS cell (r,c) in this pose, or -1 for background. */
static int winner_of(const Pose *p, int r, int c)
{
    int w = -1; uint8_t i; uint16_t cw;
    for (i = 0; i < p->nsp; ++i)
        if (contrib(&p->sp[i].col[c], r, &cw)) w = (int)i;
    return w;
}

static void classify(const Pose *prev, const Pose *cur)
{
    uint8_t i, j;
    int c, r, ops_this = 0;
    unsigned long dirty_here = 0;
    int dy;

    memset(g_dirty, 0, sizeof g_dirty);
    memset(g_cause, 0, sizeof g_cause);
    ++n_pairs;

    dy = (int)cur->yaw - (int)prev->yaw;
    if (dy > 128) dy -= 256;
    if (dy < -128) dy += 256;
    if (dy < 0) dy = -dy;
    bk = dy == 0 ? 0u : dy <= 2 ? 1u : dy <= 4 ? 2u : dy <= 8 ? 3u
       : dy <= 16 ? 4u : 5u;
    ++b_pairs[bk];

    /* Span slide, in pixels and in columns, and whether it stayed sub-cell. */
    for (i = 0; i < cur->nsp; ++i) {
        const SpanState *ps = find_span(prev, cur->sp[i].keyid);
        int dpx, dcol;
        if (!ps) continue;
        dpx = (int)cur->sp[i].x0 - (int)ps->x0;
        dcol = (int)cur->sp[i].c0 - (int)ps->c0;
        if (dpx < 0) dpx = -dpx;
        if (dcol < 0) dcol = -dcol;
        ++n_slide;
        ++n_slide_px[dpx > 16 ? 16 : dpx];
        ++n_slide_col[dcol > 8 ? 8 : dcol];
        if (dcol == 0 && dpx != 0) ++n_slide_subcell;
    }

    /* Is one shared delta enough to advance a whole span's boundary state? */
    for (i = 0; i < cur->nsp; ++i) {
        const SpanState *ps = find_span(prev, cur->sp[i].keyid);
        int have = 0, uni = 1, uni_top = 1;
        int d0 = 0, d1 = 0, d2 = 0, d3 = 0;
        if (!ps) continue;
        for (c = 0; c < (int)TSP_COLS; ++c) {
            const ColState *o = &ps->col[c], *n = &cur->sp[i].col[c];
            int e0, e1, e2, e3;
            if (!o->present || !n->present) continue;
            e0 = n->tl - o->tl; e1 = n->tr - o->tr;
            e2 = n->bl - o->bl; e3 = n->br - o->br;
            if (!have) { d0 = e0; d1 = e1; d2 = e2; d3 = e3; have = 1; continue; }
            if (e0 != d0 || e1 != d1 || e2 != d2 || e3 != d3) uni = 0;
            if (e0 != d0 || e1 != d1) uni_top = 0;
        }
        if (!have) continue;
        ++n_span_delta_checked;
        if (uni) ++n_span_delta_uniform;
        if (uni_top) ++n_span_delta_uniform_top;
    }

    /* Ownership: if two spans present in both poses swapped relative draw
     * order, every cell either covers could flip winner. Cheap to detect from
     * retained state, and it must not be skipped - A26 was bitten by exactly
     * this kind of cross-run state. */
    for (i = 0; i < cur->nsp; ++i) {
        const SpanState *pa = find_span(prev, cur->sp[i].keyid);
        if (!pa) continue;
        for (j = 0; j < cur->nsp; ++j) {
            const SpanState *pb;
            if (j == i) continue;
            pb = find_span(prev, cur->sp[j].keyid);
            if (!pb) continue;
            if ((cur->sp[i].rank < cur->sp[j].rank) != (pa->rank < pb->rank)) {
                /* Only cells the two spans BOTH cover can change winner.
                 * Marking every cell of both was the first rule here and it
                 * cost 48.6% of the dirty set under rotation, almost all of
                 * it cells only one span ever touched. */
                ++n_rank_flip;
                for (c = 0; c < (int)TSP_COLS; ++c) {
                    int a, b, lo1, hi1, lo2, hi2;
                    const ColState *A = cur->sp[i].col[c].present
                                      ? &cur->sp[i].col[c] : &pa->col[c];
                    const ColState *B = cur->sp[j].col[c].present
                                      ? &cur->sp[j].col[c] : &pb->col[c];
                    if (!A->present || !B->present) continue;
                    top_rows(A, &a, &b); lo1 = a; hi1 = b;
                    bot_rows(A, &a, &b); if (b > hi1) hi1 = b;
                    top_rows(B, &a, &b); lo2 = a; hi2 = b;
                    bot_rows(B, &a, &b); if (b > hi2) hi2 = b;
                    if (lo2 > lo1) lo1 = lo2;
                    if (hi2 < hi1) hi1 = hi2;
                    if (hi1 >= lo1) mark(lo1, hi1, c, CZ_TOPO);
                }
                ++n_op[OP_FALLBACK];
            }
        }
    }

    /* Spans that vanished: every cell they used to own must be restored. */
    for (i = 0; i < prev->nsp; ++i) {
        if (find_span(cur, prev->sp[i].keyid)) continue;
        for (c = 0; c < (int)TSP_COLS; ++c) {
            const ColState *o = &prev->sp[i].col[c];
            int a, b;
            if (!o->present) continue;
            top_rows(o, &a, &b); mark(a, b, c, CZ_TOPO);
            bot_rows(o, &a, &b); mark(a, b, c, CZ_TOPO);
            int_rows(o, &a, &b); if (a <= b) mark(a, b, c, CZ_TOPO);
            ++n_vacated;
        }
        ++n_span_vanish;
        ++n_op[OP_FALLBACK];
    }

    for (i = 0; i < cur->nsp; ++i) {
        const SpanState *cs = &cur->sp[i];
        const SpanState *ps = find_span(prev, cs->keyid);
        ++n_spans;
        if (!ps) {   /* span appeared */
            for (c = 0; c < (int)TSP_COLS; ++c) {
                const ColState *n = &cs->col[c];
                int a, b;
                if (!n->present) continue;
                top_rows(n, &a, &b); mark(a, b, c, CZ_TOPO);
                bot_rows(n, &a, &b); mark(a, b, c, CZ_TOPO);
                int_rows(n, &a, &b); if (a <= b) mark(a, b, c, CZ_TOPO);
                ++n_colmat; ++n_col_full; ++b_colmat[bk]; ++b_full[bk];
            }
            ++n_span_appear;
            ++n_op[OP_FALLBACK];
            ++ops_this;
            continue;
        }
        for (c = 0; c < (int)TSP_COLS; ++c) {
            const ColState *o = &ps->col[c], *n = &cs->col[c];
            int oa, ob, na, nb, op = OP_NO_CHANGE;
            int top_moved, bot_moved, phase_only = 1;

            /* A column neither pose touches is not an EVENT, it is empty
             * screen. Counting it as NO_CHANGE put the op distribution at
             * 71.7% NO_CHANGE, which was mostly the 20-column screen's blank
             * space congratulating itself. */
            if (!o->present && !n->present) continue;
            if (n->present) { ++n_colmat; ++b_colmat[bk]; }
            if (o->present != n->present) {
                const ColState *s = o->present ? o : n;
                top_rows(s, &oa, &ob); mark(oa, ob, c, CZ_COLSHIFT);
                bot_rows(s, &oa, &ob); mark(oa, ob, c, CZ_COLSHIFT);
                int_rows(s, &oa, &ob);
                if (oa <= ob) mark(oa, ob, c, CZ_COLSHIFT);
                if (o->present) ++n_vacated;
                if (n->present) { ++n_col_full; ++b_full[bk]; }
                ++n_op[OP_V_COLUMN_SHIFT]; ++ops_this;
                continue;
            }
            if (col_same(o, n)) {
                ++n_op[OP_NO_CHANGE]; ++n_col_skip; ++b_skip[bk]; continue;
            }

            note_transition(o, n);
            ++ops_this;

            if (o->shade != n->shade || o->border != n->border) {
                top_rows(o, &oa, &ob); top_rows(n, &na, &nb);
                mark(oa < na ? oa : na, ob > nb ? ob : nb, c, CZ_SHADE);
                bot_rows(o, &oa, &ob); bot_rows(n, &na, &nb);
                mark(oa < na ? oa : na, ob > nb ? ob : nb, c, CZ_SHADE);
                int_rows(o, &oa, &ob); int_rows(n, &na, &nb);
                if (oa <= ob) mark(oa, ob, c, CZ_SHADE);
                if (na <= nb) mark(na, nb, c, CZ_SHADE);
                ++n_op[OP_FALLBACK];
                ++n_col_full; ++b_full[bk];
                continue;
            }
            ++n_col_edge_only; ++b_edge[bk];

            top_moved = (o->tl != n->tl || o->tr != n->tr);
            bot_moved = (o->bl != n->bl || o->br != n->br);

            if (top_moved) {
                top_rows(o, &oa, &ob); top_rows(n, &na, &nb);
                for (r = (oa > na ? oa : na); r <= (ob < nb ? ob : nb); ++r)
                    edge_delta(edge_entry(o->shade, (int16_t)(o->tl - (r << 3)),
                                          clamp_s8((int16_t)(o->tr - o->tl), -7, 7), 0u),
                               edge_entry(n->shade, (int16_t)(n->tl - (r << 3)),
                                          clamp_s8((int16_t)(n->tr - n->tl), -7, 7), 0u));
                mark(oa < na ? oa : na, ob > nb ? ob : nb, c, CZ_TOP);
                if (oa != na || ob != nb) phase_only = 0;
                if (clamp_s8((int16_t)(o->tr - o->tl), -7, 7)
                    != clamp_s8((int16_t)(n->tr - n->tl), -7, 7)) {
                    op = OP_SLOPE_CHANGE; phase_only = 0;
                } else if (oa != na || ob != nb) {
                    op = OP_EDGE_CROSS;
                } else if ((n->tl - o->tl) != (n->tr - o->tr)) {
                    op = OP_PHASE_RAMP;
                } else if (op == OP_NO_CHANGE) {
                    op = OP_PHASE_SHIFT;
                }
            }
            if (bot_moved) {
                top_rows(o, &oa, &ob);  /* reuse locals below */
                bot_rows(o, &oa, &ob); bot_rows(n, &na, &nb);
                for (r = (oa > na ? oa : na); r <= (ob < nb ? ob : nb); ++r)
                    edge_delta(edge_entry(o->shade, (int16_t)(o->bl - (r << 3)),
                                          clamp_s8((int16_t)(o->br - o->bl), -7, 7), 1u),
                               edge_entry(n->shade, (int16_t)(n->bl - (r << 3)),
                                          clamp_s8((int16_t)(n->br - n->bl), -7, 7), 1u));
                mark(oa < na ? oa : na, ob > nb ? ob : nb, c, CZ_BOT);
                if (oa != na || ob != nb) phase_only = 0;
                if (clamp_s8((int16_t)(o->br - o->bl), -7, 7)
                    != clamp_s8((int16_t)(n->br - n->bl), -7, 7)) {
                    op = OP_SLOPE_CHANGE; phase_only = 0;
                } else if (oa != na || ob != nb) {
                    if (op != OP_SLOPE_CHANGE) op = OP_EDGE_CROSS;
                } else if ((n->bl - o->bl) != (n->br - o->br)) {
                    if (op == OP_NO_CHANGE || op == OP_PHASE_SHIFT)
                        op = OP_PHASE_RAMP;
                } else if (op == OP_NO_CHANGE) {
                    op = OP_PHASE_SHIFT;
                }
            }

            /* THE HYPOTHESIS, in one statement: interior rows that stay
             * interior at the same shade and border are NOT dirty, however
             * far the projected geometry moved. Only the symmetric difference
             * is touched. */
            int_rows(o, &oa, &ob); int_rows(n, &na, &nb);
            {
                int grew = 0, shrank = 0;
                for (r = 0; r < (int)TSP_ROWS; ++r) {
                    int wasi = (oa <= ob && r >= oa && r <= ob);
                    int isi  = (na <= nb && r >= na && r <= nb);
                    if (wasi && isi) ++n_interior_preserved;
                    if (wasi != isi) {
                        ++n_interior_swapped;
                        mark(r, r, c, CZ_INTERIOR);
                        if (isi) grew = 1; else { shrank = 1; ++n_vacated; }
                        phase_only = 0;
                    }
                }
                /* Grow/shrink are ATTRIBUTES, not a mutually exclusive class:
                 * a column can ramp its top edge and grow downward in the same
                 * update. Counting them only when nothing else fired reported
                 * them as 0.0% and hid a real 7.41 cells/update. */
                if (grew) ++n_grow;
                if (shrank) ++n_shrink;
                if (op == OP_NO_CHANGE || op == OP_PHASE_SHIFT) {
                    if (grew && !shrank) op = OP_V_GROW;
                    else if (shrank && !grew) op = OP_V_SHRINK;
                    else if (grew && shrank) op = OP_EDGE_CROSS;
                }
            }
            if (phase_only) ++n_phase_only;
            ++n_op[op];
        }
    }

    /* What replaces a vacated cell? Measured, because a runtime cannot afford
     * the full ordered-run resolution this probe uses to find out.
     *
     * A cell is vacated only if the span OWNED it - won the far->near race -
     * and no longer does. The first version of this counted every cell a span
     * merely CONTRIBUTED to, including ones a nearer span was already
     * covering, which inflated "replaced by a different span" to 79.9%. A far
     * span losing a cell it was never showing is not a restoration event. */
    for (i = 0; i < prev->nsp; ++i) {
        const SpanState *cs = find_span(cur, prev->sp[i].keyid);
        for (c = 0; c < (int)TSP_COLS; ++c) {
            for (r = 0; r < (int)TSP_ROWS; ++r) {
                uint16_t cw;
                int wp, wn;
                if (!contrib(&prev->sp[i].col[c], r, &cw)) continue;
                wp = winner_of(prev, r, c);
                if (wp < 0 || prev->sp[wp].keyid != prev->sp[i].keyid) continue;
                wn = winner_of(cur, r, c);
                if (wn >= 0 && cs && cur->sp[wn].keyid == cs->keyid) continue;
                ++n_restore_tot;
                if (wn < 0) { ++n_restore[RS_BG]; continue; }
                {   /* near->far scan depth to find the new owner */
                    int d = 0, q;
                    for (q = (int)cur->nsp - 1; q >= 0; --q) {
                        uint16_t tw;
                        ++d;
                        if (contrib(&cur->sp[q].col[c], r, &tw)) break;
                    }
                    ++n_scan_tot;
                    ++n_scan_depth[d > 7 ? 7 : d];
                    if (wp >= 0 && wn == wp - 1) ++n_scan_adjacent;
                }
                if (find_span(prev, cur->sp[wn].keyid)) {
                    const SpanState *op = find_span(prev, cur->sp[wn].keyid);
                    uint16_t noww, thenw;
                    ++n_restore[RS_KNOWN_SPAN];
                    contrib(&cur->sp[wn].col[c], r, &noww);
                    if (contrib(&op->col[c], r, &thenw) && thenw == noww)
                        ++n_restore_underlay_hit;
                } else ++n_restore[RS_NEW_SPAN];
            }
        }
    }

    /* IS PER-SPAN SKIPPING SAFE ON ITS OWN?
     *
     * The obvious Z80 rung is "if this span's column state is unchanged, skip
     * the column" - a purely local test, no cross-span union, no second pass.
     * It is worth knowing BEFORE writing that kernel whether it is correct,
     * because the union is what makes the dirty set expensive and A26 died on
     * exactly that kind of classifier cost.
     *
     * Simulate it: start from the previous name table, and for every span in
     * far->near order redraw only the columns whose own state changed. Compare
     * against the true new image. */
    {
        static uint16_t naive[TSP_MAP_CELLS];
        memcpy(naive, prev->map, sizeof naive);
        for (i = 0; i < cur->nsp; ++i) {
            const SpanState *ps = find_span(prev, cur->sp[i].keyid);
            for (c = 0; c < (int)TSP_COLS; ++c) {
                const ColState *n = &cur->sp[i].col[c];
                const ColState *o = ps ? &ps->col[c] : NULL;
                if (!n->present) continue;
                if (o && col_same(o, n)) continue;      /* the local skip */
                for (r = 0; r < (int)TSP_ROWS; ++r) {
                    uint16_t w;
                    if (contrib(n, r, &w)) naive[k_row_base[r] + c] = w;
                }
            }
        }
        for (c = 0; c < (int)TSP_MAP_CELLS; ++c) {
            ++n_naive_cells;
            if (naive[c] != cur->map[c]) ++n_naive_bad_cells;
        }
        for (c = 0; c < (int)TSP_MAP_CELLS; ++c)
            if (naive[c] != cur->map[c]) { ++n_naive_bad; break; }
    }

    /* ---- metrics + the go/no-go verification ---- */
    for (c = 0; c < (int)TSP_COLS; ++c) {
        const SpanState *s;
        int a, b;
        (void)s; (void)a; (void)b;
        for (r = 0; r < (int)TSP_ROWS; ++r) {
            int idx = k_row_base[r] + c;
            int changed = (prev->map[idx] != cur->map[idx]);
            if (!changed) ++n_cells_same; else ++n_cells_true_changed;
            if (!changed) ; else ++b_true[bk];
            if (g_dirty[r][c]) {
                ++n_dirty; ++dirty_here; ++n_cause[g_cause[r][c]];
                ++b_dirty[bk];
            }
            /* A cell outside the dirty set must ALREADY be correct. */
            if (changed && !g_dirty[r][c]) {
                ++verify_cells_missed;
                if (!reported_divergence) {
                    reported_divergence = 1;
                    fprintf(stderr,
                        "DIVERGENCE row=%d col=%d prev=%04x cur=%04x"
                        "  pose prev(%d,%d,yaw %u) cur(%d,%d,yaw %u)\n",
                        r, c, prev->map[idx], cur->map[idx],
                        prev->x_q4, prev->y_q4, prev->yaw,
                        cur->x_q4, cur->y_q4, cur->yaw);
                }
            }
        }
    }
    if (verify_cells_missed) ++verify_fail;

    /* Row-writes the CURRENT materializer performs, for the ratio. */
    for (i = 0; i < cur->nsp; ++i)
        for (c = 0; c < (int)TSP_COLS; ++c) {
            const ColState *n = &cur->sp[i].col[c];
            int a, b, lo, hi;
            if (!n->present) continue;
            top_rows(n, &a, &b); lo = a; hi = b;
            bot_rows(n, &a, &b); if (a < lo) lo = a; if (b > hi) hi = b;
            if (hi >= lo) {
                n_rowwrites_full += (unsigned long)(hi - lo + 1);
                b_rowwr[bk] += (unsigned long)(hi - lo + 1);
            }
        }

    ++hist_dirty[dirty_here > TSP_MAP_CELLS ? TSP_MAP_CELLS : dirty_here];
    n_span_ops += (unsigned long)ops_this;
    ++hist_ops[ops_this > 63 ? 63 : ops_this];
}

static unsigned long pct_of(const unsigned long *h, unsigned n, double q)
{
    unsigned long tot = 0, acc = 0; unsigned i;
    for (i = 0; i < n; ++i) tot += h[i];
    for (i = 0; i < n; ++i) { acc += h[i]; if (acc >= (unsigned long)(q * tot)) return i; }
    return n ? n - 1 : 0;
}

/* ---- corpus: identical to A28's so the two are directly comparable ---- */

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

int main(int argc, char **argv)
{
    unsigned U = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 4u;
    unsigned frames = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 0) : 240u;
    unsigned yaw_step = (argc > 3) ? (unsigned)strtoul(argv[3], 0, 0) : 64u;
    int only = (argc > 4) ? (int)strtol(argv[4], 0, 0) : -1;
    const char *seq_path = (argc > 5) ? argv[5] : NULL;
    unsigned t, f, gx, gy, yy, i;

    if (!U) U = 1;
    if (!yaw_step) yaw_step = 64u;
    g_tspf_appearance_mode = 0u;
    if (seq_path) {
        g_seq = fopen(seq_path, "w");
        if (!g_seq) { fprintf(stderr, "cannot open %s\n", seq_path); return 1; }
        g_seq_stride = (argc > 6) ? (unsigned long)strtoul(argv[6], 0, 0) : 64ul;
        if (!g_seq_stride) g_seq_stride = 1;
    }

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
                Pose *prev = &g_p[0], *cur = &g_p[1], *sw;
                int have_prev = 0;
                /* Sample whole trajectories, not whole runs of every
                 * trajectory - one in `g_seq_stride` spawn/yaw pairs
                 * contributes its entire pose sequence. */
                g_seq_run = g_seq && ((g_seq_traj++ % g_seq_stride) == 0);
                tsp_reset(&st);
                st.x_q4 = px; st.y_q4 = py;
                st.yaw = (uint8_t)yy; st.manual = k_regime[t].manual;
                for (f = 0; f < frames; ++f) {
                    tsp_step(&st, k_regime[t].inp[(f / 30u) % 8u]);
                    if ((f % U) != U - 1) continue;
                    build_pose(&st, cur);
                    if (have_prev) { selfcheck(cur); classify(prev, cur); }
                    /* Emit whole consecutive RUNS of poses, never isolated
                     * ones: a temporal kernel is only meaningful on a
                     * sequence, and striding inside a trajectory would hand it
                     * a delta the real runtime never sees. */
                    if (g_seq) {
                        if (!have_prev) { if (g_seq_run) dump_pose(cur, 1); }
                        else if (g_seq_run) dump_pose(cur, 0);
                    }
                    have_prev = 1;
                    sw = prev; prev = cur; cur = sw;
                }
            }
        }
    }

    printf("=== TEMPORAL BOUNDARY EVENTS (perspective projection, unchanged) ===\n");
    printf("regime %s   U=%u   yaw step %u   update pairs %lu\n\n",
           only >= 0 ? k_regime[only].name : "ALL", U, yaw_step, n_pairs);

    printf("SELF-CHECK - is the retained sextuple a complete span description?\n");
    printf("  cells where state-only re-derivation != renderer output   %lu\n",
           selfcheck_fail);
    printf("  %s\n\n", selfcheck_fail ? "*** RETAINED STATE IS INCOMPLETE ***"
                                    : "complete");

    printf("GO/NO-GO - does the boundary representation DESCRIBE every visual change?\n");
    printf("  cells that changed but were NOT in the dirty set   %lu\n",
           verify_cells_missed);
    printf("  update pairs with any such miss                    %lu / %lu\n",
           verify_fail, n_pairs);
    printf("  VERDICT: %s\n\n",
           verify_cells_missed ? "*** REPRESENTATION INCOMPLETE ***"
                               : "complete - dirty set is a superset of all change");

    printf("THE HEADLINE - work that survives the boundary filter\n");
    printf("  row-writes the current materializer performs   %8.2f /update\n",
           (double)n_rowwrites_full / n_pairs);
    printf("  cells the boundary classifier marks dirty      %8.2f /update"
           "   (%.1f%%)\n",
           (double)n_dirty / n_pairs,
           100.0 * n_dirty / (double)n_rowwrites_full);
    printf("  cells that ACTUALLY changed (exact floor)      %8.2f /update"
           "   (%.1f%%)\n",
           (double)n_cells_true_changed / n_pairs,
           100.0 * n_cells_true_changed / (double)n_rowwrites_full);
    printf("  classifier overshoot vs that floor             %8.2fx\n",
           n_cells_true_changed
             ? (double)n_dirty / (double)n_cells_true_changed : 0.0);
    printf("  final cells unchanged                          %8.1f%%\n",
           100.0 * n_cells_same / (n_pairs * (double)TSP_MAP_CELLS));
    printf("  dirty cells  mean %.2f   p95 %lu   max %lu\n",
           (double)n_dirty / n_pairs,
           pct_of(hist_dirty, TSP_MAP_CELLS + 1, 0.95),
           pct_of(hist_dirty, TSP_MAP_CELLS + 1, 1.0));

    printf("\nAT THE UNIT THE KERNEL IS PRICED IN - COLUMN-MATERIALIZATIONS\n");
    printf("  column-materializations the kernel does now  %7.2f /update\n",
           (double)n_colmat / n_pairs);
    printf("  fully skippable   (state identical)          %7.2f   %5.1f%%\n",
           (double)n_col_skip / n_pairs,
           n_colmat ? 100.0 * n_col_skip / n_colmat : 0.0);
    printf("  edge-only         (interior stays resident)  %7.2f   %5.1f%%\n",
           (double)n_col_edge_only / n_pairs,
           n_colmat ? 100.0 * n_col_edge_only / n_colmat : 0.0);
    printf("  full derivation   (new col / border / topo)  %7.2f   %5.1f%%\n",
           (double)n_col_full / n_pairs,
           n_colmat ? 100.0 * n_col_full / n_colmat : 0.0);
    printf("  interior cells left resident                 %7.2f /update\n",
           (double)n_interior_preserved / n_pairs);
    printf("  interior cells entering/leaving              %7.2f /update\n",
           (double)n_interior_swapped / n_pairs);

    printf("\nIS A PURELY LOCAL PER-SPAN SKIP CORRECT? (no cross-span union)\n");
    printf("  update pairs whose image comes out WRONG   %lu / %lu   %.1f%%\n",
           n_naive_bad, n_pairs, 100.0 * n_naive_bad / n_pairs);
    printf("  cells wrong                                %.3f /update  %.3f%%\n",
           (double)n_naive_bad_cells / n_pairs,
           n_naive_cells ? 100.0 * n_naive_bad_cells / n_naive_cells : 0.0);

    printf("\nBY |dyaw| PER UPDATE - the physically meaningful variable\n");
    printf("  %-8s %9s %9s %9s %8s %8s %8s\n",
           "|dyaw|", "pairs", "dirty%", "floor%", "skip%", "edge%", "full%");
    for (i = 0; i < NBK; ++i) {
        if (!b_pairs[i]) continue;
        printf("  %-8s %9lu %8.1f%% %8.1f%% %7.1f%% %7.1f%% %7.1f%%\n",
               k_bk_name[i], b_pairs[i],
               b_rowwr[i] ? 100.0 * b_dirty[i] / b_rowwr[i] : 0.0,
               b_rowwr[i] ? 100.0 * b_true[i] / b_rowwr[i] : 0.0,
               b_colmat[i] ? 100.0 * b_skip[i] / b_colmat[i] : 0.0,
               b_colmat[i] ? 100.0 * b_edge[i] / b_colmat[i] : 0.0,
               b_colmat[i] ? 100.0 * b_full[i] / b_colmat[i] : 0.0);
    }

    printf("\nDOES V_COLUMN_SHIFT COLLAPSE? span slide per update\n");
    printf("  spans tracked %lu   stayed sub-cell (pixels moved, no column"
           " crossed) %.1f%%\n",
           n_slide, n_slide ? 100.0 * n_slide_subcell / n_slide : 0.0);
    printf("  |dcolumns|:");
    for (i = 0; i < 9; ++i)
        if (n_slide_col[i])
            printf("  %u:%.1f%%", i, 100.0 * n_slide_col[i] / n_slide);
    printf("\n  |dpixels| :");
    for (i = 0; i < 17; ++i)
        if (n_slide_px[i] && 100.0 * n_slide_px[i] / n_slide >= 0.5)
            printf("  %u:%.1f%%", i, 100.0 * n_slide_px[i] / n_slide);
    printf("\n");

    printf("\nCAN A WHOLE SPAN BE ADVANCED BY ONE SHARED DELTA?\n");
    printf("  (decides whether an edge-only column escapes its GEOMETRY\n"
           "   derivation or only its interior writes - the open A29 question)\n");
    printf("  spans present in both poses            %lu\n",
           n_span_delta_checked);
    printf("  one delta covers all four boundaries   %.1f%%\n",
           n_span_delta_checked
             ? 100.0 * n_span_delta_uniform / n_span_delta_checked : 0.0);
    printf("  one delta covers the TOP edge alone    %.1f%%\n",
           n_span_delta_checked
             ? 100.0 * n_span_delta_uniform_top / n_span_delta_checked : 0.0);

    printf("\nWHAT REPLACES A VACATED CELL? (the restoration question)\n");
    for (i = 0; i < RS_COUNT; ++i)
        printf("  %-36s %7.1f%%   %.2f /update\n", k_rs_name[i],
               n_restore_tot ? 100.0 * n_restore[i] / n_restore_tot : 0.0,
               (double)n_restore[i] / n_pairs);
    printf("  total vacated cells                    %.2f /update\n",
           (double)n_restore_tot / n_pairs);
    printf("  of the known-span restores, the SAME word was already being\n"
           "  contributed underneath last update      %.1f%%"
           "   <- a one-deep underlay is enough here\n",
           n_restore[RS_KNOWN_SPAN]
             ? 100.0 * n_restore_underlay_hit / n_restore[RS_KNOWN_SPAN] : 0.0);

    printf("  near->far scan depth to find the new owner:");
    for (i = 0; i < 8; ++i)
        if (n_scan_depth[i])
            printf("  %u:%.1f%%", i, 100.0 * n_scan_depth[i] / n_scan_tot);
    printf("\n  new owner is the span immediately behind the old one  %.1f%%\n",
           n_scan_tot ? 100.0 * n_scan_adjacent / n_scan_tot : 0.0);

    printf("\nDIRTY CELLS BY CAUSE (first cause to claim the cell; the edge\n"
           "unions subsume most interior transitions, so read the column\n"
           "table above for the interior question)\n");
    for (i = 0; i < CZ_COUNT; ++i)
        printf("  %-24s %8.2f /update   %5.1f%%\n", k_cz_name[i],
               (double)n_cause[i] / n_pairs,
               n_dirty ? 100.0 * n_cause[i] / n_dirty : 0.0);
    printf("  %-24s %8.2f /update\n",
           "of which phase-only cols", (double)n_phase_only / n_pairs);

    printf("\nSPAN-COLUMN EVENTS BY CLASS\n");
    {
        unsigned long tot = 0;
        for (i = 0; i < OP_COUNT; ++i) tot += n_op[i];
        for (i = 0; i < OP_COUNT; ++i)
            printf("  %-16s %10lu   %5.1f%%\n", k_op_name[i], n_op[i],
                   tot ? 100.0 * n_op[i] / tot : 0.0);
    }
    printf("  temporal ops per update  mean %.2f  p95 %lu\n",
           (double)n_span_ops / n_pairs, pct_of(hist_ops, 64, 0.95));
    printf("  visible spans per update mean %.2f\n", (double)n_spans / n_pairs);
    printf("  ops per visible span     mean %.2f\n",
           n_spans ? (double)n_span_ops / n_spans : 0.0);

    printf("  vertical GROW events %.2f /update   SHRINK %.2f /update"
           "   (attributes, not exclusive classes)\n",
           (double)n_grow / n_pairs, (double)n_shrink / n_pairs);

    printf("\nIS THE EDGE LUT ALREADY A TEMPORAL STATE MACHINE?\n");
    printf("  TSP_TILE_EDGE = BASE + ((shade*16 + off)*8) + slope\n"
           "  so vertical phase has stride 8 and quantized slope stride 1.\n");
    printf("  edge cells compared old vs new          %lu\n", n_edge_cmp);
    printf("  attribute bits (FLIPX/FLIPY/PAL) equal  %.1f%%\n",
           n_edge_cmp ? 100.0 * n_edge_same_attr / n_edge_cmp : 0.0);
    printf("  reachable by `tile_id += small delta`   %.1f%%\n",
           n_edge_cmp ? 100.0 * n_edge_incrementable / n_edge_cmp : 0.0);
    printf("  phase delta distribution (units of 8):");
    for (i = 0; i < 9; ++i)
        if (n_edge_delta8[i])
            printf("  %+d:%.1f%%", (int)i - 4,
                   100.0 * n_edge_delta8[i] / n_edge_cmp);
    printf("\n");

    printf("\nTOPOLOGY EVENTS PER UPDATE\n");
    printf("  spans appearing   %.3f    vanishing %.3f    draw-order flips %.3f\n",
           (double)n_span_appear / n_pairs, (double)n_span_vanish / n_pairs,
           (double)n_rank_flip / n_pairs);

    printf("\nRESTORATION LOAD (the open problem)\n");
    printf("  cells/column-runs vacated by a shrinking or departing boundary"
           "  %.2f /update\n", (double)n_vacated / n_pairs);
    printf("  Their correct new content is background, a FULL interior, or"
           " another\n  farther span. This probe recomputes them; the Z80 cost"
           " of deciding which\n  is NOT measured here.\n");

    printf("\nBAKED-TRANSITION ENTROPY (metric 13, indicative)\n");
    printf("  per-column boundary deltas observed   %lu\n", n_tr_total);
    printf("  distinct (dtl,dtr,dbl,dbr) codes      %lu\n", n_tr_distinct);
    printf("  (clamped to +/-8; a small alphabet is what makes a transition\n"
           "   table bakeable. This counts geometry deltas only, not the\n"
           "   cell-level programs they expand into.)\n");
    if (g_seq) {
        fclose(g_seq);
        fprintf(stderr, "sequence oracle: %lu poses dumped"
                " (1 trajectory in %lu)\n", g_seq_emitted, g_seq_stride);
    }
    return (verify_cells_missed || selfcheck_fail) ? 1 : 0;
}
