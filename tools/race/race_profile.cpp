/* Cycle-accurate adjudicator for the Z80 raster-kernel race.
 *
 * The ROM sets g_race_phase around each kernel and this samples that marker per
 * instruction, attributing master clock cycles to whichever phase was live. It
 * is the same technique the PROGJOIN A/B profiler uses, because the Z80 cannot
 * read its own clock.
 *
 * It refuses to report timings until the ROM has proved all three kernels
 * produce byte-identical output against the generated expectation: a fast kernel
 * that is wrong must not be allowed to win.
 */
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
static bool sym2(const char* path, const char* a, const char* b, u16& out) {
    return find_symbol(path, a, out) || find_symbol(path, b, out);
}
static uint16_t rd16(Memory* m, u16 a) {
    return (uint16_t)m->DebugRetrieve(a) | ((uint16_t)m->DebugRetrieve((u16)(a + 1u)) << 8);
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s rom.gg rom.sym target_iterations\n", argv[0]); return 2; }
    const unsigned target = (unsigned)std::strtoul(argv[3], nullptr, 0);

    u16 phase = 0, iter = 0, mism = 0, cases = 0;
    if (!sym2(argv[2], "_g_race_phase", "g_race_phase", phase) ||
        !sym2(argv[2], "_g_race_iter", "g_race_iter", iter) ||
        !sym2(argv[2], "_g_race_mismatch", "g_race_mismatch", mism) ||
        !sym2(argv[2], "_g_race_cases", "g_race_cases", cases)) {
        std::fprintf(stderr, "required symbols missing\n"); return 3;
    }
    for (u16 a : {phase, iter, mism, cases})
        if (a < 0xC000u || a > 0xDFFFu) { std::fprintf(stderr, "symbol outside GG work RAM: %04X\n", a); return 3; }

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if (!core.LoadROM(argv[1])) { std::fprintf(stderr, "LoadROM failed\n"); return 4; }
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN * GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN * 4);
    std::vector<s16> audio(16384); int samples = 0;
    GearsystemCore::GS_Debug_Run dbg{};
    dbg.step_debugger = true; dbg.stop_on_breakpoint = false;
    dbg.stop_on_run_to_breakpoint = false; dbg.stop_on_irq = false;
    Memory* mem = core.GetMemory();

    static const int NPH = 25;   /* 0 = driver, then li*6 + kernel + 1 */
    uint64_t acc[NPH] = {0};
    uint64_t prev = core.GetMasterClockCycles();
    const uint64_t limit = 4000000000ull;
    uint64_t steps = 0;
    unsigned done = 0;

    while (done < target && steps < limit) {
        samples = 0;
        core.RunToVBlank(fb.data(), audio.data(), &samples, &dbg, false);
        ++steps;
        const uint64_t now = core.GetMasterClockCycles();
        const uint8_t p = mem->DebugRetrieve(phase);
        if (p < NPH) acc[p] += now - prev;
        prev = now;
        done = rd16(mem, iter);
    }
    const unsigned nmis = rd16(mem, mism);
    const unsigned ncases = rd16(mem, cases);
    if (!ncases) { std::fprintf(stderr, "RACE_FAIL the ROM never completed a case\n"); return 5; }
    if (nmis) { std::fprintf(stderr, "RACE_FAIL %u cases disagreed; timings withheld\n", nmis); return 6; }

    std::printf("RACE_EXACT %u cases, all five kernels byte-identical to the expectation\n", ncases);
    std::printf("completed outer iterations %u, sampled instructions %llu\n",
                done, (unsigned long long)steps);
    std::printf("units are Z80 T-states: Gearsystem accumulates the value RunInstruction\n"
                "returns, which is the instruction's own cycle count.\n");

    /* the ROM sweeps RACE_NLEN lengths x RACE_NCASE cases per outer iteration */
    const unsigned nlen = 4, nk = 6, ncase = ncases / (nlen * (done ? done : 1));
    const double per = (double)(ncase * (done ? done : 1));
    static const int LEN[4] = {3, 6, 12, 18};

    std::printf("\nT-states per span, by span length (%u cases each, %u iterations)\n",
                ncase, done);
    std::printf("  %-8s %10s %10s %10s %10s %10s %10s\n", "columns",
                "DDA C", "BAND C", "PACKED C", "DDA asm", "PACK asm", "asm gap");
    for (unsigned li = 0; li < nlen; ++li) {
        double d  = (double)acc[li * nk + 1] / per;
        double b  = (double)(acc[li * nk + 2] + acc[li * nk + 3]) / per;
        double pk = (double)acc[li * nk + 4] / per;
        double da = (double)acc[li * nk + 5] / per;
        double pa = (double)acc[li * nk + 6] / per;
        std::printf("  %-8d %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f\n",
                    LEN[li], d, b, pk, da, pa, da - pa);
    }
    std::printf("\n  \"asm gap\" is the real selector budget: what a scheme that names a\n");
    std::printf("  stream must cost LESS than, to beat deriving it with the hand DDA.\n");
    std::printf("  The C columns are a shape comparison and overstate that budget.\n");
    std::printf("\n  driver and comparison overhead, excluded above: %llu T-states\n",
                (unsigned long long)acc[0]);
    std::printf("  PACKED is charged nothing for identifying which stream it needs;\n");
    std::printf("  it is a floor on table-driven replay, not a proposal.\n");
    return 0;
}
