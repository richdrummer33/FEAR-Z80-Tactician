/* Cycle adjudicator for the real-tuple replay. Same phase-marker technique as
 * the race, two kernels, and the same refusal to report timings unless every
 * case matched. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "GearsystemCore.h"
#include "Memory.h"

bool g_mcp_stdio_mode = false;

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
static u16 need(const char* path, const char* name) {
    u16 a = 0;
    if (!find_symbol(path, name, a)) { std::fprintf(stderr, "symbol %s missing\n", name); std::exit(3); }
    return a;
}
static uint16_t rd16(Memory* m, u16 a) {
    return (uint16_t)m->DebugRetrieve(a) | ((uint16_t)m->DebugRetrieve((u16)(a + 1u)) << 8);
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s rom.gg rom.noi n_tuples\n", argv[0]); return 2; }
    const unsigned n = (unsigned)std::strtoul(argv[3], nullptr, 0);
    const u16 s_ph = need(argv[2], "_g_cp_phase"), s_done = need(argv[2], "_g_cp_done");
    const u16 s_bad = need(argv[2], "_g_cp_bad"), s_i = need(argv[2], "_g_cp_i");
    const u16 s_biq = need(argv[2], "_g_cp_badiq"), s_bst = need(argv[2], "_g_cp_badstep");
    const u16 s_bc = need(argv[2], "_g_cp_badcols");

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if (!core.LoadROM(argv[1])) { std::fprintf(stderr, "LoadROM failed\n"); return 4; }
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN * GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN * 4);
    std::vector<s16> audio(16384); int samples = 0;
    GearsystemCore::GS_Debug_Run dbg{};
    dbg.step_debugger = true; dbg.stop_on_breakpoint = false;
    dbg.stop_on_run_to_breakpoint = false; dbg.stop_on_irq = false;
    Memory* mem = core.GetMemory();

    uint64_t acc[3] = {0, 0, 0};
    uint64_t prev = core.GetMasterClockCycles();
    uint64_t steps = 0;
    const uint64_t limit = 4000000000ull;
    while (steps < limit && rd16(mem, s_done) != 0xC0B5u) {
        samples = 0;
        core.RunToVBlank(fb.data(), audio.data(), &samples, &dbg, false);
        ++steps;
        const uint64_t now = core.GetMasterClockCycles();
        const uint8_t p = mem->DebugRetrieve(s_ph);
        if (p < 3) acc[p] += now - prev;
        prev = now;
    }
    if (rd16(mem, s_done) != 0xC0B5u) {
        std::fprintf(stderr, "CORPUS_INCOMPLETE stopped at tuple %u\n", rd16(mem, s_i));
        return 5;
    }
    const unsigned bad = rd16(mem, s_bad);
    if (bad) {
        std::fprintf(stderr, "CORPUS_FAIL %u of %u real tuples disagreed; first iq %d step %d cols %u\n",
                     bad, n, (int16_t)rd16(mem, s_biq), (int16_t)rd16(mem, s_bst),
                     mem->DebugRetrieve(s_bc));
        return 6;
    }
    const double d = (double)acc[1] / n, s = (double)acc[2] / n;
    std::printf("CORPUS_EXACT %u real raster tuples, both kernels byte-identical\n", n);
    std::printf("sampled instructions %llu\n", (unsigned long long)steps);
    std::printf("\n  %-26s %12s %12s\n", "", "T-states", "per span");
    std::printf("  %-26s %12llu %12.1f\n", "hand DDA", (unsigned long long)acc[1], d);
    std::printf("  %-26s %12llu %12.1f\n", "B packed, linear", (unsigned long long)acc[2], s);
    std::printf("\n  speedup on the renderer's own workload: %.3fx\n", d / s);
    std::printf("  saving: %.1f T-states per span, %.1f%% of the DDA's cost\n",
                d - s, 100.0 * (d - s) / d);
    std::printf("\n  driver time (tuple fetch and comparison), excluded: %llu T-states\n",
                (unsigned long long)acc[0]);
    return 0;
}
