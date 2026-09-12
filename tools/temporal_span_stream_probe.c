/*
 * SPARSE SPAN STREAM: is the right retained unit the span, not the column?
 *
 * A29-A31 retain three bytes per (span, column) and compare per column. A33
 * measured the Z80 cost of that and it is 35-55% of a full render, with the
 * cost sitting in per-column iteration rather than in any real work. The
 * hypothesis here is that the column is the wrong unit: represent the frame as
 * a sorted stream of vertical boundary events with one compact record per
 * visible span between them, and both the comparison and the dirty-region
 * derivation shrink to the number of spans.
 *
 * THE FIRST THING TO SETTLE, AND IT SIMPLIFIES THE PROPOSAL
 * --------------------------------------------------------
 * The premise assumed an exact edge X is one byte - five bits of tile column
 * plus three of sub-tile pixel - and that several edges may share a tile.
 * `draw_run` does not consume sub-tile X at all:
 *
 *     c0 = x0 >> 3;  c1 = x1 >> 3;  ... for (c = c0; c <= c1; ++c)
 *
 * Nothing else reads x0 or x1. The span footprint is TILE-COLUMN granular, so
 * an edge position needs 5 bits, not 8, and "two edges in one tile" is just
 * two spans whose c0 is the same column. That is not an approximation being
 * introduced here - it is the exact renderer's existing semantics, and this
 * probe proves it by reconstruction rather than by reading the source.
 *
 * THE RECORD
 * ----------
 * Everything draw_run consumes about one span:
 *
 *     sid (1) | inv0 (1) | inv1 (1) | c0 (1) | c1 (1) | flags (1)   = 6 bytes
 *
 * flags carries left_real and right_real. sid supplies profile via
 * k_tspf_profile and the shade bias; in appearance mode 0 shade is constant.
 * Draw order is the array order, far->near, so ownership is positional and
 * needs no stored identifier.
 *
 * Against the retained-column form that is 6 bytes per span instead of 3 bytes
 * times the span's column count.
 *
 * EXACTNESS IS THE GATE
 * ---------------------
 * Reconstruct the whole 20x18 name table from the stream alone and require it
 * to equal the renderer's own output on every pose. Not close - equal.
 *
 * usage: temporal_span_stream_probe [U] [frames] [yaw_step] [regime]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define MAXSPAN TSPF_MAX_ACTIVE

typedef struct {
    uint8_t sid, inv0, inv1, c0, c1, flags;
    uint8_t keyid;      /* persistent identity; NOT part of the 6-byte record */
    uint8_t inv_mid;    /* the DEPTH SORT KEY, for the inversion classifier */
} SpanRec;

typedef struct {
    SpanRec sp[MAXSPAN];
    uint8_t n;
    uint16_t map[TSP_MAP_CELLS];
    int16_t x_q4, y_q4;
    uint8_t yaw;
    uint8_t x0[MAXSPAN], x1[MAXSPAN];   /* pixel extents, for the sub-tile stat */
} Frame;

static Frame g_a, g_b;
static uint8_t g_key_of_run[MAXSPAN];

/* Reconstruct one span's contribution from its six bytes alone. This is
 * draw_run's column loop with nothing else available to it. */
static void replay_span(uint16_t *out, const SpanRec *s)
{
    uint8_t c0 = s->c0, c1 = s->c1, n, c;
    uint8_t profile = k_tspf_profile[s->sid];
    int16_t iq, step;
    if (c1 < c0) return;
    n = (uint8_t)(c1 - c0 + 1u);
    iq = (int16_t)((int16_t)s->inv0 << 6);
    step = (int16_t)(((int16_t)s->inv1 - (int16_t)s->inv0)
                     * (int16_t)k_col_recip_q8[n]);
    step = shr_signed(step, 2);
    for (c = c0; c <= c1; ++c) {
        uint8_t il = (uint8_t)clamp_u8i((int16_t)((iq + 32) >> 6), 255u);
        uint8_t ir = (uint8_t)clamp_u8i((int16_t)((iq + step + 32) >> 6), 255u);
        uint8_t mid = (uint8_t)(((uint16_t)il + ir) >> 1);
        uint8_t hl = (uint8_t)(il >> 1), hr = (uint8_t)(ir >> 1);
        int16_t tl = (int16_t)(TSPF_HORIZON - hl), tr = (int16_t)(TSPF_HORIZON - hr);
        int16_t bl = (int16_t)(TSPF_HORIZON + hl), br = (int16_t)(TSPF_HORIZON + hr);
        uint8_t border = 0, shade;
        if (profile == TSP_PROFILE_FULL) { tl--; tr--; }
        if (c == c0 && (s->flags & 1u)) border |= 1u;
        if (c == c1 && (s->flags & 2u)) border |= 2u;
        shade = g_tspf_appearance_mode
              ? shade_for(mid, k_tspf_shade_bias[s->sid]) : 1u;
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
        draw_edge(out, c, tl, tr, shade, 0u);
        draw_edge(out, c, bl, br, shade, 1u);
        draw_full(out, c, (int8_t)(row_floor(tl > tr ? tl : tr) + 1),
                  (int8_t)(row_floor(bl < br ? bl : br) - 1), shade, border);
        iq = (int16_t)(iq + step);
    }
}

static void build_frame(const TSPState *st, Frame *f)
{
    uint8_t ks[64], nk = 0, count = 0, j;
    uint8_t recipe, base_id, cond_count, lx, ly, gx, gy;
    uint16_t gi, offs, i;
    const uint8_t *pp, *b;

    f->n = 0;
    f->x_q4 = st->x_q4; f->y_q4 = st->y_q4; f->yaw = st->yaw;
    map_init(f->map);

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
        g_key_of_run[count] = ks[j];
        insert_run(count, &count);
    }

    for (i = 0; i < count; ++i) {
        const PolarRun *r = &g_runs[g_run_order[i]];
        uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3);
        SpanRec *s;
        draw_run(f->map, NULL, r);        /* the renderer's own output */
        if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
        if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
        if (c1 < c0) continue;
        s = &f->sp[f->n];
        s->sid = r->sid; s->inv0 = r->inv0; s->inv1 = r->inv1;
        s->c0 = c0; s->c1 = c1;
        s->flags = (uint8_t)((r->left_real ? 1u : 0u)
                           | (r->right_real ? 2u : 0u));
        s->keyid = g_key_of_run[g_run_order[i]];
        s->inv_mid = r->inv_mid;
        f->x0[f->n] = r->x0; f->x1[f->n] = r->x1;
        ++f->n;
    }
}

/* ================= DRAW-ORDER INVERSION CLASSIFIER =================
 *
 * A35 found that inversions caused the union's whole tail, and fixed the
 * handling. But the mechanism was never established, and the premise deserves
 * scrutiny: under PURE ROTATION the camera does not move, so two static
 * non-intersecting walls cannot exchange world-space depth. Something else is
 * reordering them.
 *
 * The sort key is `inv_mid = (inv0 + inv1) >> 1` - the midpoint inverse depth
 * of the span's PROJECTED, FOV-CLIPPED endpoints. A28 established that inv0
 * and inv1 carry an explicit sec(bearing - yaw) factor, so they move under pure
 * yaw even for a corner at constant range. A key built from them can therefore
 * cross with no physical depth reversal at all.
 *
 * Classification is by COUNTERFACTUAL, not by inspection: re-render the new
 * frame with the pair forced back to their previous relative order. If the
 * image changes, the inversion was load-bearing. If not, it was stream order
 * and nothing else.
 */
static const SpanRec *find_rec(const Frame *f, uint8_t keyid);
static unsigned long pct(const unsigned long *h, unsigned n, double q);

static unsigned long inv_pairs, inv_loadbearing, inv_streamonly;
static unsigned long inv_tie_prev, inv_tie_cur, inv_key_crossed;
static unsigned long inv_clip_changed, inv_extent_changed, inv_no_overlap;
static unsigned long inv_overlap_sum, inv_overlap_max, inv_hist_ov[TSP_COLS + 1];
static unsigned long inv_updates, inv_updates_any;

static void render_order(const SpanRec *sp, const uint8_t *ord, uint8_t n,
                         uint16_t *out)
{
    uint8_t i;
    unsigned save = g_touched_count;
    map_init(out);
    for (i = 0; i < n; ++i) replay_span(out, &sp[ord[i]]);
    g_touched_count = save;
}

static void classify_inversions(const Frame *p, const Frame *c)
{
    uint8_t i, j, ord[MAXSPAN];
    static uint16_t alt[TSP_MAP_CELLS];
    int any = 0;
    ++inv_updates;
    for (i = 0; i < c->n; ++i) ord[i] = i;
    for (i = 0; i < c->n; ++i) {
        const SpanRec *pi = find_rec(p, c->sp[i].keyid);
        int ri;
        if (!pi) continue;
        ri = (int)(pi - p->sp);
        for (j = (uint8_t)(i + 1); j < c->n; ++j) {
            const SpanRec *pj = find_rec(p, c->sp[j].keyid);
            int rj, ov, lo, hi;
            uint8_t t;
            if (!pj) continue;
            rj = (int)(pj - p->sp);
            if (ri < rj) continue;              /* order held */
            ++inv_pairs;
            any = 1;

            /* mechanism */
            if (pi->inv_mid == pj->inv_mid) ++inv_tie_prev;
            if (c->sp[i].inv_mid == c->sp[j].inv_mid) ++inv_tie_cur;
            if ((pi->inv_mid > pj->inv_mid)
                != (c->sp[i].inv_mid > c->sp[j].inv_mid)) ++inv_key_crossed;
            if (pi->flags != c->sp[i].flags || pj->flags != c->sp[j].flags)
                ++inv_clip_changed;
            if (pi->c0 != c->sp[i].c0 || pi->c1 != c->sp[i].c1
                || pj->c0 != c->sp[j].c0 || pj->c1 != c->sp[j].c1)
                ++inv_extent_changed;

            /* overlap width in the new frame */
            lo = c->sp[i].c0 > c->sp[j].c0 ? c->sp[i].c0 : c->sp[j].c0;
            hi = c->sp[i].c1 < c->sp[j].c1 ? c->sp[i].c1 : c->sp[j].c1;
            ov = hi - lo + 1;
            if (ov < 0) ov = 0;
            inv_overlap_sum += (unsigned long)ov;
            if ((unsigned long)ov > inv_overlap_max) inv_overlap_max = (unsigned long)ov;
            ++inv_hist_ov[ov > (int)TSP_COLS ? TSP_COLS : ov];
            if (!ov) { ++inv_no_overlap; ++inv_streamonly; continue; }

            /* counterfactual: force the pair back to the old relative order */
            t = ord[i]; ord[i] = ord[j]; ord[j] = t;
            render_order(c->sp, ord, c->n, alt);
            t = ord[i]; ord[i] = ord[j]; ord[j] = t;
            if (memcmp(alt, c->map, sizeof alt)) ++inv_loadbearing;
            else ++inv_streamonly;
        }
    }
    if (any) ++inv_updates_any;
}

/* ---- oracle for the Z80 span-stream union ----
 *
 * The requirement is stronger and simpler than A33's: dump both span streams
 * and the set of cells that ACTUALLY changed between the two rendered frames.
 * A kernel's dirty mask must be a superset of that. A33 verified against the
 * host classifier's own conservative mask, which measures agreement with one
 * particular classifier; this measures agreement with the truth.
 *
 *   line := <n_cur> [keyid sid inv0 inv1 c0 c1 flags profile]*n_cur
 *           <n_prev> [same]*n_prev
 *           <54 mask bytes, row-major, 3 per row>
 */
static FILE *g_dump = NULL;
static unsigned long g_dump_stride = 1, g_dump_seen = 0, g_dump_out = 0;

static void dump_stream(const Frame *f)
{
    uint8_t i;
    fprintf(g_dump, "%u", f->n);
    for (i = 0; i < f->n; ++i) {
        const SpanRec *s = &f->sp[i];
        fprintf(g_dump, " %u %u %u %u %u %u %u %u", s->keyid, s->sid,
                s->inv0, s->inv1, s->c0, s->c1, s->flags,
                k_tspf_profile[s->sid]);
    }
}

static void dump_pair(const Frame *p, const Frame *c)
{
    int r, col;
    if (!g_dump) return;
    if (g_dump_seen++ % g_dump_stride) return;
    dump_stream(c);
    fputc(' ', g_dump);
    dump_stream(p);
    for (r = 0; r < (int)TSP_ROWS; ++r) {
        unsigned m = 0;
        for (col = 0; col < (int)TSP_COLS; ++col)
            if (p->map[k_row_base[r] + col] != c->map[k_row_base[r] + col])
                m |= 1u << col;
        fprintf(g_dump, " %u %u %u", m & 0xffu, (m >> 8) & 0xffu,
                (m >> 16) & 0xffu);
    }
    fputc('\n', g_dump);
    ++g_dump_out;
}

/* ---------------- measurement ---------------- */

static unsigned long n_frames, n_exact_fail;
static unsigned long n_spans, hist_spans[MAXSPAN + 1];
static unsigned long n_events, n_ev_same_tile, n_ev_tot;
static unsigned long n_pairs;
static unsigned long n_span_same, n_span_changed, n_span_appear, n_span_vanish;
static unsigned long n_cols_of_changed, n_cols_total;
static unsigned long n_edge_moved, n_edge_still, edge_move_sum, edge_move_max;
static unsigned long n_geom_only, n_extent_only, n_both;
static unsigned long hist_changed[MAXSPAN + 1];
/* Cost model inputs: the comparison work each representation implies. */
static unsigned long cmp_bytes_column, cmp_bytes_span;

static int rec_same(const SpanRec *a, const SpanRec *b)
{
    return a->sid == b->sid && a->inv0 == b->inv0 && a->inv1 == b->inv1
        && a->c0 == b->c0 && a->c1 == b->c1 && a->flags == b->flags;
}

/* Matched by KEY id, not sid. sid is the wall segment and two visible runs can
 * share one: measured at 1,340 frames in 1,789,440 where two spans carry the
 * same sid. Rare, but matching on it silently pairs the wrong spans, so the
 * retained stream needs the key as its identity. It is identity, not geometry,
 * so it sits outside the six bytes the renderer consumes. */
static const SpanRec *find_rec(const Frame *f, uint8_t keyid)
{
    uint8_t i;
    for (i = 0; i < f->n; ++i) if (f->sp[i].keyid == keyid) return &f->sp[i];
    return NULL;
}

static unsigned long n_dup_sid;
/* A span record that CHANGED still says nothing about which of its columns
 * differ. If most columns of a changed span really do change, span granularity
 * loses nothing; if few do, the span form has thrown away resolution the
 * column form had. Measured rather than argued. */
static unsigned long n_chcol_tot, n_chcol_diff;

static void col_state(const SpanRec *s, uint8_t c, uint8_t *hl, uint8_t *hr,
                      uint8_t *bd)
{
    uint8_t n = (uint8_t)(s->c1 - s->c0 + 1u);
    int16_t iq = (int16_t)((int16_t)s->inv0 << 6);
    int16_t step = (int16_t)(((int16_t)s->inv1 - (int16_t)s->inv0)
                             * (int16_t)k_col_recip_q8[n]);
    uint8_t k;
    step = shr_signed(step, 2);
    for (k = s->c0; k < c; ++k) iq = (int16_t)(iq + step);
    *hl = (uint8_t)(clamp_u8i((int16_t)((iq + 32) >> 6), 255u) >> 1);
    *hr = (uint8_t)(clamp_u8i((int16_t)((iq + step + 32) >> 6), 255u) >> 1);
    *bd = (uint8_t)(((c == s->c0 && (s->flags & 1u)) ? 1u : 0u)
                  | ((c == s->c1 && (s->flags & 2u)) ? 2u : 0u));
}

static void measure_frame(const Frame *f)
{
    uint8_t i, j;
    uint8_t seen[TSP_COLS];
    ++n_frames;
    n_spans += f->n;
    ++hist_spans[f->n];
    memset(seen, 0, sizeof seen);
    for (i = 0; i < f->n; ++i) {
        for (j = 0; j < f->n; ++j)
            if (j != i && f->sp[j].sid == f->sp[i].sid) { ++n_dup_sid; break; }
        /* Two boundary events per span: where it starts and where it ends. */
        n_events += 2;
        n_ev_tot += 2;
        if (seen[f->sp[i].c0]) ++n_ev_same_tile; else seen[f->sp[i].c0] = 1;
        if (seen[f->sp[i].c1]) ++n_ev_same_tile; else seen[f->sp[i].c1] = 1;
        n_cols_total += (unsigned long)(f->sp[i].c1 - f->sp[i].c0 + 1);
    }
}

static void compare_frames(const Frame *p, const Frame *c)
{
    uint8_t i;
    unsigned long changed = 0;
    ++n_pairs;
    for (i = 0; i < c->n; ++i) {
        const SpanRec *o = find_rec(p, c->sp[i].keyid);
        int geom, ext, dx;
        cmp_bytes_span += 6;
        if (!o) { ++n_span_appear; ++changed; continue; }
        if (rec_same(o, &c->sp[i])) { ++n_span_same; continue; }
        ++n_span_changed; ++changed;
        geom = (o->inv0 != c->sp[i].inv0 || o->inv1 != c->sp[i].inv1);
        ext = (o->c0 != c->sp[i].c0 || o->c1 != c->sp[i].c1
               || o->flags != c->sp[i].flags);
        if (geom && ext) ++n_both;
        else if (geom) ++n_geom_only;
        else ++n_extent_only;
        n_cols_of_changed += (unsigned long)(c->sp[i].c1 - c->sp[i].c0 + 1);
        {
            uint8_t cc;
            for (cc = c->sp[i].c0; cc <= c->sp[i].c1; ++cc) {
                uint8_t a1, a2, a3, b1, b2, b3;
                ++n_chcol_tot;
                if (cc < o->c0 || cc > o->c1) { ++n_chcol_diff; continue; }
                col_state(&c->sp[i], cc, &a1, &a2, &a3);
                col_state(o, cc, &b1, &b2, &b3);
                if (a1 != b1 || a2 != b2 || a3 != b3) ++n_chcol_diff;
            }
        }
        dx = (int)c->sp[i].c0 - (int)o->c0;
        if (dx < 0) dx = -dx;
        if (dx) {
            ++n_edge_moved;
            edge_move_sum += (unsigned long)dx;
            if ((unsigned long)dx > edge_move_max) edge_move_max = (unsigned long)dx;
        } else ++n_edge_still;
    }
    for (i = 0; i < p->n; ++i)
        if (!find_rec(c, p->sp[i].keyid)) { ++n_span_vanish; ++changed; }
    ++hist_changed[changed > MAXSPAN ? MAXSPAN : changed];
    /* What the retained-COLUMN form would have had to compare: three bytes for
     * every column of every span present in either pose. */
    for (i = 0; i < c->n; ++i)
        cmp_bytes_column += 3u * (unsigned long)(c->sp[i].c1 - c->sp[i].c0 + 1);
}

static unsigned long pct(const unsigned long *h, unsigned n, double q)
{
    unsigned long tot = 0, acc = 0; unsigned i;
    for (i = 0; i < n; ++i) tot += h[i];
    for (i = 0; i < n; ++i) { acc += h[i]; if (acc >= (unsigned long)(q * tot)) return i; }
    return n ? n - 1 : 0;
}

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
    unsigned U = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 1u;
    unsigned frames = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 0) : 240u;
    unsigned yaw_step = (argc > 3) ? (unsigned)strtoul(argv[3], 0, 0) : 64u;
    int only = (argc > 4) ? (int)strtol(argv[4], 0, 0) : -1;
    const char *dump_path = (argc > 5 && argv[5][0]) ? argv[5] : NULL;
    unsigned t, f, gx, gy, yy, i;
    static uint16_t rebuilt[TSP_MAP_CELLS];

    if (!U) U = 1;
    if (!yaw_step) yaw_step = 64u;
    g_tspf_appearance_mode = 0u;
    if (dump_path) {
        g_dump = fopen(dump_path, "w");
        if (!g_dump) { fprintf(stderr, "cannot open %s\n", dump_path); return 1; }
        g_dump_stride = (argc > 6) ? (unsigned long)strtoul(argv[6], 0, 0) : 997ul;
        if (!g_dump_stride) g_dump_stride = 1;
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
                Frame *prev = &g_a, *cur = &g_b, *sw;
                int have_prev = 0;
                tsp_reset(&st);
                st.x_q4 = px; st.y_q4 = py;
                st.yaw = (uint8_t)yy; st.manual = k_regime[t].manual;
                for (f = 0; f < frames; ++f) {
                    unsigned save;
                    tsp_step(&st, k_regime[t].inp[(f / 30u) % 8u]);
                    if ((f % U) != U - 1) continue;
                    build_frame(&st, cur);

                    /* THE GATE: rebuild the frame from the six-byte records
                     * alone and demand equality with the renderer's output. */
                    save = g_touched_count;
                    map_init(rebuilt);
                    for (i = 0; i < cur->n; ++i) replay_span(rebuilt, &cur->sp[i]);
                    g_touched_count = save;
                    if (memcmp(rebuilt, cur->map, sizeof rebuilt)) ++n_exact_fail;

                    measure_frame(cur);
                    if (have_prev) {
                        compare_frames(prev, cur);
                        classify_inversions(prev, cur);
                        dump_pair(prev, cur);
                    }
                    have_prev = 1;
                    sw = prev; prev = cur; cur = sw;
                }
            }
        }
    }

    printf("=== SPARSE SPAN STREAM ===\n");
    printf("U=%u  regime %s  frames %lu  pose pairs %lu\n\n",
           U, only >= 0 ? k_regime[only].name : "ALL", n_frames, n_pairs);

    printf("EXACTNESS GATE - rebuild from the 6-byte records alone\n");
    printf("  frames whose rebuild != the renderer's own output   %lu / %lu\n",
           n_exact_fail, n_frames);
    printf("  %s\n\n", n_exact_fail
           ? "*** NOT EXACT - the record is missing an input ***"
           : "pixel-for-pixel identical - the record is complete");

    printf("STREAM SIZE\n");
    printf("  visible spans/frame     mean %.2f   median %lu   p95 %lu   max %lu\n",
           (double)n_spans / n_frames, pct(hist_spans, MAXSPAN + 1, 0.5),
           pct(hist_spans, MAXSPAN + 1, 0.95), pct(hist_spans, MAXSPAN + 1, 1.0));
    printf("  boundary events/frame   mean %.2f  (two per span)\n",
           (double)n_events / n_frames);
    printf("  events sharing a tile with an earlier one   %.1f%%\n",
           n_ev_tot ? 100.0 * n_ev_same_tile / n_ev_tot : 0.0);
    printf("  column-materializations/frame  %.2f  (what the column form"
           " retains)\n", (double)n_cols_total / n_frames);
    printf("  spans sharing one sid in a frame  %lu  %s\n", n_dup_sid,
           n_dup_sid ? "<- sid is NOT a unique span key" : "(sid is unique)");
    printf("  RETAINED BYTES   span form %.1f   column form %.1f   ratio %.2fx\n",
           6.0 * n_spans / n_frames, 3.0 * n_cols_total / n_frames,
           n_spans ? (3.0 * n_cols_total) / (6.0 * n_spans) : 0.0);

    printf("\nTEMPORAL CHANGE, PER POSE PAIR\n");
    {
        unsigned long tot = n_span_same + n_span_changed + n_span_appear;
        printf("  spans unchanged        %6.2f /update   %.1f%%\n",
               (double)n_span_same / n_pairs,
               tot ? 100.0 * n_span_same / tot : 0.0);
        printf("  spans changed          %6.2f /update   %.1f%%\n",
               (double)n_span_changed / n_pairs,
               tot ? 100.0 * n_span_changed / tot : 0.0);
        printf("  spans appearing        %6.2f /update\n",
               (double)n_span_appear / n_pairs);
        printf("  spans vanishing        %6.2f /update\n",
               (double)n_span_vanish / n_pairs);
        printf("  changed spans/update   median %lu   p95 %lu   max %lu\n",
               pct(hist_changed, MAXSPAN + 1, 0.5),
               pct(hist_changed, MAXSPAN + 1, 0.95),
               pct(hist_changed, MAXSPAN + 1, 1.0));
    }
    printf("  of the changed: geometry only %.1f%%   extent only %.1f%%"
           "   both %.1f%%\n",
           n_span_changed ? 100.0 * n_geom_only / n_span_changed : 0.0,
           n_span_changed ? 100.0 * n_extent_only / n_span_changed : 0.0,
           n_span_changed ? 100.0 * n_both / n_span_changed : 0.0);
    printf("  left edge stationary %.1f%% of changed spans;"
           " when it moves, mean %.2f columns, max %lu\n",
           (n_edge_moved + n_edge_still)
             ? 100.0 * n_edge_still / (n_edge_moved + n_edge_still) : 0.0,
           n_edge_moved ? (double)edge_move_sum / n_edge_moved : 0.0,
           edge_move_max);
    printf("  columns belonging to CHANGED spans  %.2f /update"
           "   of %.2f total\n",
           (double)n_cols_of_changed / n_pairs,
           (double)n_cols_total / n_frames);

    printf("  within a CHANGED span, columns whose own state actually"
           " differs  %.1f%%\n",
           n_chcol_tot ? 100.0 * n_chcol_diff / n_chcol_tot : 0.0);

    printf("\nDRAW-ORDER INVERSIONS - what are they physically?\n");
    if (!inv_pairs) {
        printf("  none observed\n");
    } else {
        printf("  updates with any inversion   %.1f%%   inverted pairs/update %.3f\n",
               100.0 * inv_updates_any / inv_updates,
               (double)inv_pairs / inv_updates);
        printf("  LOAD-BEARING (forcing the old order changes the image)"
               "   %6.2f%%\n", 100.0 * inv_loadbearing / inv_pairs);
        printf("  STREAM-ORDER ONLY (image identical either way)"
               "          %6.2f%%\n", 100.0 * inv_streamonly / inv_pairs);
        printf("   of which the pair does not overlap at all"
               "              %6.2f%%\n", 100.0 * inv_no_overlap / inv_pairs);
        printf("  mechanism, not exclusive:\n");
        printf("    sort key inv_mid genuinely crossed      %6.2f%%\n",
               100.0 * inv_key_crossed / inv_pairs);
        printf("    inv_mid TIED in the previous frame      %6.2f%%\n",
               100.0 * inv_tie_prev / inv_pairs);
        printf("    inv_mid TIED in the current frame       %6.2f%%\n",
               100.0 * inv_tie_cur / inv_pairs);
        printf("    an FOV-clip flag changed                %6.2f%%\n",
               100.0 * inv_clip_changed / inv_pairs);
        printf("    a column extent changed                 %6.2f%%\n",
               100.0 * inv_extent_changed / inv_pairs);
        printf("  overlap width  mean %.2f cols   p95 %lu   max %lu\n",
               (double)inv_overlap_sum / inv_pairs,
               pct(inv_hist_ov, TSP_COLS + 1, 0.95), inv_overlap_max);
    }

    printf("\nCOMPARISON WORK IMPLIED (bytes compared per update)\n");
    printf("  span form    %7.2f\n", (double)cmp_bytes_span / n_pairs);
    printf("  column form  %7.2f\n", (double)cmp_bytes_column / n_pairs);
    printf("  ratio        %7.2fx fewer bytes to compare\n",
           cmp_bytes_span ? (double)cmp_bytes_column / cmp_bytes_span : 0.0);
    printf("  A span record that matches proves ALL its columns match, which\n"
           "  is what turns a per-column sweep into a per-span one.\n");
    if (g_dump) {
        fclose(g_dump);
        fprintf(stderr, "span-stream oracle: %lu pairs dumped (stride %lu)\n",
                g_dump_out, g_dump_stride);
    }
    return n_exact_fail ? 1 : 0;
}
