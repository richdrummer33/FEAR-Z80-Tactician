/*
 * EDGE_PROGJOIN_BAKE: emit REAL compiled edge programs and a REAL rank-dispatch
 * table, sized to what the pose corpus actually reaches.
 *
 * WHY THIS EXISTS
 * ---------------
 * `edge_program_bake.c` and `edge_dispatch_verify.c` compute programs into a
 * stack array, hash them, and throw them away - they are counters, not bakers.
 * Neither contains an fopen. Nothing they describe has ever been consumed by a
 * kernel, so the compiled-edge-program architecture's 65,785 T figure rests on
 * a playback loop that replayed CAPTURED OLD-RENDERER OUTPUT. This tool WRITES
 * THE BYTES, so a Z80 kernel can dispatch into them and play them back.
 *
 * WHAT A PROGRAM IS
 * -----------------
 * For a chunk of up to C consecutive columns of one run-edge, a program is the
 * exact sequence of name-table cells that edge writes, in playback-ready form:
 *
 *     [C+1 bytes: cumulative cell count after each column]
 *     [4 bytes per cell: name-table word (LE), destination delta (LE)]
 *
 * The delta is what the playback loop adds to its cursor after storing a word
 * and one `inc hl`, i.e. next_dest - dest - 1. The LAST cell's delta advances to
 * the first cell of the NEXT chunk, so programs self-chain and the destination
 * cursor is initialised once per run-edge. That is why a C-column program must
 * read C+1 heights.
 *
 * The prefix-sum header lets a short final chunk play a prefix of the same
 * program instead of needing a separate one.
 *
 * HOW A PROGRAM IS SELECTED
 * -------------------------
 * With a = iq + 32 the accumulator and a = 128*H + u,
 *
 *     h_k = H + ((u + k*step) >> 7)
 *
 * so the height sequence depends on u only through which of the thresholds
 * t_k = (-k*step) mod 128, k = 1..C, it has passed. Sorted, those give a RANK
 * in 0..C. The tile also needs H modulo the family's base count M = period/128.
 * Hence      program = T[family][step][H mod M][rank(u)].
 *
 * This bakes exactly that, from the model's OWN formulas - not from captured
 * renderer output. Whether it reproduces the renderer is then a real question,
 * answered by z80_progjoin_bench.py against the renderer's own draw_edge.
 *
 * SCOPE, STATED HONESTLY
 * ----------------------
 * Programs are position-independent, so they carry NO SCREEN CLIPPING, while
 * the renderer's draw_edge clamps rows to the 18-row viewport. A run-edge whose
 * drawn columns leave the viewport, or whose inverse depth hits the 255 clamp,
 * is COUNTED AND EXCLUDED rather than silently mis-baked. Those fractions are
 * reported: they are a real gap in the architecture, not an artefact here.
 *
 * Shade is constant (appearance mode 0 => shade 1) in the benchmarked
 * configuration, so absolute name-table words can be baked. In appearance mode
 * >= 1 shade varies per column with `mid`, and absolute-word baking would need
 * per-shade duplication or a runtime add. Reported, not assumed away.
 *
 *   usage: edge_progjoin_bake <oracle> <C> <pose_limit> [emit_dir]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define MAXC 8
#define MAXCELLS 64
#define NFAM 5

static const int k_famperiod[NFAM] = { 1024, 1024, 1024, 2048, 4096 };
static const char *k_famname[NFAM] = {
    "71-h    FULL top", "72-h    LINTEL/RAISED top", "72+h    FULL/RISER bot",
    "72-h>>1 LINTEL bot", "72+h-h>>2 RAISED bot/RISER top" };
static int fam_M(int fam) { return k_famperiod[fam] / 128; }

static long g_clipok, g_cliptot, g_clipdrop, g_clipall;
static int g_off_h;                     /* height at the last offscreen hit */
static long g_off_fam[NFAM][3];         /* [fam][1=top,2=bottom] exclusions */
static long g_off_hsum[NFAM], g_off_hn[NFAM];
static int fam_is_bottom(int fam) { return (fam == 2 || fam == 3 || fam == 4); }

static int16_t endpoint_of(int fam, int h) {
    switch (fam) {
    case 0: return (int16_t)(TSPF_HORIZON - 1 - h);   /* FULL top: draw_run's tl-- */
    case 1: return (int16_t)(TSPF_HORIZON - h);
    case 2: return (int16_t)(TSPF_HORIZON + h);
    case 3: return (int16_t)(TSPF_HORIZON - (h >> 1));
    default: return (int16_t)(TSPF_HORIZON + h - (h >> 2));
    }
}

/* One column's cells, from the renderer's own formulas.
 * `iq_col` is the accumulator BEFORE the +32, wrapped to int16 exactly as
 * draw_run's `iq=(int16_t)(iq+step)` does. */
static int column_cells(int fam, int16_t iq_col, int16_t step, uint8_t shade,
                        uint8_t bottom, int clip,
                        uint16_t *words, int *rows,
                        int *hit_clamp, int *hit_offscreen) {
    int32_t a = (int32_t)iq_col + 32;
    int32_t rl = a >> 6, rw = (a + step) >> 6;
    uint8_t invl = (uint8_t)clamp_u8i((int16_t)rl, 255u);
    uint8_t invr = (uint8_t)clamp_u8i((int16_t)rw, 255u);
    int hl, hr, r, n = 0;
    int16_t tl, tr;
    int8_t slope, r0, r1;
    if (rl != (int32_t)invl || rw != (int32_t)invr) { *hit_clamp = 1; if (!clip) return -1; }
    hl = invl >> 1; hr = invr >> 1;
    tl = endpoint_of(fam, hl); tr = endpoint_of(fam, hr);
    slope = clamp_s8((int16_t)(tr - tl), -7, 7);
    r0 = row_floor(tl < tr ? tl : tr);
    r1 = row_floor(tl > tr ? tl : tr);
    if (r0 < 0 || r1 >= (int8_t)TSP_ROWS) {
        /* 1 = runs off the TOP of the 144-line viewport, 2 = off the BOTTOM.
         * Both mean the wall is near enough that this edge leaves the screen,
         * which draw_edge handles by clamping the row range. A
         * position-independent program cannot carry that clamp. */
        *hit_offscreen |= (r0 < 0) ? 1 : 0;
        *hit_offscreen |= (r1 >= (int8_t)TSP_ROWS) ? 2 : 0;
        g_off_h = hl;
        if (!clip) return -1;
        if (clip == 1) {
            if (r0 < 0) r0 = 0;
            if (r1 >= (int8_t)TSP_ROWS) r1 = (int8_t)(TSP_ROWS - 1u);
        }
        /* clip == 2: leave the range unclamped, rows may fall outside */
    }
    for (r = r0; r <= r1 && n < MAXCELLS; ++r) {
        words[n] = edge_entry(shade, (int16_t)(tl - ((int16_t)r << 3)), slope, bottom);
        rows[n] = r;
        ++n;
    }
    return n;
}

static void thresholds(int16_t step, int C, int *out) {
    int k, i, j, t[MAXC];
    for (k = 1; k <= C; ++k) {
        int v = (int)((-(int32_t)k * (int32_t)step) % 128);
        if (v < 0) v += 128;
        t[k - 1] = v;
    }
    for (i = 0; i < C; ++i) out[i] = t[i];
    for (i = 1; i < C; ++i) { int v = out[i];
        for (j = i; j > 0 && out[j - 1] > v; --j) out[j] = out[j - 1];
        out[j] = v; }
}
static int rank_of(int u, const int *sorted, int C) {
    int k, r = 0;
    for (k = 0; k < C; ++k) if (u >= sorted[k]) ++r;
    return r;
}

/* `ncols` is how many columns this chunk actually draws; a final chunk draws
 * fewer than C. Only those may cause an exclusion. The C+1'th height is needed
 * ONLY when a next chunk follows, for the chaining delta - building the full C
 * columns unconditionally would exclude run-edges for the geometry of columns
 * past the run's own right edge, which are never drawn. */
static int build_program(int fam, int16_t iq0, int16_t step, uint8_t shade,
                         int ncols, int has_next,
                         uint16_t *words, int32_t *dests, uint8_t *prefix,
                         int *hit_clamp, int *hit_offscreen) {
    int c, n = 0;
    uint8_t bottom = (uint8_t)(fam_is_bottom(fam) ? 1u : 0u);
    prefix[0] = 0;
    for (c = 0; c < ncols; ++c) {
        uint16_t w[MAXCELLS]; int rws[MAXCELLS]; int k, m;
        m = column_cells(fam, (int16_t)(iq0 + (int16_t)(c * step)), step, shade,
                         bottom, 0, w, rws, hit_clamp, hit_offscreen);
        if (m <= 0) return -1;
        for (k = 0; k < m; ++k) {
            if (n >= MAXCELLS) return -1;
            words[n] = w[k];
            dests[n] = (int32_t)rws[k] * (TSP_COLS * 2) + (int32_t)c * 2;
            ++n;
        }
        prefix[c + 1] = (uint8_t)n;
    }
    for (c = ncols + 1; c <= MAXC; ++c) prefix[c] = (uint8_t)n;
    /* The chaining delta is computed ALWAYS, even for the final chunk of a
     * run-edge where it is never used. Otherwise a final chunk bakes a
     * different body than a continuing one for the SAME dispatch key - they
     * differ in exactly that one delta - and the table conflicts. So a
     * C-column program genuinely needs C+1 on-screen columns, including past
     * the run's own right edge. That is an architectural cost, not a bake
     * artefact, and it is what the exclusion rate below prices. */
    (void)has_next;
    {
        uint16_t w[MAXCELLS]; int rws[MAXCELLS]; int m;
        m = column_cells(fam, (int16_t)(iq0 + (int16_t)(ncols * step)), step,
                         shade, bottom, 0, w, rws, hit_clamp, hit_offscreen);
        if (m <= 0) return -1;
        dests[n] = (int32_t)rws[0] * (TSP_COLS * 2) + (int32_t)ncols * 2;
    }
    return n;
}

/* ---------------------- buffers, dedup ---------------------- */
typedef struct { uint8_t *b; size_t n, cap; } Buf;
static void buf_put(Buf *b, const void *p, size_t k) {
    if (b->n + k > b->cap) {
        b->cap = (b->n + k) * 2 + 4096;
        b->b = realloc(b->b, b->cap);
        if (!b->b) { fprintf(stderr, "oom\n"); exit(1); }
    }
    memcpy(b->b + b->n, p, k); b->n += k;
}
#define BBITS 18
#define BSIZE (1u << BBITS)
static uint64_t g_bh[BSIZE]; static uint32_t g_bo[BSIZE]; static unsigned long g_bn;
static uint64_t hash_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull; size_t i;
    for (i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}
static uint32_t body_intern(Buf *bodies, const uint8_t *p, size_t n) {
    uint64_t h = hash_bytes(p, n);
    unsigned i = (unsigned)((h * 1181783497276652981ull) >> (64 - BBITS));
    for (;;) {
        if (!g_bh[i]) {
            uint32_t off = (uint32_t)bodies->n;
            g_bh[i] = h; g_bo[i] = off; ++g_bn;
            buf_put(bodies, p, n);
            return off;
        }
        if (g_bh[i] == h && g_bo[i] + n <= bodies->n
            && !memcmp(bodies->b + g_bo[i], p, n)) return g_bo[i];
        i = (i + 1u) & (BSIZE - 1u);
    }
}

/* ---------------------- main ---------------------- */
static int g_slot_of[4096];             /* step+2048 -> slot, -1 absent */
static int g_slot_step[4096];
static int g_nslots;

int main(int argc, char **argv) {
    const char *oracle = (argc > 1) ? argv[1] : "build/coverage_pose_oracle.txt";
    int C = (argc > 2) ? atoi(argv[2]) : 6;
    long pose_limit = (argc > 3) ? atol(argv[3]) : 0;
    const char *emit = (argc > 4) ? argv[4] : NULL;
    /* Window start, so the corpus can be covered in slices that each fit the
     * Z80's 64 KiB address space. That limit is a harness constraint, not an
     * architectural one - the real table needs banking regardless. */
    long pose_start = (argc > 5) ? atol(argv[5]) : 0;
    long skipped = 0;
    FILE *f;
    long poses = 0, runs = 0, edges = 0, chunks = 0, cells = 0;
    long ex_clamp = 0, ex_off = 0, ok_edges = 0, cases = 0;
    char line[65536];
    Buf bodies = {0}, cases_buf = {0};
    int pass;
    /* per (fam,slot) block of M*(C+1) 16-bit body pointers, lazily allocated */
    uint32_t **block = NULL;
    int32_t **blockiq = NULL;
    char *casefile = NULL;
    FILE *cf = NULL;

    /* The rank dimension is C+2 (ranks 0..C+1) and the emitted stride is a
     * padded 8, so C must leave room. */
    if (C < 1 || C > 6) { fprintf(stderr, "C out of range (1..6)\n"); return 1; }
    for (int i = 0; i < 4096; ++i) g_slot_of[i] = -1;

    /* ---- pass 0: collect the step slots actually reached ---- */
    f = fopen(oracle, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", oracle); return 1; }
    while (fgets(line, sizeof line, f)) {
        char *p = line; int n, i;
        if (pose_limit && poses >= pose_limit) break;
        n = (int)strtol(p, &p, 10);
        if (n <= 0) continue;
        if (skipped < pose_start) { ++skipped; continue; }
        ++poses;
        for (i = 0; i < n; ++i) {
            int v[8], k;
            for (k = 0; k < 8; ++k) v[k] = (int)strtol(p, &p, 10);
            if (v[3] < v[2]) continue;
            if (v[1] >= -2048 && v[1] < 2048 && g_slot_of[v[1] + 2048] < 0) {
                g_slot_of[v[1] + 2048] = g_nslots;
                g_slot_step[g_nslots] = v[1];
                ++g_nslots;
            }
        }
    }
    fclose(f);

    printf("=== EDGE_PROGJOIN_BAKE, C = %d columns per program ===\n", C);
    printf("oracle: %s   poses: %ld   distinct steps: %d\n\n",
           oracle, poses, g_nslots);

    block = calloc((size_t)NFAM * C * g_nslots, sizeof(uint32_t *));
    blockiq = calloc((size_t)NFAM * C * g_nslots, sizeof(int32_t *));
    if (!block || !blockiq) { fprintf(stderr, "oom\n"); return 1; }

    if (emit) {
        casefile = malloc(strlen(emit) + 64);
        sprintf(casefile, "%s/progjoin_cases.txt", emit);
        cf = fopen(casefile, "w");
        if (!cf) { fprintf(stderr, "cannot write %s\n", casefile); return 1; }
    }

    /* ---- pass 1: bake ---- */
    poses = 0; skipped = 0;
    f = fopen(oracle, "r");
    while (fgets(line, sizeof line, f)) {
        char *p = line; int n, i;
        if (pose_limit && poses >= pose_limit) break;
        n = (int)strtol(p, &p, 10);
        if (n <= 0) continue;
        if (skipped < pose_start) { ++skipped; continue; }
        ++poses;
        for (i = 0; i < n; ++i) {
            int iq0 = (int)strtol(p, &p, 10);
            int step = (int)strtol(p, &p, 10);
            int c0 = (int)strtol(p, &p, 10);
            int c1 = (int)strtol(p, &p, 10);
            int prof = (int)strtol(p, &p, 10);
            int lr = (int)strtol(p, &p, 10);
            int rr = (int)strtol(p, &p, 10);
            int sh = (int)strtol(p, &p, 10);
            int ncol, e;
            (void)lr; (void)rr;
            if (c1 < c0) continue;
            ncol = c1 - c0 + 1;
            ++runs;
            for (e = 0; e < 2; ++e) {
                int fam = e == 0
                    ? (prof == TSP_PROFILE_FULL ? 0 : prof == TSP_PROFILE_RISER ? 4 : 1)
                    : (prof == TSP_PROFILE_LINTEL ? 3 : prof == TSP_PROFILE_RAISED ? 4 : 2);
                int M = fam_M(fam), slot = g_slot_of[step + 2048], cs;
                int hit_clamp = 0, hit_off = 0, edge_ok = 1;
                /* stage the whole run-edge, commit only if every chunk bakes */
                uint32_t offs[32]; int ncs = 0, want_of[32], first_dest = -1;
                uint8_t stage[32][MAXC + 1 + 4 * MAXCELLS]; size_t stagelen[32];
                int keybase[32], keyrank[32];
                ++edges;
                for (cs = 0; cs < ncol && edge_ok; cs += C) {
                    int16_t iq = (int16_t)(iq0 + (int16_t)(cs * step));
                    int32_t a = (int32_t)iq + 32;
                    int H = (int)(a >> 7), u = (int)(a & 127);
                    int sorted[MAXC], rank, want = ncol - cs, m, k;
                    uint16_t words[MAXCELLS]; int32_t dests[MAXCELLS];
                    uint8_t prefix[MAXC + 1], *sp;
                    if (want > C) want = C;
                    /* C+1 thresholds, not C. A C-column program's DRAWN cells
                     * need heights h_0..h_C, which C thresholds cover. But the
                     * self-chaining delta also needs the NEXT chunk's starting
                     * row, and that row is set by h_C together with h_{C+1} -
                     * the right endpoint of the lookahead column. So the
                     * selector depends on C+2 heights and needs C+1
                     * thresholds. With C thresholds two chunks that agree on
                     * every drawn cell but disagree on where the next chunk
                     * starts collide on one key: found at fam=0 step=5 base=7
                     * rank=0, iq 3008 vs 2974, differing only in the final
                     * chaining delta. */
                    thresholds((int16_t)step, C + 1, sorted);
                    rank = rank_of(u, sorted, C + 1);
                    m = build_program(fam, iq, (int16_t)step, (uint8_t)sh, want,
                                      cs + want < ncol, words, dests, prefix,
                                      &hit_clamp, &hit_off);
                    ++chunks;
                    if (m < 0) { edge_ok = 0; break; }
                    /* serialise: prefix header then 4-byte cells */
                    sp = stage[ncs];
                    memcpy(sp, prefix, (size_t)C + 1);
                    for (k = 0; k < m; ++k) {
                        int32_t d = dests[k + 1] - dests[k] - 1;
                        sp[C + 1 + 4 * k + 0] = (uint8_t)(words[k] & 0xff);
                        sp[C + 1 + 4 * k + 1] = (uint8_t)(words[k] >> 8);
                        sp[C + 1 + 4 * k + 2] = (uint8_t)(d & 0xff);
                        sp[C + 1 + 4 * k + 3] = (uint8_t)((d >> 8) & 0xff);
                    }
                    stagelen[ncs] = (size_t)C + 1 + 4 * (size_t)m;
                    want_of[ncs] = want;
                    /* M is a power of two and the Z80 dispatcher masks, so the
                     * residue must be the NON-NEGATIVE one. C's % gives a
                     * negative result for a negative accumulator (a = iq+32 can
                     * be < 0), which indexed outside the block and silently
                     * corrupted neighbouring dispatch entries. Mask, exactly as
                     * the kernel does. */
                    keybase[ncs] = H & (M - 1);
                    keyrank[ncs] = rank;
                    if (first_dest < 0) {
                        /* run setup supplies the absolute cursor once per edge */
                        uint16_t w[MAXCELLS]; int rws[MAXCELLS]; int hc = 0, ho = 0;
                        column_cells(fam, iq, (int16_t)step, (uint8_t)sh,
                                     (uint8_t)(fam_is_bottom(fam) ? 1u : 0u), 0,
                                     w, rws, &hc, &ho);
                        first_dest = rws[0] * (TSP_COLS * 2) + c0 * 2;
                    }
                    cells += prefix[want];
                    ++ncs;
                    if (ncs >= 32) { edge_ok = 0; break; }
                }
                if (!edge_ok && hit_off) {
                    /* For each drawn column, compare the RENDERER's clamped
                     * cells against the unclamped model cells filtered to the
                     * viewport. If they match everywhere, clipping is a pure
                     * row-range filter over an unchanged program body. */
                    int c;
                    uint8_t bt = (uint8_t)(fam_is_bottom(fam) ? 1u : 0u);
                    for (c = 0; c < ncol; ++c) {
                        uint16_t wa[MAXCELLS], wb[MAXCELLS];
                        int ra[MAXCELLS], rb[MAXCELLS];
                        int hc = 0, ho = 0, ma, mb, k, j, ok = 1, kept = 0;
                        int16_t iqc = (int16_t)(iq0 + (int16_t)(c * step));
                        ma = column_cells(fam, iqc, (int16_t)step, (uint8_t)sh,
                                          bt, 1, wa, ra, &hc, &ho);
                        hc = ho = 0;
                        mb = column_cells(fam, iqc, (int16_t)step, (uint8_t)sh,
                                          bt, 2, wb, rb, &hc, &ho);
                        if (ma < 0 || mb < 0) continue;
                        for (j = 0; j < mb; ++j)
                            if (rb[j] >= 0 && rb[j] < (int)TSP_ROWS) ++kept;
                        if (kept != ma) ok = 0;
                        else {
                            k = 0;
                            for (j = 0; j < mb && ok; ++j) {
                                if (rb[j] < 0 || rb[j] >= (int)TSP_ROWS) continue;
                                if (rb[j] != ra[k] || wb[j] != wa[k]) ok = 0;
                                ++k;
                            }
                        }
                        ++g_cliptot;
                        g_clipall += mb;
                        g_clipdrop += mb - kept;
                        if (ok) ++g_clipok;
                    }
                }
                if (!edge_ok) {
                    if (hit_off) {
                        ++ex_off;
                        if (hit_off & 1) ++g_off_fam[fam][1];
                        if (hit_off & 2) ++g_off_fam[fam][2];
                        g_off_hsum[fam] += g_off_h; ++g_off_hn[fam];
                    } else ++ex_clamp;
                    continue;
                }
                ++ok_edges;
                if (!emit) continue;
                /* commit bodies and dispatch entries */
                for (int q = 0; q < ncs; ++q) {
                    /* The played column count is part of the key. The published
                     * model T[fam][step][base][rank] does NOT distinguish a full
                     * C-column chunk from a short final one, and they are
                     * different programs - that is a real conflict, hit on
                     * fam=0 step=0 first. Resolving it by always storing the
                     * full C-column body and playing a prefix would instead
                     * require every one of the C columns to be on-screen,
                     * raising the exclusion rate from 18.0% to 24.5%. Keying by
                     * length excludes fewer real run-edges and multiplies the
                     * dispatch table by C. Both costs are reported. */
                    uint32_t bi = ((uint32_t)fam * (uint32_t)C
                                   + (uint32_t)(want_of[q] - 1))
                                  * (uint32_t)g_nslots + (uint32_t)slot;
                    offs[q] = body_intern(&bodies, stage[q], stagelen[q]);
                    if (!block[bi]) {
                        block[bi] = malloc(sizeof(uint32_t) * (size_t)M * (size_t)(C + 2));
                        blockiq[bi] = malloc(sizeof(int32_t) * (size_t)M * (size_t)(C + 2));
                        for (int z = 0; z < M * (C + 2); ++z) block[bi][z] = 0xFFFFFFFFu;
                    }
                    {
                        int idx = keybase[q] * (C + 2) + keyrank[q];
                        if (block[bi][idx] != 0xFFFFFFFFu && block[bi][idx] != offs[q]) {
                            fprintf(stderr, "DISPATCH CONFLICT fam=%d step=%d base=%d "
                                    "rank=%d want=%d\n", fam, step, keybase[q],
                                    keyrank[q], want_of[q]);
                            fprintf(stderr, "  NEW iq(chunk)=%d iq0=%d c0=%d ncol=%d chunk=%d\n",
                                    (int)(int16_t)(iq0 + (int16_t)(q * C * step)),
                                    iq0, c0, ncol, q);
                            fprintf(stderr, "  OLD iq(chunk)=%d\n", blockiq[bi][idx]);
                            fprintf(stderr, "  NEW body (len %zu):", stagelen[q]);
                            for (size_t z = 0; z < stagelen[q]; ++z)
                                fprintf(stderr, " %02x", stage[q][z]);
                            fprintf(stderr, "\n  OLD body at off %u:", block[bi][idx]);
                            {
                                size_t z, L = stagelen[q];
                                for (z = 0; z < L && block[bi][idx] + z < bodies.n; ++z)
                                    fprintf(stderr, " %02x", bodies.b[block[bi][idx] + z]);
                            }
                            fprintf(stderr, "\n");
                            return 2;
                        }
                        block[bi][idx] = offs[q];
                        blockiq[bi][idx] = (int32_t)(int16_t)(iq0 + (int16_t)(q * C * step));
                    }
                }
                /* the case: runtime inputs, then the RENDERER's own expected cells */
                fprintf(cf, "%d %d %d %d %d %d %d %d", fam, step, iq0, c0, ncol,
                        sh, first_dest, ncs);
                for (int q = 0; q < ncs; ++q) fprintf(cf, " %d", want_of[q]);
                {
                    int c, ncell = 0; char tmp[1 << 16]; int tl_ = 0;
                    tmp[0] = 0;
                    for (c = 0; c < ncol; ++c) {
                        uint16_t w[MAXCELLS]; int rws[MAXCELLS]; int hc = 0, ho = 0, k, m;
                        m = column_cells(fam, (int16_t)(iq0 + (int16_t)(c * step)),
                                         (int16_t)step, (uint8_t)sh,
                                         (uint8_t)(fam_is_bottom(fam) ? 1u : 0u), 1,
                                         w, rws, &hc, &ho);
                        for (k = 0; k < m; ++k) {
                            tl_ += sprintf(tmp + tl_, " %d %d",
                                           rws[k] * (TSP_COLS * 2) + (c0 + c) * 2, w[k]);
                            ++ncell;
                        }
                    }
                    fprintf(cf, " %d%s\n", ncell, tmp);
                }
                ++cases;
            }
        }
    }
    fclose(f);

    printf("run-edges                   %ld\n", edges);
    printf("  fully bakeable            %ld  (%.2f%%)\n",
           ok_edges, 100.0 * ok_edges / (edges ? edges : 1));
    printf("  excluded, row offscreen   %ld  (%.2f%%)  <- programs carry no clipping\n",
           ex_off, 100.0 * ex_off / (edges ? edges : 1));
    printf("  excluded, invdepth clamp  %ld  (%.2f%%)\n",
           ex_clamp, 100.0 * ex_clamp / (edges ? edges : 1));
    printf("\nIS CLIPPING JUST \"DROP THE OUT-OF-RANGE CELLS\"?\n");
    printf("  clipped-equals-filtered columns   %ld of %ld", g_clipok, g_cliptot);
    printf("%s\n", g_clipok == g_cliptot ? "   YES, EXACTLY" : "   NO");
    printf("  cells dropped per clipped column  %.2f of %.2f\n",
           g_cliptot ? (double)g_clipdrop / (double)g_cliptot : 0.0,
           g_cliptot ? (double)g_clipall / (double)g_cliptot : 0.0);
    printf("  If YES, the fallback is not a separate renderer: the SAME baked\n"
           "  program plays, with leading/trailing cells skipped.\n");

    printf("\nWHY THE EXCLUDED ONES ARE EXCLUDED (offscreen breakdown)\n");
    printf("  %-34s %8s %8s %8s\n", "family", "off TOP", "off BOT", "mean h");
    for (int fm = 0; fm < NFAM; ++fm) {
        if (!g_off_hn[fm]) continue;
        printf("  %-34s %8ld %8ld %8.1f\n", k_famname[fm],
               g_off_fam[fm][1], g_off_fam[fm][2],
               (double)g_off_hsum[fm] / (double)g_off_hn[fm]);
    }
    printf("  (h is the wall half-height in scanlines; the viewport is 144\n"
           "   lines centred on y=71.5, so h > ~72 puts an edge off-screen.\n"
           "   Large h = the wall is CLOSE. This is near-wall view-frustum\n"
           "   clipping, NOT occlusion - occlusion is handled separately by\n"
           "   the depth sort plus near-to-far ownership.)\n\n");
    printf("dispatch chunks             %ld  (%.2f per pose)\n",
           chunks, (double)chunks / (poses ? poses : 1));
    printf("edge cells                  %ld  (%.2f per pose)\n",
           cells, (double)cells / (poses ? poses : 1));

    if (!emit) { printf("\n(census only; pass an emit dir to write tables)\n"); return 0; }
    fclose(cf);

    /* ---- serialise the dispatch structures ---- */
    {
        Buf blocks = {0}, desc = {0}, thresh = {0}, stepmap = {0};
        size_t nblocks = 0;
        uint8_t *sm = calloc(4096, 1);
        uint8_t *ds = calloc((size_t)g_nslots * 32 * 2, 1);
        char path[512]; FILE *o;
        for (int i = 0; i < 4096; ++i) sm[i] = 0xFF;
        for (int i = 0; i < 4096; ++i)
            if (g_slot_of[i] >= 0) {
                if (g_slot_of[i] > 254) { fprintf(stderr,
                    "too many step slots (%d) for a byte map - lower the pose limit\n",
                    g_nslots); return 3; }
                sm[i] = (uint8_t)g_slot_of[i];
            }
        /* Strides are padded to powers of two so the Z80 dispatcher indexes
         * with shifts instead of multiplies: thresholds 8 bytes per slot,
         * descriptors 32 entries per slot, dispatch rows 8 entries per base.
         * Costs 14% more block bytes and buys a multiply-free index. */
        for (int s = 0; s < g_nslots; ++s) {
            int sorted[MAXC]; uint8_t t[8];
            thresholds((int16_t)g_slot_step[s], C + 1, sorted);
            memset(t, 0xFF, sizeof t);
            for (int k = 0; k < C + 1; ++k) t[k] = (uint8_t)sorted[k];
            buf_put(&thresh, t, 8);
        }
        for (int fm = 0; fm < NFAM; ++fm)
          for (int wl = 0; wl < C; ++wl)
            for (int s = 0; s < g_nslots; ++s) {
                uint32_t bi = ((uint32_t)fm * (uint32_t)C + (uint32_t)wl)
                              * (uint32_t)g_nslots + (uint32_t)s;
                if (!block[bi]) continue;
                uint32_t base = (uint32_t)blocks.n;
                int M = fam_M(fm);
                size_t di = ((size_t)s * 32u + (size_t)fm * (size_t)C
                             + (size_t)wl) * 2u;
                ds[di + 0] = (uint8_t)(base & 0xff);
                ds[di + 1] = (uint8_t)(base >> 8);
                for (int b = 0; b < M; ++b)
                    for (int rk = 0; rk < 8; ++rk) {
                        uint32_t v = (rk <= C + 1) ? block[bi][b * (C + 2) + rk]
                                                   : 0xFFFFFFFFu;
                        uint8_t w[2];
                        if (v == 0xFFFFFFFFu) v = 0xFFFF;   /* unreachable */
                        w[0] = (uint8_t)(v & 0xff); w[1] = (uint8_t)((v >> 8) & 0xff);
                        buf_put(&blocks, w, 2);
                    }
                ++nblocks;
            }
        buf_put(&stepmap, sm, 4096);
        buf_put(&desc, ds, (size_t)g_nslots * 32 * 2);

#define WRITE(nm, bufp) \
        do { sprintf(path, "%s/progjoin_" nm ".bin", emit); \
             o = fopen(path, "wb"); if (!o) { perror(path); return 4; } \
             fwrite((bufp).b, 1, (bufp).n, o); fclose(o); } while (0)
        WRITE("stepmap", stepmap);
        WRITE("desc", desc);
        WRITE("thresh", thresh);
        WRITE("blocks", blocks);
        WRITE("bodies", bodies);
#undef WRITE

        printf("\n--- EMITTED (bytes actually written to disk) ---\n");
        printf("  stepmap  %8zu   step -> slot, 4096 entries\n", stepmap.n);
        printf("  desc     %8zu   slot*32 + fam*C + (len-1) -> block base\n", desc.n);
        printf("  thresh   %8zu   %d slots x 8 (C+1=%d used)\n",
               thresh.n, g_nslots, C + 1);
        printf("  blocks   %8zu   %zu blocks of M x 8 program pointers\n",
               blocks.n, nblocks);
        printf("  bodies   %8zu   %lu distinct programs\n", bodies.n, g_bn);
        printf("  TOTAL    %8zu\n",
               stepmap.n + desc.n + thresh.n + blocks.n + bodies.n);
        printf("  cases    %ld run-edges written to %s\n", cases, casefile);

        sprintf(path, "%s/progjoin_manifest.txt", emit);
        o = fopen(path, "w");
        fprintf(o, "C %d\nNFAM %d\nNSLOTS %d\n", C, NFAM, g_nslots);
        fprintf(o, "stepmap %zu\ndesc %zu\nthresh %zu\nblocks %zu\nbodies %zu\n",
                stepmap.n, desc.n, thresh.n, blocks.n, bodies.n);
        for (int fm = 0; fm < NFAM; ++fm) fprintf(o, "M %d %d\n", fm, fam_M(fm));
        fprintf(o, "poses %ld\nedges %ld\nbakeable %ld\noffscreen %ld\nclamped %ld\n",
                poses, edges, ok_edges, ex_off, ex_clamp);
        fprintf(o, "chunks %ld\ncells %ld\n", chunks, cells);
        fclose(o);
    }
    for (int fm = 0; fm < NFAM; ++fm)
        printf("  family %d M=%-3d %s\n", fm, fam_M(fm), k_famname[fm]);
    return 0;
}
