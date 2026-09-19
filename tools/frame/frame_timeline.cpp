/* Per-frame T-state timeline for the whole game loop.
 *
 * The existing function profiler gives per-function totals across a run, which
 * answers "what does a frame cost on average" and nothing about spikes. A frame
 * that is 40% over budget once a second is a different engineering problem from
 * one that is 5% over every frame, and the mean cannot tell them apart.
 *
 * So this segments the trace into loop iterations and keeps a full per-frame
 * cost vector: the four coarse loop stages the ROM already marks (input and
 * motion, render, vsync, VRAM upload) plus a breakdown of the render stage by
 * PC range, grouped into the subsystems worth attacking separately.
 *
 * Output is a per-frame CSV and a percentile summary. Mean, p50, p95, p99 and
 * worst, because the budget question is about the tail.
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

/* Game Gear NTSC: 3.579545 MHz, 59.92 Hz. One frame of Z80 budget. */
static const double GG_HZ = 3579545.0;
static const double FRAME_T_60 = GG_HZ / 59.92;

static bool find_symbol(const char* path, const char* want, u16& addr) {
    std::ifstream f(path); std::string line;
    if (!f) return false;
    while (std::getline(f, line)) {
        const char* p = line.c_str();
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == ';') continue;
        unsigned bank = 0, a = 0; char sym[256] = {0};
        if (std::strncmp(p, "DEF ", 4) == 0) {
            if (std::sscanf(p, "DEF %255s %x", sym, &a) == 2 && !std::strcmp(sym, want)) { addr = (u16)a; return true; }
            continue;
        }
        if (std::sscanf(p, "%x:%x %255s", &bank, &a, sym) == 3 && !std::strcmp(sym, want)) { addr = (u16)a; return true; }
    }
    return false;
}
/* Every code symbol, address-ordered, so ranges follow the CURRENT link.
 *
 * The renderer is -autobank'd, so tsp_polar_render and its C helpers live in a
 * banked bank at 0x8000-0xBFFF. An earlier version of this filter stopped at
 * 0x8000 and silently dumped every one of them into "other", which made the
 * render breakdown useless. Symbols below 0x0100 are hardware registers, not
 * code, and bank thunks are not functions either. */
static std::vector<std::pair<u16, std::string>> all_symbols(const char* path, unsigned want_bank) {
    /* Banked symbols carry a bank-tagged address (bank << 16 | addr), so the
     * renderer's own C functions do not look like 16-bit addresses at all. Take
     * the fixed bank plus exactly the bank the renderer lives in; other banks
     * would alias into the same 0x8000 window and mis-attribute. */
    std::vector<std::pair<u16, std::string>> v;
    std::ifstream f(path); std::string line;
    while (std::getline(f, line)) {
        const char* p = line.c_str();
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == ';') continue;
        unsigned a = 0; char sym[256] = {0};
        if (std::strncmp(p, "DEF ", 4) == 0) {
            if (std::sscanf(p, "DEF %255s %x", sym, &a) != 2) continue;
            const unsigned bank = a >> 16, addr = a & 0xFFFFu;
            if (bank != 0 && bank != want_bank) continue;
            a = addr;
            if (a < 0x0100u || a >= 0xC000u) continue;
            const std::string n(sym);
            if (n.rfind("b_", 0) == 0 || n.rfind("___bank", 0) == 0 || n.rfind("l_", 0) == 0) continue;
            /* SDCC -debug emits a statement label A$module$NNNN and a source
             * label C$file$line$... for every C statement. They share an
             * address with the function entry and sort ahead of it, so keeping
             * them meant every C function was represented by a line label with
             * no function name in it -- and landed in "other". Dropping them
             * lets each function's own range cover its whole body. */
            if (n.rfind("A$", 0) == 0 || n.rfind("C$", 0) == 0 || n.rfind("G$", 0) == 0) continue;
            v.push_back({(u16)a, n});
        }
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end(),
        [](const auto& x, const auto& y){ return x.first == y.first; }), v.end());
    return v;
}

/* Which subsystem a renderer symbol belongs to. The groups are chosen so that
 * each one is a thing you could go and optimise independently. */
enum Group { G_PROJ = 0, G_GEOM, G_MAT, G_RET, G_PATCH, G_NT, G_HELP, G_OTHER, G_NGROUP };
static const char* GNAME[G_NGROUP] = {
    "projection/setup", "geometry walk", "materializer", "retained gate",
    "boundary patch", "nametable/VRAM", "arith helpers", "other render"
};
static Group classify(const std::string& raw) {
    /* SDCC writes C statics as Fmodule$name$0_0$0, so match on the bare name;
     * otherwise "project_key" hides behind its module prefix. */
    std::string s = raw;
    size_t d1 = s.find('$');
    if (d1 != std::string::npos) {
        size_t d2 = s.find('$', d1 + 1);
        s = s.substr(d1 + 1, d2 == std::string::npos ? std::string::npos : d2 - d1 - 1);
    }
    auto has = [&](const char* k){ return s.find(k) != std::string::npos; };
    /* The retained swept-boundary bookkeeping is priced as its own group: it
     * is pure overhead on a column it fails to skip, so folding it into the
     * materializer would hide exactly the number that decides whether it is
     * worth keeping. */
    /* These two are probe labels planted inside surface_column_fast, so the
     * range they open is ordinary materializer body, not a new subsystem.
     * Leaving them unclassified quietly drained the materializer into "other". */
    if (has("probe_ret_skip") || has("probe_patch_hit")) return G_MAT;
    if (has("ret_try_patch") || has("ret_patch_fill_run") || has("ret_record_clean"))
        return G_PATCH;
    if (has("ret_column_gate") || has("ret_column_kill") || has("ret_key_cmp") ||
        has("ret_key_put") || has("ret_run_begin") || has("ret_bitmask") ||
        has("ret_begin_frame") || has("ret_end_frame") || has("ret_invalidate"))
        return G_RET;
    /* The tsp_h_ aliases added in rung 18 are materializer helpers. Without
     * them here the group total silently shrinks and two runs stop being
     * comparable, which is exactly the trap the per-function shares fell into. */
    if (has("p_fill") || has("p_span") || has("p_edge") || has("p_cap") ||
        has("p_symbot") || has("p_symtop") || has("surface_column_fast") ||
        has("mark_span_fast") || has("set_span_owned_fast") || has("row_unclaimed_fast") || has("mark_dirty_fast") ||
        has("map_ptr_row_col") || has("full_tile_low") || has("row_floor_hl") ||
        has("prepare_edge") || has("prepare_symfull_edges") ||
        has("draw_symfull_edge_pair")) return G_MAT;
    if (has("profile_half") || has("q6_round_u8")) return G_GEOM;
    if (has("run_geometry_fast") || has("draw_run") || has("draw_edge") ||
        has("draw_full") || has("put_cell") || has("edge_entry") || has("row_floor")) return G_GEOM;
    if (has("polar_nt") || has("upload") || has("map_init") || has("restore_touched")) return G_NT;
    if (has("mul") || has("div") || has("memset") || has("memcpy")) return G_HELP;
    if (has("project_key") || has("project_envelope_span") || has("bearing") || has("angle_x") || has("inv_for_dq4") ||
        has("wall_d_q4") || has("add_key") || has("ratio_q8") || has("signed_q12") ||
        has("screen_depth_plane") || has("insert_run") || has("selector_pass") ||
        has("projection_") || has("inv_at_invd") || has("shade_for") || has("ao_class") ||
        has("depthplane") || has("clamp") || has("shr_signed")) return G_PROJ;
    return G_OTHER;
}

struct Range { u16 lo, hi; Group g; };

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s rom.gg rom.noi out.csv [frames=180] [warmup=8]\n", argv[0]);
        return 2;
    }
    const char* rom = argv[1];
    const char* noi = argv[2];
    const char* csvp = argv[3];
    const unsigned target = argc > 4 ? (unsigned)std::strtoul(argv[4], nullptr, 0) : 180u;
    const unsigned warmup = argc > 5 ? (unsigned)std::strtoul(argv[5], nullptr, 0) : 8u;

    u16 s_phase = 0, s_loop = 0;
    if (!find_symbol(noi, "_g_ts_prof_phase", s_phase) && !find_symbol(noi, "g_ts_prof_phase", s_phase)) {
        std::fprintf(stderr, "no _g_ts_prof_phase: build with POLAR_PROFILE_HOOKS=1\n"); return 3;
    }
    if (!find_symbol(noi, "_g_ts_loop_count", s_loop) && !find_symbol(noi, "g_ts_loop_count", s_loop)) {
        std::fprintf(stderr, "no _g_ts_loop_count\n"); return 3;
    }

    /* PC ranges from the current link, one per fixed-bank symbol */
    unsigned rbank = 0;
    { u16 dummy = 0; std::ifstream f(noi); std::string line;
      while (std::getline(f, line)) {
          unsigned a = 0; char sym[256] = {0};
          const char* q = line.c_str(); while (*q == ' ' || *q == '\t') ++q;
          if (std::strncmp(q, "DEF ", 4)) continue;
          if (std::sscanf(q, "DEF %255s %x", sym, &a) != 2) continue;
          if (!std::strcmp(sym, "_tsp_polar_render")) { rbank = a >> 16; break; }
      }
      (void)dummy; }
    std::printf("renderer code bank: %u\n", rbank);
    auto syms = all_symbols(noi, rbank);
    std::vector<Range> ranges;
    std::vector<std::string> rname;
    /* SDCC -debug emits a line label A$module$NNNN for every C statement, which
     * chops each C function into ranges whose names carry no function at all.
     * Left alone they all fall into "other", which is how a third of render
     * came to have no owner. A line label belongs to the function it sits in,
     * so it inherits the group of the nearest preceding real symbol. */
    for (size_t i = 0; i + 1 < syms.size(); ++i) {
        ranges.push_back({syms[i].first, syms[i + 1].first, classify(syms[i].second)});
        rname.push_back(syms[i].second);
    }
    /* A group that is a third of render and has no owner is a blind spot, not
     * a category. Keep a per-range total so "other" can always be named. */
    std::vector<double> rcost(ranges.size(), 0.0);
    auto group_of_t = [&](u16 pc, unsigned t) -> Group {
        size_t lo = 0, hi = ranges.size();
        while (lo < hi) { size_t m = (lo + hi) / 2;
            if (pc < ranges[m].lo) hi = m;
            else if (pc >= ranges[m].hi) lo = m + 1;
            else { rcost[m] += t; return ranges[m].g; } }
        return G_OTHER;
    };

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if (!core.LoadROM(rom)) { std::fprintf(stderr, "LoadROM failed\n"); return 4; }
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN * GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN * 4);
    std::vector<s16> audio(16384); int samples = 0;
    GearsystemCore::GS_Debug_Run dbg{};
    dbg.step_debugger = true; dbg.stop_on_breakpoint = false;
    dbg.stop_on_run_to_breakpoint = false; dbg.stop_on_irq = false;
    Memory* mem = core.GetMemory();
    Processor* cpu = core.GetProcessor();

    /* phase 1 input+motion, 2 render, 3 vsync, 4 VRAM upload, 5 loop tail */
    struct Frame { uint64_t ph[6]; uint64_t grp[G_NGROUP]; uint64_t total; };
    std::vector<Frame> frames;
    Frame cur{}; std::memset(&cur, 0, sizeof cur);
    uint64_t prev = core.GetMasterClockCycles();
    unsigned seen_loops = 0, last_loop = 0xFFFFu;
    uint64_t steps = 0;
    const uint64_t limit = 6000000000ull;

    while (frames.size() < target && steps < limit) {
        samples = 0;
        core.RunToVBlank(fb.data(), audio.data(), &samples, &dbg, false);
        ++steps;
        const uint64_t now = core.GetMasterClockCycles();
        const uint64_t dt = now - prev;
        prev = now;
        const uint8_t ph = mem->DebugRetrieve(s_phase);
        const u16 pc = cpu->GetState()->PC->GetValue();
        if (ph < 6) cur.ph[ph] += dt;
        if (ph == 2) cur.grp[group_of_t(pc, seen_loops >= warmup ? dt : 0u)] += dt;
        cur.total += dt;

        const unsigned lc = mem->DebugRetrieve(s_loop);
        if (lc != last_loop) {
            if (last_loop != 0xFFFFu) {
                if (seen_loops >= warmup) frames.push_back(cur);
                ++seen_loops;
            }
            std::memset(&cur, 0, sizeof cur);
            last_loop = lc;
        }
    }
    if (frames.empty()) { std::fprintf(stderr, "no complete frames captured\n"); return 5; }

    FILE* csv = std::fopen(csvp, "w");
    if (csv) {
        std::fprintf(csv, "frame,total,input_motion,render,vsync,vram");
        for (int g = 0; g < G_NGROUP; ++g) std::fprintf(csv, ",%s", GNAME[g]);
        std::fprintf(csv, "\n");
        for (size_t i = 0; i < frames.size(); ++i) {
            const Frame& f = frames[i];
            std::fprintf(csv, "%zu,%llu,%llu,%llu,%llu,%llu", i,
                (unsigned long long)f.total, (unsigned long long)f.ph[1],
                (unsigned long long)f.ph[2], (unsigned long long)f.ph[3],
                (unsigned long long)f.ph[4]);
            for (int g = 0; g < G_NGROUP; ++g) std::fprintf(csv, ",%llu", (unsigned long long)f.grp[g]);
            std::fprintf(csv, "\n");
        }
        std::fclose(csv);
    }

    auto pct = [&](std::vector<uint64_t> v, double q) {
        std::sort(v.begin(), v.end());
        return (double)v[(size_t)(q * (v.size() - 1))];
    };
    std::vector<uint64_t> tot;
    for (auto& f : frames) tot.push_back(f.total);
    double mean = 0; for (auto t : tot) mean += (double)t; mean /= tot.size();

    std::printf("frame timeline: %zu frames (warmup %u discarded)\n", frames.size(), warmup);
    std::printf("Game Gear budget at 60 Hz is %.0f T-states a frame, %.0f at 30 Hz\n\n",
                FRAME_T_60, 2 * FRAME_T_60);
    std::printf("  whole loop, T-states per frame\n");
    std::printf("    mean %10.0f   p50 %10.0f   p95 %10.0f   p99 %10.0f   worst %10.0f\n",
                mean, pct(tot, .50), pct(tot, .95), pct(tot, .99), pct(tot, 1.0));
    std::printf("    that is %.2f  60 Hz frames on average, %.2f at the worst\n",
                mean / FRAME_T_60, pct(tot, 1.0) / FRAME_T_60);
    unsigned over60 = 0, over30 = 0;
    for (auto t : tot) { if ((double)t > FRAME_T_60) ++over60; if ((double)t > 2 * FRAME_T_60) ++over30; }
    std::printf("    frames over the 60 Hz budget: %.1f%%   over 30 Hz: %.1f%%\n\n",
                100.0 * over60 / tot.size(), 100.0 * over30 / tot.size());

    static const char* PHN[6] = {"(unmarked)", "input/motion", "render", "vsync", "VRAM upload", "loop tail"};
    std::printf("  loop stage        %10s %10s %10s %10s   share\n", "mean", "p50", "p95", "worst");
    for (int p = 0; p < 6; ++p) {
        std::vector<uint64_t> v; double m = 0;
        for (auto& f : frames) { v.push_back(f.ph[p]); m += (double)f.ph[p]; }
        m /= v.size();
        if (m < 1.0) continue;
        std::printf("  %-17s %10.0f %10.0f %10.0f %10.0f  %5.2f%%\n", PHN[p], m,
                    pct(v, .50), pct(v, .95), pct(v, 1.0), 100.0 * m / mean);
    }
    std::printf("\n  inside render     %10s %10s %10s %10s   share\n", "mean", "p50", "p95", "worst");
    for (int g = 0; g < G_NGROUP; ++g) {
        std::vector<uint64_t> v; double m = 0;
        for (auto& f : frames) { v.push_back(f.grp[g]); m += (double)f.grp[g]; }
        m /= v.size();
        if (m < 1.0) continue;
        std::printf("  %-17s %10.0f %10.0f %10.0f %10.0f  %5.2f%%\n", GNAME[g], m,
                    pct(v, .50), pct(v, .95), pct(v, 1.0), 100.0 * m / mean);
    }
    {
        std::vector<size_t> idx;
        for (size_t i = 0; i < ranges.size(); ++i)
            if (ranges[i].g == G_OTHER && rcost[i] > 0.0) idx.push_back(i);
        std::sort(idx.begin(), idx.end(),
                  [&](size_t a, size_t b){ return rcost[a] > rcost[b]; });
        if (!idx.empty()) {
            std::printf("\n  largest unclassified ranges, T-states a frame\n");
            const double n = (double)(frames.empty() ? 1u : frames.size());
            for (size_t k = 0; k < idx.size() && k < 12; ++k)
                std::printf("    %-40s %9.0f\n", rname[idx[k]].c_str(), rcost[idx[k]] / n);
        }
    }
    std::printf("\n  wrote %s\n", csvp);
    return 0;
}
