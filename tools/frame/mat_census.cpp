/* Materializer workload census.
 *
 * p_fill owns about a fifth of render because it is called ~146 times an update.
 * The question that decides what to do about it is not "how fast is the loop"
 * but "why is it entered that often", and there are three different answers with
 * three different fixes:
 *
 *   most iterations hidden behind nearer geometry  -> enumerate from the mask
 *   most visible but unchanged since last frame    -> temporal reuse
 *   most genuinely changing, in wide same-tile runs -> row-span materialization
 *
 * So this counts them rather than guessing. It samples at exported labels and
 * reads materializer state through read-only alias symbols; no instruction is
 * added to the ROM, so the build measured is cycle-identical to the shipping one.
 *
 * Probe points
 *   _tsp_polar_p_fill   once per interior row, immediately after the ownership
 *                       test. The Z flag carries the answer, r_row the row, and
 *                       the saved HL the name-table word, which is everything
 *                       needed to classify the iteration.
 *   _tsp_polar_p_span   once per column that survives the whole-span ownership
 *                       test, with the 18-bit unclaimed mask live.
 *
 * Frame level it reads g_map itself, so horizontal same-tile runs and
 * frame-to-frame identity are measured on the real output, not inferred.
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "GearsystemCore.h"
#include "Memory.h"
#include "Processor.h"

bool g_mcp_stdio_mode = false;

static bool find_symbol(const char* path, const char* want, unsigned& out) {
    std::ifstream f(path); std::string line;
    if (!f) return false;
    while (std::getline(f, line)) {
        const char* p = line.c_str();
        while (*p == ' ' || *p == '\t') ++p;
        if (std::strncmp(p, "DEF ", 4)) continue;
        unsigned a = 0; char sym[256] = {0};
        if (std::sscanf(p, "DEF %255s %x", sym, &a) == 2 && !std::strcmp(sym, want)) { out = a; return true; }
    }
    return false;
}
static unsigned need(const char* path, const char* n) {
    unsigned a = 0;
    if (!find_symbol(path, n, a)) { std::fprintf(stderr, "symbol %s missing\n", n); std::exit(3); }
    return a;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s rom.gg rom.noi label [frames=120] [warmup=8]\n", argv[0]); return 2; }
    const char* rom = argv[1]; const char* noi = argv[2]; const char* label = argv[3];
    const unsigned target = argc > 4 ? (unsigned)std::strtoul(argv[4], nullptr, 0) : 120u;
    const unsigned warmup = argc > 5 ? (unsigned)std::strtoul(argv[5], nullptr, 0) : 8u;
    /* Optional per-frame name-table digest. Any change to the materializer has
     * to leave g_map byte-identical, and a digest stream compared between two
     * builds is the cheapest form of that gate that still catches a one-cell
     * difference in one frame out of a hundred. */
    const char* hashp = argc > 6 ? argv[6] : nullptr;
    FILE* hf = hashp ? std::fopen(hashp, "w") : nullptr;

    const u16 P_FILL = (u16)need(noi, "_tsp_polar_p_fill");
    const u16 P_SPAN = (u16)need(noi, "_tsp_polar_p_span");
    const u16 S_ROW  = (u16)need(noi, "_tsp_probe_row");
    const u16 S_TILE = (u16)need(noi, "_tsp_probe_full_tile");
    const u16 S_U0   = (u16)need(noi, "_tsp_probe_unclaimed0");
    const u16 S_FF   = (u16)need(noi, "_tsp_probe_fill_first");
    const u16 S_MAP  = (u16)need(noi, "_g_map");
    const u16 S_LOOP = (u16)need(noi, "_g_ts_loop_count");
    const u16 S_COL  = (u16)need(noi, "_g_polar_mat_col");
    const u16 S_SHD  = (u16)need(noi, "_g_polar_mat_shade");
    const u16 S_BRD  = (u16)need(noi, "_g_polar_mat_border");
    const u16 S_TL   = (u16)need(noi, "_g_polar_mat_top_l");
    const u16 S_TR   = (u16)need(noi, "_g_polar_mat_top_r");
    const u16 S_TMIN = (u16)need(noi, "_tsp_probe_top_min");
    const u16 S_TMAX = (u16)need(noi, "_tsp_probe_top_max");
    const u16 S_BMIN = (u16)need(noi, "_tsp_probe_bot_min");
    const u16 S_BMAX = (u16)need(noi, "_tsp_probe_bot_max");

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if (!core.LoadROM(rom)) { std::fprintf(stderr, "LoadROM failed\n"); return 4; }
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN * GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN * 4);
    std::vector<s16> audio(16384); int samples = 0;
    GearsystemCore::GS_Debug_Run dbg{};
    dbg.step_debugger = true; dbg.stop_on_breakpoint = false;
    dbg.stop_on_run_to_breakpoint = false; dbg.stop_on_irq = false;
    Memory* mem = core.GetMemory();
    Processor* cpu = core.GetProcessor();

    /* p_fill iteration outcomes */
    uint64_t it_total = 0, it_claimed = 0, it_equal = 0, it_changed = 0;
    /* spans and their interiors */
    uint64_t spans = 0, span_full_open = 0, span_partial = 0, span_none = 0;
    uint64_t run_count = 0;                 /* contiguous unclaimed runs in interiors */
    std::vector<uint64_t> height_hist(20, 0);
    /* per-column descriptor identity between consecutive frames */
    uint64_t desc_total = 0, desc_same = 0, desc_same_q = 0;
    /* Two keys, because the difference between them is the whole question.
     * PIXEL is the raw endpoint pair: a one-pixel wobble changes it even when no
     * tile changes. QUANTISED is what the materializer actually produces -- the
     * tile rows and the tile word -- so it only differs when the output differs.
     * If temporal reuse is worth anything, it is on the quantised key. */
    struct Desc { uint8_t shade, border, tmin, tmax, bmin, bmax, tile; uint16_t tl, tr; bool live; };
    std::vector<Desc> prev_desc(24), cur_desc(24);
    for (auto& d : prev_desc) d.live = false;
    /* Frame-to-frame symmetric difference of the name table. This is the ground
     * truth the whole temporal argument rests on: the number of cells that
     * ACTUALLY have to change. Everything the materializer does beyond this is,
     * by definition, work spent discovering that nothing happened. It needs no
     * probes -- g_map is read directly -- so it cannot be wrong about the ROM.
     *
     * Changed cells are also classified by where they sit in their column, because
     * the shape decides the representation. Changes only at the ends of a column's
     * occupied range are a boundary that moved, and an interval-delta renderer
     * touches one or two rows. Changes in the middle mean the tile word itself
     * changed, and the whole interval has to be rewritten. */
    uint64_t dcells = 0, dcols = 0, dframes = 0;
    uint64_t cls_boundary = 0, cls_interior = 0, cls_mixed = 0, cls_whole = 0;
    std::vector<uint64_t> dcell_hist(64, 0), dcol_hist(24, 0);
    std::vector<uint64_t> run_hist(20, 0), runlen_hist(20, 0);
    std::vector<uint16_t> prev_map(18 * 20, 0xFFFF);
    bool have_prev_map = false;
    /* horizontal same-tile runs in the finished name table */
    uint64_t hruns = 0, hcells = 0;
    std::vector<uint64_t> hrun_hist(24, 0);

    unsigned frames = 0, last_loop = 0xFFFFu; bool counting = false;
    uint64_t steps = 0; const uint64_t limit = 4000000000ull;
    /* per-span row-claim pattern, flushed when the span ends */
    std::vector<uint8_t> pattern;

    auto flush_span = [&]() {
        if (pattern.empty()) return;
        if (pattern.size() < height_hist.size()) ++height_hist[pattern.size()];
        uint64_t runs = 0; bool in = false; size_t open = 0;
        for (uint8_t v : pattern) { if (v) { open++; if (!in) { ++runs; in = true; } } else in = false; }
        run_count += runs;
        if (open == pattern.size()) ++span_full_open;
        else if (open == 0) ++span_none;
        else ++span_partial;
        pattern.clear();
    };

    while (frames < target && steps < limit) {
        samples = 0;
        core.RunToVBlank(fb.data(), audio.data(), &samples, &dbg, false);
        ++steps;
        const u16 pc = cpu->GetState()->PC->GetValue();

        if (counting && pc == P_SPAN) {
            flush_span();
            ++spans;
            ++desc_total;
            const uint8_t col = mem->DebugRetrieve(S_COL);
            Desc d{ mem->DebugRetrieve(S_SHD), mem->DebugRetrieve(S_BRD),
                    mem->DebugRetrieve(S_TMIN), mem->DebugRetrieve(S_TMAX),
                    mem->DebugRetrieve(S_BMIN), mem->DebugRetrieve(S_BMAX),
                    mem->DebugRetrieve(S_TILE),
                    (uint16_t)(mem->DebugRetrieve(S_TL) | (mem->DebugRetrieve((u16)(S_TL + 1)) << 8)),
                    (uint16_t)(mem->DebugRetrieve(S_TR) | (mem->DebugRetrieve((u16)(S_TR + 1)) << 8)), true };
            if (col < cur_desc.size()) {
                cur_desc[col] = d;
                const Desc& p = prev_desc[col];
                if (p.live && p.shade == d.shade && p.border == d.border && p.tl == d.tl && p.tr == d.tr)
                    ++desc_same;
                if (p.live && p.shade == d.shade && p.border == d.border && p.tmin == d.tmin &&
                    p.tmax == d.tmax && p.bmin == d.bmin && p.bmax == d.bmax && p.tile == d.tile)
                    ++desc_same_q;
            }
        }
        if (counting && pc == P_FILL) {
            ++it_total;
            /* Z set means the row was already claimed by nearer geometry. */
            const bool claimed = (cpu->GetState()->AF->GetLow() & 0x40) != 0;
            pattern.push_back(claimed ? 0 : 1);
            if (claimed) { ++it_claimed; }
            else {
                /* saved HL is on the stack: the loop pushes it around the test */
                const u16 sp = cpu->GetState()->SP->GetValue();
                const u16 hl = (u16)(mem->DebugRetrieve(sp) | (mem->DebugRetrieve((u16)(sp + 1)) << 8));
                const uint8_t want = mem->DebugRetrieve(S_TILE);
                if (mem->DebugRetrieve(hl) == want && mem->DebugRetrieve((u16)(hl + 1)) == 0) ++it_equal;
                else ++it_changed;
            }
        }

        const unsigned lc = mem->DebugRetrieve(S_LOOP);
        if (lc != last_loop) {
            if (last_loop != 0xFFFFu) {
                if (counting) {
                    flush_span();
                    /* horizontal same-tile runs across the finished name table */
                    for (int r = 0; r < 18; ++r) {
                        int len = 0; uint16_t prevw = 0xFFFF;
                        for (int c = 0; c < 20; ++c) {
                            const u16 a = (u16)(S_MAP + r * 40 + c * 2);
                            const uint16_t w = (uint16_t)(mem->DebugRetrieve(a) | (mem->DebugRetrieve((u16)(a + 1)) << 8));
                            ++hcells;
                            if (c && w == prevw) ++len;
                            else { if (len && len < (int)hrun_hist.size()) ++hrun_hist[len]; if (len) ++hruns; len = 1; }
                            prevw = w;
                        }
                        if (len && len < (int)hrun_hist.size()) ++hrun_hist[len];
                        if (len) ++hruns;
                    }
                    {   /* read the name table once, use it for both analyses */
                        std::vector<uint16_t> cur_v(18 * 20);
                        for (int r = 0; r < 18; ++r) for (int c = 0; c < 20; ++c) {
                            const u16 a = (u16)(S_MAP + r * 40 + c * 2);
                            cur_v[r * 20 + c] = (uint16_t)(mem->DebugRetrieve(a) |
                                              (mem->DebugRetrieve((u16)(a + 1)) << 8));
                        }
                        if (have_prev_map) {
                            ++dframes;
                            uint64_t fd = 0, fc = 0;
                            for (int c = 0; c < 20; ++c) {
                                int first = -1, last = -1, n = 0;
                                for (int r = 0; r < 18; ++r)
                                    if (cur_v[r * 20 + c] != prev_map[r * 20 + c]) {
                                        if (first < 0) first = r; last = r; ++n;
                                    }
                                if (!n) continue;
                                ++fc; fd += n;
                                /* How many contiguous RUNS of changed cells, and how
                                 * long are they? A single run is one boundary moving.
                                 * TWO short runs is a symmetric FULL wall whose top and
                                 * bottom edges both moved -- which an earlier, cruder
                                 * classifier called "scattered" and wrote off, when it
                                 * is exactly the case interval-delta handles best,
                                 * because the materializer already treats top and
                                 * bottom as separate mirrored edges. */
                                int runs = 0, longest = 0, cur = 0;
                                for (int r = 0; r < 18; ++r) {
                                    if (cur_v[r * 20 + c] != prev_map[r * 20 + c]) {
                                        if (!cur) ++runs;
                                        if (++cur > longest) longest = cur;
                                    } else cur = 0;
                                }
                                if (runs < (int)run_hist.size()) ++run_hist[runs];
                                if (longest < (int)runlen_hist.size()) ++runlen_hist[longest];
                                if (n >= 17) ++cls_whole;
                                else if (runs <= 2 && longest <= 3) ++cls_boundary;
                                else if (runs <= 2) ++cls_interior;
                                else ++cls_mixed;
                            }
                            dcells += fd; dcols += fc;
                            if (fd < dcell_hist.size()) ++dcell_hist[fd];
                            if (fc < dcol_hist.size()) ++dcol_hist[fc];
                        }
                        prev_map.swap(cur_v);
                        have_prev_map = true;
                    }
                    if (hf) {
                        uint64_t h = 1469598103934665603ull;
                        for (int a = 0; a < 18 * 40; ++a) {
                            h ^= mem->DebugRetrieve((u16)(S_MAP + a)); h *= 1099511628211ull;
                        }
                        std::fprintf(hf, "%u %016llx\n", frames, (unsigned long long)h);
                    }
                    prev_desc = cur_desc;
                    for (auto& d : cur_desc) d.live = false;
                    ++frames;
                }
                if (!counting && ++frames >= warmup) { counting = true; frames = 0;
                    it_total = it_claimed = it_equal = it_changed = 0;
                    spans = span_full_open = span_partial = span_none = 0;
                    run_count = desc_total = desc_same = desc_same_q = hruns = hcells = 0;
                    std::fill(height_hist.begin(), height_hist.end(), 0);
                    std::fill(hrun_hist.begin(), hrun_hist.end(), 0); }
            }
            last_loop = lc;
        }
    }

    const double F = frames ? (double)frames : 1.0;
    std::printf("materializer census [%s] over %u loop iterations\n\n", label, frames);
    std::printf("  p_fill interior-row iterations: %llu total, %.1f per update\n",
                (unsigned long long)it_total, it_total / F);
    const double T = it_total ? (double)it_total : 1.0;
    std::printf("    already claimed by nearer geometry   %10llu  %6.2f%%\n",
                (unsigned long long)it_claimed, 100.0 * it_claimed / T);
    std::printf("    visible, name table already correct  %10llu  %6.2f%%\n",
                (unsigned long long)it_equal, 100.0 * it_equal / T);
    std::printf("    visible and actually changes g_map   %10llu  %6.2f%%\n",
                (unsigned long long)it_changed, 100.0 * it_changed / T);
    std::printf("    (a dirty mark is raised for each change, so %.1f a update)\n\n",
                it_changed / F);

    std::printf("  spans reaching the interior: %llu, %.1f a update\n",
                (unsigned long long)spans, spans / F);
    const double S = (span_full_open + span_partial + span_none) ?
                     (double)(span_full_open + span_partial + span_none) : 1.0;
    std::printf("    interior wholly unclaimed (mask is all ones) %6.2f%%\n", 100.0 * span_full_open / S);
    std::printf("    partially occluded                           %6.2f%%\n", 100.0 * span_partial / S);
    std::printf("    wholly occluded (every row wasted)           %6.2f%%\n", 100.0 * span_none / S);
    std::printf("    contiguous unclaimed runs per interior       %6.2f\n", run_count / S);
    std::printf("    interior height:");
    for (size_t i = 1; i < height_hist.size(); ++i) if (height_hist[i])
        std::printf(" %zu:%.1f%%", i, 100.0 * height_hist[i] / S);
    std::printf("\n\n");

    std::printf("  per-column descriptor identical to the previous frame, of %llu columns\n",
                (unsigned long long)desc_total);
    std::printf("    keyed on raw pixel endpoints  %6.2f%%\n",
                desc_total ? 100.0 * desc_same / desc_total : 0.0);
    std::printf("    keyed on the materialized result (tile rows + tile word)  %6.2f%%\n",
                desc_total ? 100.0 * desc_same_q / desc_total : 0.0);
    {
        const double D = dframes ? (double)dframes : 1.0;
        std::printf("  name-table change between consecutive frames (ground truth)\n");
        std::printf("    %.2f cells change a frame, out of 360 (%.2f%%)\n",
                    dcells / D, 100.0 * dcells / (D * 360.0));
        std::printf("    %.2f of 20 columns change a frame; %.2f%% of columns are identical\n",
                    dcols / D, 100.0 * (1.0 - dcols / (D * 20.0)));
        const double C = dcols ? (double)dcols : 1.0;
        std::printf("    changed columns by shape: <=2 runs and <=3 long %5.2f%%,"
                    " <=2 longer runs %5.2f%%, 3+ runs %5.2f%%, whole column %5.2f%%\n",
                    100.0 * cls_boundary / C, 100.0 * cls_interior / C,
                    100.0 * cls_mixed / C, 100.0 * cls_whole / C);
        std::printf("    contiguous runs of changed cells in a changed column:");
        for (size_t i = 1; i < run_hist.size(); ++i) if (run_hist[i])
            std::printf(" %zu:%.1f%%", i, 100.0 * run_hist[i] / C);
        std::printf("\n    longest run:");
        for (size_t i = 1; i < runlen_hist.size(); ++i) if (runlen_hist[i])
            std::printf(" %zu:%.1f%%", i, 100.0 * runlen_hist[i] / C);
        std::printf("\n");
        std::printf("    changed cells in a changed column: %.2f mean\n", dcells / C);
        std::printf("    cells changed per frame:");
        for (size_t i = 0; i < dcell_hist.size(); ++i) if (dcell_hist[i])
            std::printf(" %zu:%.0f%%", i, 100.0 * dcell_hist[i] / D);
        std::printf("\n\n");
    }
    std::printf("  horizontal same-tile runs in the finished name table:\n");
    std::printf("    %llu runs over %llu cells, mean length %.2f\n",
                (unsigned long long)hruns, (unsigned long long)hcells,
                hruns ? (double)hcells / hruns : 0.0);
    if (hf) { std::fclose(hf); std::printf("  name-table digests written to %s\n", hashp); }
    std::printf("    length:");
    for (size_t i = 1; i < hrun_hist.size(); ++i) if (hrun_hist[i])
        std::printf(" %zu:%.1f%%", i, 100.0 * hrun_hist[i] / (double)hruns);
    std::printf("\n");
    return 0;
}
