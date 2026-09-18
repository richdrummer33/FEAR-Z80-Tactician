#ifndef RACE_KERNELS_H
#define RACE_KERNELS_H
/* The three kernels, shared verbatim between the Game Gear ROM and the host
 * checker, so that what is debugged on the host is exactly what is timed on the
 * Z80. Keeping them in the ROM only meant every correctness question cost a full
 * build-and-emulate cycle. */
#include <stdint.h>
#include "race_data.h"

#define MAXMV 40
#define CHUNK 6
/* kernels timed per span length; the profiler decodes phases with this */
#define RACE_NK 11

/* sampled per instruction by the profiler to attribute master cycles */

/* ---- phase 1: arithmetic DDA ------------------------------------------- */
static uint8_t dda_span(int16_t iq, int16_t step, uint8_t ncols, uint8_t *out)
{
    int16_t a = (int16_t)(iq + 32);
    uint8_t n = 0, c;
    int8_t r0, r1, r2;
    /* rows for the first two column edges */
    r0 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
    a = (int16_t)(a + step);
    r1 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
    for (c = 0; c < ncols; ++c) {
        int8_t lo = r0 < r1 ? r0 : r1, hi = r0 < r1 ? r1 : r0, nd, lo1;
        for (nd = (int8_t)(hi - lo); nd > 0; --nd) out[n++] = 0;
        if (c + 1u == ncols) { out[n++] = 6; break; }
        a = (int16_t)(a + step);
        r2 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
        lo1 = r1 < r2 ? r1 : r2;
        out[n++] = (uint8_t)(1 - (int8_t)(lo1 - hi));
        r0 = r1; r1 = r2;
    }
    return n;
}

/* ---- phases 2 and 3: the band transducer -------------------------------- */
/* Per run: the eight phases at which a six-column behaviour can change. */
static uint16_t g_edge[8];
static uint8_t g_prog[8][CHUNK];   /* memoised six-column programs, packed */
static uint8_t g_prog_have;        /* one valid bit per band */

static void band_setup(int16_t step)
{
    uint8_t c;
    int16_t acc = 0;
    g_prog_have = 0;
    for (c = 0; c < 8; ++c) { g_edge[c] = (uint16_t)(acc & 1023); acc = (int16_t)(acc - step); }
}
/* Band index: the number of edges at or below this phase, minus one.
 *
 * The earlier analysis called this "a rank among eight computed phases", which
 * was a hand-wave and not a valid index: counting edges within half a turn
 * behind the phase puts two DIFFERENT bands on the same number, so a memoised
 * program gets replayed for a chunk it does not describe. That is exactly what
 * the race caught on the second chunk of every multi-chunk span.
 *
 * Counting edges at or below the phase is a correct interval index, and there is
 * no wrap case to handle because the c = 0 edge is always at phase 0: -0*step is
 * 0 for every step. Coincident edges (step 0 collapses all eight onto 0) simply
 * land every phase in the same band, which is right. */
static uint8_t band_of(uint16_t phase)
{
    uint8_t c, r = 0;
    for (c = 0; c < 8; ++c) if (g_edge[c] <= phase) ++r;
    return (uint8_t)(r - 1u);
}
/* the six-column program for this chunk, packed one byte per column as
   ndown*8 + (-jump); computed once per band and then replayed */
static void band_build(int16_t a0, int16_t step, uint8_t *p)
{
    int16_t a = a0;
    uint8_t c;
    int8_t r0, r1, r2;
    r0 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
    a = (int16_t)(a + step);
    r1 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
    for (c = 0; c < CHUNK; ++c) {
        int8_t lo = r0 < r1 ? r0 : r1, hi = r0 < r1 ? r1 : r0, lo1;
        a = (int16_t)(a + step);
        r2 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
        lo1 = r1 < r2 ? r1 : r2;
        p[c] = (uint8_t)(((uint8_t)(hi - lo) << 3) | (uint8_t)(hi - lo1));
        r0 = r1; r1 = r2;
    }
}
static uint8_t band_span(int16_t iq, int16_t step, uint8_t ncols, uint8_t *out)
{
    uint8_t n = 0, left = ncols, c;
    int16_t a = (int16_t)(iq + 32);
    while (left) {
        uint8_t want = left > CHUNK ? CHUNK : left, b, *p;
        uint8_t bit;
        b = band_of((uint16_t)(a & 1023));
        bit = (uint8_t)(1u << (b & 7u));
        p = g_prog[b & 7u];
        if (!(g_prog_have & bit)) { band_build(a, step, p); g_prog_have |= bit; }
        for (c = 0; c < want; ++c) {
            uint8_t v = p[c], nd = (uint8_t)(v >> 3);
            while (nd--) out[n++] = 0;
            if (left - c == 1u) { out[n++] = 6; break; }
            out[n++] = (uint8_t)(1u + (v & 7u));
        }
        if (left <= CHUNK) break;
        left = (uint8_t)(left - CHUNK);
        a = (int16_t)(a + (int16_t)(CHUNK * step));
    }
    return n;
}

/* ---- phase 4: packed replay -------------------------------------------- */
static uint8_t pack_span(const uint8_t *src, uint8_t mlen, uint8_t *out)
{
    uint8_t i;
    for (i = 0; i < mlen; ++i) out[i] = src[i];
    return mlen;
}


/* ---- phases 5..9: the selector ladder ----------------------------------- */
/* Every selector below answers the SAME question in the same shape: given the
 * ten-bit DDA phase, name the six-column body, then replay it. They differ only
 * in how the body is named, so the difference between any two of them is the
 * naming cost and nothing else.
 *
 * The replay is a straight copy of the body's move stream, because the packed
 * comparator already established that a copy is the floor for emitting moves.
 * A chunk that does not end the run copies the whole six-column stream; a chunk
 * that ends the run copies the prefix up to that column's downs and then writes
 * the run terminator in place of the jump byte. Those prefix lengths are baked
 * per body, one byte each, so nothing is counted at run time.
 *
 *   A0   the ideal oracle: a dense one-byte-per-state table indexed by the step
 *        ordinal and the exact phase, with the ordinal supplied free. It is not
 *        a proposal -- it is the lower bound any naming scheme is racing.
 *   A1   the same table, but paying the real step-to-ordinal conversion and
 *        address formation. A0 and A1 bracket what "just look it up" costs.
 *   B    the exact-step phase-interval selectors: eight fixed slots scanned
 *        linearly, eight fixed slots searched in three compares, and the packed
 *        variable-length record scanned linearly.
 */
#include "selector_tables.h"

#define SEL_LK_A0   0
#define SEL_LK_FLIN 1
#define SEL_LK_FBIN 2
#define SEL_LK_PAK  3

static const uint8_t *sel_a0_row(uint8_t ord)
{
    return ord < 16u ? gg_sel_a0_0 + (uint16_t)ord * 1024u
                     : gg_sel_a0_1 + (uint16_t)(ord - 16u) * 1024u;
}
/* the step-to-ordinal map A1 must walk: a page index on the step's high byte,
 * then a two-byte entry on its low byte */
static uint16_t sel_a1_ord(int16_t step)
{
    uint8_t pg = gg_sel_a1hi[(uint16_t)step >> 8];
    const uint8_t *p = gg_sel_a1lo + (uint16_t)pg * 512u + ((uint16_t)step & 255u) * 2u;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
/* thresholds are stored high byte first, so the scan compares the high byte
 * before it needs the low one and never has to park a byte in a register */
static uint8_t sel_thr_le(const uint8_t *r, uint16_t ph)
{
    uint8_t phh = (uint8_t)(ph >> 8);
    if (r[0] < phh) return 1;
    if (r[0] > phh) return 0;
    return r[1] <= (uint8_t)ph;
}
static uint8_t sel_lookup(int mode, uint8_t ord, uint16_t ph)
{
    if (mode == SEL_LK_A0) return sel_a0_row(ord)[ph];
    if (mode == SEL_LK_FLIN) {
        const uint8_t *r = gg_sel_fix + (uint16_t)ord * 24u + 21u;
        /* slot zero's threshold is zero, so the walk always terminates and
         * needs no counter; padded slots hold 0xFFFF and are stepped over */
        while (!sel_thr_le(r, ph)) r -= 3;
        return r[2];
    }
    if (mode == SEL_LK_FBIN) {
        const uint8_t *b = gg_sel_fix + (uint16_t)ord * 24u, *r = b + 12;
        if (!sel_thr_le(r, ph)) r -= 12;
        r += 6; if (!sel_thr_le(r, ph)) r -= 6;
        r += 3; if (!sel_thr_le(r, ph)) r -= 3;
        return r[2];
    }
    {
        uint16_t off = (uint16_t)gg_sel_poff[ord * 2u] | ((uint16_t)gg_sel_poff[ord * 2u + 1u] << 8);
        const uint8_t *r = gg_sel_pblob + off;
        while (!sel_thr_le(r, ph)) r += 3;
        return r[2];
    }
}
static uint8_t sel_span(int mode, uint8_t ord, int16_t iq, int16_t step,
                        uint8_t ncols, uint8_t *out)
{
    uint8_t n = 0, left = ncols;
    int16_t a = (int16_t)(iq + 32);
    for (;;) {
        uint16_t ph = (uint16_t)(a & 1023);
        uint8_t b = sel_lookup(mode, ord, ph), i;
        const uint8_t *s = gg_sel_bstream +
            ((uint16_t)gg_sel_bptr[b * 2u] | ((uint16_t)gg_sel_bptr[b * 2u + 1u] << 8));
        if (left > CHUNK) {
            uint8_t len = (uint8_t)(gg_sel_bpre[b * 8u + 5u] + 1u);
            for (i = 0; i < len; ++i) out[n++] = s[i];
            left = (uint8_t)(left - CHUNK);
            a = (int16_t)(a + (int16_t)(CHUNK * step));
        } else {
            uint8_t len = gg_sel_bpre[b * 8u + left - 1u];
            for (i = 0; i < len; ++i) out[n++] = s[i];
            out[n++] = 6;
            return n;
        }
    }
}

#endif
