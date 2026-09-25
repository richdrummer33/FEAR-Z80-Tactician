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
static uint16_t rd16(Memory* mem, u16 a) {
    return (uint16_t)mem->DebugRetrieve(a) | ((uint16_t)mem->DebugRetrieve((u16)(a + 1u)) << 8);
}
static uint64_t fnv1a64(Memory* mem, u16 a, unsigned n) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < n; ++i) {
        h ^= (uint64_t)mem->DebugRetrieve((u16)(a + i));
        h *= 1099511628211ull;
    }
    return h;
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
    const char* map_dump_path = argc > 6 ? argv[6] : nullptr;
    const char* mixed_event_dump_path = argc > 7 ? argv[7] : nullptr;

    u16 s_phase = 0, s_loop = 0;
    if (!find_symbol(noi, "_g_ts_prof_phase", s_phase) && !find_symbol(noi, "g_ts_prof_phase", s_phase)) {
        std::fprintf(stderr, "no _g_ts_prof_phase: build with POLAR_PROFILE_HOOKS=1\n"); return 3;
    }
    if (!find_symbol(noi, "_g_ts_loop_count", s_loop) && !find_symbol(noi, "g_ts_loop_count", s_loop)) {
        std::fprintf(stderr, "no _g_ts_loop_count\n"); return 3;
    }
    u16 s_state = 0, s_map = 0, s_env_phase = 0;
    u16 s_dirty_min = 0, s_vblank_bursts = 0, s_vblank_missed = 0;
    u16 s_join_count = 0, s_join_max = 0, s_join_sum = 0;
    u16 s_mix_patterns=0, s_mix_patches=0, s_mix_skip=0;
    u16 s_mix_fallback=0, s_mix_unsupported=0, s_mix_any=0;
    u16 s_mix_pending=0, s_mix_events=0, s_mix_overflow=0;
    u16 s_mix_x=0, s_mix_left=0, s_mix_right=0, s_mix_vid=0;
    const bool have_state = find_symbol(noi, "_g_state", s_state) || find_symbol(noi, "g_state", s_state);
    const bool have_map = find_symbol(noi, "_g_map", s_map) || find_symbol(noi, "g_map", s_map);
    const bool have_dirty_min =
        find_symbol(noi, "_g_polar_nt_row_min", s_dirty_min) || find_symbol(noi, "g_polar_nt_row_min", s_dirty_min);
    const bool have_vblank_stats =
        (find_symbol(noi, "_g_ts_vblank_bursts", s_vblank_bursts) || find_symbol(noi, "g_ts_vblank_bursts", s_vblank_bursts)) &&
        (find_symbol(noi, "_g_ts_vblank_missed", s_vblank_missed) || find_symbol(noi, "g_ts_vblank_missed", s_vblank_missed));
    const bool have_env_phase =
        find_symbol(noi, "_g_tspf_env_phase", s_env_phase) || find_symbol(noi, "g_tspf_env_phase", s_env_phase);
    const bool have_join_anchor =
        (find_symbol(noi, "_g_tspf_join_anchor_count", s_join_count) || find_symbol(noi, "g_tspf_join_anchor_count", s_join_count)) &&
        (find_symbol(noi, "_g_tspf_join_anchor_max_px", s_join_max) || find_symbol(noi, "g_tspf_join_anchor_max_px", s_join_max)) &&
        (find_symbol(noi, "_g_tspf_join_anchor_sum_px", s_join_sum) || find_symbol(noi, "g_tspf_join_anchor_sum_px", s_join_sum));
    const bool have_mixed_diag =
        (find_symbol(noi, "_g_tspf_mixed_last_patterns", s_mix_patterns) || find_symbol(noi, "g_tspf_mixed_last_patterns", s_mix_patterns)) &&
        (find_symbol(noi, "_g_tspf_mixed_last_patches", s_mix_patches) || find_symbol(noi, "g_tspf_mixed_last_patches", s_mix_patches)) &&
        (find_symbol(noi, "_g_tspf_mixed_skip_reason", s_mix_skip) || find_symbol(noi, "g_tspf_mixed_skip_reason", s_mix_skip)) &&
        (find_symbol(noi, "_g_tspf_mixed_local_fallbacks", s_mix_fallback) || find_symbol(noi, "g_tspf_mixed_local_fallbacks", s_mix_fallback)) &&
        (find_symbol(noi, "_g_tspf_mixed_unsupported_tiles", s_mix_unsupported) || find_symbol(noi, "g_tspf_mixed_unsupported_tiles", s_mix_unsupported)) &&
        (find_symbol(noi, "_g_tspf_mixed_any", s_mix_any) || find_symbol(noi, "g_tspf_mixed_any", s_mix_any)) &&
        (find_symbol(noi, "_g_tspf_mixed_patterns_pending", s_mix_pending) || find_symbol(noi, "g_tspf_mixed_patterns_pending", s_mix_pending)) &&
        (find_symbol(noi, "_g_tspf_mixed_event_count", s_mix_events) || find_symbol(noi, "g_tspf_mixed_event_count", s_mix_events)) &&
        (find_symbol(noi, "_g_tspf_mixed_event_overflow", s_mix_overflow) || find_symbol(noi, "g_tspf_mixed_event_overflow", s_mix_overflow));
    const bool have_mixed_events =
        have_mixed_diag &&
        (find_symbol(noi, "_g_tspf_mixed_event_x", s_mix_x) || find_symbol(noi, "g_tspf_mixed_event_x", s_mix_x)) &&
        (find_symbol(noi, "_g_tspf_mixed_event_left", s_mix_left) || find_symbol(noi, "g_tspf_mixed_event_left", s_mix_left)) &&
        (find_symbol(noi, "_g_tspf_mixed_event_right", s_mix_right) || find_symbol(noi, "g_tspf_mixed_event_right", s_mix_right)) &&
        (find_symbol(noi, "_g_tspf_mixed_event_vid", s_mix_vid) || find_symbol(noi, "g_tspf_mixed_event_vid", s_mix_vid));

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
    static const unsigned ENV_PHASE_COUNT = 9;
    static const char* ENV_PHASE_NAME[ENV_PHASE_COUNT] = {
        "env_idle", "env_fetch", "env_focus", "env_walk",
        "env_draw", "env_ret_end", "env_nt_end",
        "env_mixed_prepare", "env_mixed_apply"
    };
    struct Frame {
        uint64_t ph[6]; uint64_t grp[G_NGROUP]; uint64_t envph[ENV_PHASE_COUNT]; uint64_t total;
        int16_t x_q4, y_q4, z_q4; uint8_t yaw;
        uint64_t map_fnv64;
        uint8_t dirty_rows_pending;
        uint16_t vblank_bursts, vblank_missed;
        uint8_t join_anchor_count, join_anchor_max_px;
        uint16_t join_anchor_sum_px;
        uint8_t mix_patterns,mix_patches,mix_skip,mix_fallback,mix_unsupported;
        uint8_t mix_any,mix_pending,mix_events,mix_overflow;
        uint8_t mix_x[32],mix_left[32],mix_right[32],mix_vid[32];
    };
    std::vector<Frame> frames;
    std::vector<std::vector<uint8_t>> map_snaps;
    unsigned pending_by_row[18] = {0}, pending_run[18] = {0}, pending_run_max[18] = {0};
    Frame cur{}; std::memset(&cur, 0, sizeof cur);
    uint64_t prev = core.GetMasterClockCycles();
    unsigned seen_loops = 0, last_loop = 0xFFFFu;
    bool loop_started = false;
    uint16_t last_vblank_bursts = have_vblank_stats ? rd16(mem, s_vblank_bursts) : 0u;
    uint16_t last_vblank_missed = have_vblank_stats ? rd16(mem, s_vblank_missed) : 0u;
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
        const unsigned lc = mem->DebugRetrieve(s_loop);
        if(!loop_started){
            if(ph!=1u){
                prev=now;
                continue;
            }
            loop_started=true;
            last_loop=lc;
            std::memset(&cur,0,sizeof cur);
            prev=now;
            continue;
        }
        const u16 pc = cpu->GetState()->PC->GetValue();
        if (ph < 6) cur.ph[ph] += dt;
        if (ph == 2) {
            cur.grp[group_of_t(pc, seen_loops >= warmup ? dt : 0u)] += dt;
            if (have_env_phase) {
                const uint8_t ep=mem->DebugRetrieve(s_env_phase);
                if (ep<ENV_PHASE_COUNT) cur.envph[ep]+=dt;
            }
        }
        cur.total += dt;

        if (lc != last_loop) {
            if (last_loop != 0xFFFFu) {
                /* Snapshot the completed logical UPDATE here, not a VBlank.
                 * Two renderers with different speed then compare the same
                 * deterministic input/update number rather than different
                 * simulation states reached at the same display refresh. */
                if (have_state) {
                    cur.x_q4 = (int16_t)rd16(mem, s_state + 0u);
                    cur.y_q4 = (int16_t)rd16(mem, s_state + 2u);
                    cur.z_q4 = (int16_t)rd16(mem, s_state + 4u);
                    cur.yaw = mem->DebugRetrieve((u16)(s_state + 6u));
                }
                if (have_map) cur.map_fnv64 = fnv1a64(mem, s_map, 20u * 18u * 2u);
                if (have_dirty_min) {
                    uint8_t pending = 0u;
                    for (unsigned dr = 0; dr < 18u; ++dr)
                        if (mem->DebugRetrieve((u16)(s_dirty_min + dr)) != 0xffu) ++pending;
                    cur.dirty_rows_pending = pending;
                }
                if (have_join_anchor) {
                    cur.join_anchor_count = mem->DebugRetrieve(s_join_count);
                    cur.join_anchor_max_px = mem->DebugRetrieve(s_join_max);
                    cur.join_anchor_sum_px = rd16(mem, s_join_sum);
                }
                if (have_vblank_stats) {
                    const uint16_t vb = rd16(mem, s_vblank_bursts);
                    const uint16_t vm = rd16(mem, s_vblank_missed);
                    cur.vblank_bursts = (uint16_t)(vb - last_vblank_bursts);
                    cur.vblank_missed = (uint16_t)(vm - last_vblank_missed);
                    last_vblank_bursts = vb;
                    last_vblank_missed = vm;
                }
                if (have_mixed_diag) {
                    cur.mix_patterns=mem->DebugRetrieve(s_mix_patterns);
                    cur.mix_patches=mem->DebugRetrieve(s_mix_patches);
                    cur.mix_skip=mem->DebugRetrieve(s_mix_skip);
                    cur.mix_fallback=mem->DebugRetrieve(s_mix_fallback);
                    cur.mix_unsupported=mem->DebugRetrieve(s_mix_unsupported);
                    cur.mix_any=mem->DebugRetrieve(s_mix_any);
                    cur.mix_pending=mem->DebugRetrieve(s_mix_pending);
                    cur.mix_events=mem->DebugRetrieve(s_mix_events);
                    cur.mix_overflow=mem->DebugRetrieve(s_mix_overflow);
                    if(have_mixed_events){
                        if(cur.mix_events>32u)cur.mix_events=32u;
                        for(unsigned mi=0;mi<cur.mix_events;++mi){
                            cur.mix_x[mi]=mem->DebugRetrieve((u16)(s_mix_x+mi));
                            cur.mix_left[mi]=mem->DebugRetrieve((u16)(s_mix_left+mi));
                            cur.mix_right[mi]=mem->DebugRetrieve((u16)(s_mix_right+mi));
                            cur.mix_vid[mi]=mem->DebugRetrieve((u16)(s_mix_vid+mi));
                        }
                    }
                }
                if (seen_loops >= warmup) {
                    if (have_dirty_min) {
                        for (unsigned dr = 0; dr < 18u; ++dr) {
                            if (mem->DebugRetrieve((u16)(s_dirty_min + dr)) != 0xffu) {
                                ++pending_by_row[dr];
                                ++pending_run[dr];
                                if (pending_run[dr] > pending_run_max[dr]) pending_run_max[dr] = pending_run[dr];
                            } else pending_run[dr] = 0u;
                        }
                    }
                    frames.push_back(cur);
                    if (map_dump_path && have_map) {
                        std::vector<uint8_t> snap(20u * 18u * 2u);
                        for (unsigned mi = 0; mi < snap.size(); ++mi)
                            snap[mi] = mem->DebugRetrieve((u16)(s_map + mi));
                        map_snaps.push_back(std::move(snap));
                    }
                }
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
        if (have_env_phase)
            for (unsigned e=1;e<ENV_PHASE_COUNT;++e) std::fprintf(csv,",%s",ENV_PHASE_NAME[e]);
        std::fprintf(csv, ",x_q4,y_q4,z_q4,yaw,map_fnv64,dirty_rows_pending,vblank_bursts,vblank_missed,join_anchor_count,join_anchor_max_px,join_anchor_sum_px,mixed_patterns,mixed_patches,mixed_skip,mixed_fallback,mixed_unsupported,mixed_any,mixed_pending,mixed_events,mixed_overflow\n");
        for (size_t i = 0; i < frames.size(); ++i) {
            const Frame& f = frames[i];
            std::fprintf(csv, "%zu,%llu,%llu,%llu,%llu,%llu", i,
                (unsigned long long)f.total, (unsigned long long)f.ph[1],
                (unsigned long long)f.ph[2], (unsigned long long)f.ph[3],
                (unsigned long long)f.ph[4]);
            for (int g = 0; g < G_NGROUP; ++g) std::fprintf(csv, ",%llu", (unsigned long long)f.grp[g]);
            if (have_env_phase)
                for (unsigned e=1;e<ENV_PHASE_COUNT;++e) std::fprintf(csv,",%llu",(unsigned long long)f.envph[e]);
            std::fprintf(csv, ",%d,%d,%d,%u,%016llx,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                (int)f.x_q4, (int)f.y_q4, (int)f.z_q4, (unsigned)f.yaw,
                (unsigned long long)f.map_fnv64,
                (unsigned)f.dirty_rows_pending, (unsigned)f.vblank_bursts, (unsigned)f.vblank_missed,
                (unsigned)f.join_anchor_count, (unsigned)f.join_anchor_max_px, (unsigned)f.join_anchor_sum_px,
                (unsigned)f.mix_patterns,(unsigned)f.mix_patches,(unsigned)f.mix_skip,
                (unsigned)f.mix_fallback,(unsigned)f.mix_unsupported,(unsigned)f.mix_any,
                (unsigned)f.mix_pending,(unsigned)f.mix_events,(unsigned)f.mix_overflow);
        }
        std::fclose(csv);
    }
    if (map_dump_path && !map_snaps.empty()) {
        FILE* mf = std::fopen(map_dump_path, "wb");
        if (mf) {
            for (const auto& snap : map_snaps)
                std::fwrite(snap.data(), 1, snap.size(), mf);
            std::fclose(mf);
        }
    }
    if(mixed_event_dump_path && have_mixed_events){
        FILE* ef=std::fopen(mixed_event_dump_path,"w");
        if(ef){
            std::fprintf(ef,"frame,x_q4,y_q4,z_q4,yaw,event_i,x,left,right,vid\n");
            for(size_t fi=0;fi<frames.size();++fi){
                const Frame& f=frames[fi];
                for(unsigned mi=0;mi<f.mix_events;++mi)
                    std::fprintf(ef,"%zu,%d,%d,%d,%u,%u,%u,%u,%u,%u\n",
                        fi,(int)f.x_q4,(int)f.y_q4,(int)f.z_q4,(unsigned)f.yaw,
                        mi,(unsigned)f.mix_x[mi],(unsigned)f.mix_left[mi],
                        (unsigned)f.mix_right[mi],(unsigned)f.mix_vid[mi]);
            }
            std::fclose(ef);
        }
    }

    auto pct = [&](std::vector<uint64_t> v, double q) {
        std::sort(v.begin(), v.end());
        return (double)v[(size_t)(q * (v.size() - 1))];
    };
    std::vector<uint64_t> tot;
    for (auto& f : frames) tot.push_back(f.total);
    double mean = 0; for (auto t : tot) mean += (double)t; mean /= tot.size();

    if (have_dirty_min || have_vblank_stats) {
        double dirty_mean = 0.0, bursts_mean = 0.0, missed_mean = 0.0;
        unsigned dirty_worst = 0u, dirty_zero = 0u, missed_total = 0u;
        for (const auto& f : frames) {
            dirty_mean += f.dirty_rows_pending;
            bursts_mean += f.vblank_bursts;
            missed_mean += f.vblank_missed;
            if (f.dirty_rows_pending > dirty_worst) dirty_worst = f.dirty_rows_pending;
            if (!f.dirty_rows_pending) ++dirty_zero;
            missed_total += f.vblank_missed;
        }
        dirty_mean /= frames.size(); bursts_mean /= frames.size(); missed_mean /= frames.size();
        std::printf("vblank pipeline: bursts/update=%.2f missed/update=%.3f missed_total=%u "
                    "dirty_rows mean=%.2f worst=%u empty=%.1f%%\n",
                    bursts_mean, missed_mean, missed_total, dirty_mean, dirty_worst,
                    100.0 * dirty_zero / frames.size());
        if (have_dirty_min) {
            std::printf("vblank pending by row (pct/max_consecutive_updates):");
            for (unsigned dr=0; dr<18u; ++dr)
                std::printf(" r%u=%.1f/%u", dr, 100.0*pending_by_row[dr]/frames.size(), pending_run_max[dr]);
            std::printf("\n");
        }
    }
    if (have_join_anchor) {
        unsigned joins=0u, sum_px=0u, worst_px=0u, nonzero=0u;
        for (const auto& f : frames) {
            joins += f.join_anchor_count;
            sum_px += f.join_anchor_sum_px;
            if (f.join_anchor_max_px > worst_px) worst_px = f.join_anchor_max_px;
            if (f.join_anchor_sum_px) ++nonzero;
        }
        std::printf("shared-corner anchor: joins/update=%.2f prelock_mean_abs=%.2fpx "
                    "prelock_worst=%upx updates_with_mismatch=%.1f%%\n",
                    (double)joins/frames.size(), joins ? (double)sum_px/joins : 0.0,
                    worst_px, 100.0*nonzero/frames.size());
    }
    if(have_mixed_diag){
        std::vector<uint64_t> pats,patches,events;
        unsigned exact=0u,fallback=0u,unsupported=0u,pending=0u,overflows=0u;
        unsigned skip_hist[8]={0};
        double pm=0.0,wm=0.0,em=0.0;
        for(const auto& f:frames){
            pats.push_back(f.mix_patterns);patches.push_back(f.mix_patches);events.push_back(f.mix_events);
            pm+=f.mix_patterns;wm+=f.mix_patches;em+=f.mix_events;
            if(f.mix_any)++exact;
            fallback+=f.mix_fallback;unsupported+=f.mix_unsupported;
            if(f.mix_pending)++pending;if(f.mix_overflow)++overflows;
            if(f.mix_skip<8u)++skip_hist[f.mix_skip];
        }
        pm/=frames.size();wm/=frames.size();em/=frames.size();
        std::printf("direct mixed: exact_updates=%.1f%% events mean=%.2f p95=%.0f worst=%.0f "
                    "patterns mean=%.2f p95=%.0f worst=%.0f patches mean=%.2f p95=%.0f worst=%.0f "
                    "local_fallback_tiles=%u unsupported_tiles=%u pending_updates=%.1f%% overflows=%u\n",
                    100.0*exact/frames.size(),em,pct(events,.95),pct(events,1.0),
                    pm,pct(pats,.95),pct(pats,1.0),wm,pct(patches,.95),pct(patches,1.0),
                    fallback,unsupported,100.0*pending/frames.size(),overflows);
        std::printf("direct mixed skips: none=%u eye=%u appearance=%u event_overflow=%u pending=%u bank_reuse=%u other=%u\n",
                    skip_hist[0],skip_hist[1],skip_hist[2],skip_hist[3],skip_hist[4],skip_hist[5],
                    skip_hist[6]+skip_hist[7]);
    }
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
    if(have_env_phase){
        std::printf("\n  envelope phase     %10s %10s %10s %10s   share\n","mean","p50","p95","worst");
        for(unsigned e=1;e<ENV_PHASE_COUNT;++e){
            std::vector<uint64_t> v;double m=0.0;
            for(const auto& f:frames){v.push_back(f.envph[e]);m+=(double)f.envph[e];}
            m/=v.size();
            if(m<1.0)continue;
            std::printf("  %-17s %10.0f %10.0f %10.0f %10.0f  %5.2f%%\n",
                        ENV_PHASE_NAME[e],m,pct(v,.50),pct(v,.95),pct(v,1.0),100.0*m/mean);
        }
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
