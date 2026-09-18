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

    static const int NPH = 245;   /* 0 = driver, then li*RACE_NK + kernel + 1 */
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
    if (nmis) {
        u16 kmask = 0, first = 0;
        std::fprintf(stderr, "RACE_FAIL %u cases disagreed; timings withheld\n", nmis);
        if (sym2(argv[2], "_g_race_kmask", "g_race_kmask", kmask)) {
            static const char* KN[13] = {"A0", "A1", "B fix lin", "B fix bin", "B pak lin",
                                         "?5", "?6", "?7",
                                         "DDA C", "BAND C", "PACKED C", "DDA asm", "PACK asm"};
            const unsigned m = rd16(mem, kmask);
            std::fprintf(stderr, "  failing kernels:");
            for (int i = 0; i < 13; ++i) if (m & (1u << i)) std::fprintf(stderr, " %s", KN[i]);
            std::fprintf(stderr, "\n");
        }
        if (sym2(argv[2], "_g_race_first", "g_race_first", first))
            std::fprintf(stderr, "  first A0 failure: want len %u got len %u, byte0 got %u want %u\n",
                         mem->DebugRetrieve(first), mem->DebugRetrieve((u16)(first + 1)),
                         mem->DebugRetrieve((u16)(first + 2)), mem->DebugRetrieve((u16)(first + 3)));
        return 6;
    }

    std::printf("RACE_EXACT %u cases, all eleven kernels byte-identical to the expectation\n", ncases);
    std::printf("completed outer iterations %u, sampled instructions %llu\n",
                done, (unsigned long long)steps);
    std::printf("units are Z80 T-states: Gearsystem accumulates the value RunInstruction\n"
                "returns, which is the instruction's own cycle count.\n");

    const unsigned nlen = 20, nk = 11, ncase = ncases / (nlen * (done ? done : 1));
    const double per = (double)(ncase * (done ? done : 1));
    static int LEN[20]; for (int i = 0; i < 20; ++i) LEN[i] = i + 1;

    std::printf("\nT-states per span, by span length (%u cases each, %u iterations)\n", ncase, done);
    {
        FILE* csv = std::fopen("build/race/per_length.csv", "w");
        if (csv) {
            std::fprintf(csv, "cols,dda_c,band_c,dda_asm,pack_asm,a0,a1,bfl,bfb,bpl\n");
            for (unsigned li = 0; li < nlen; ++li)
                std::fprintf(csv, "%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", LEN[li],
                    acc[li*nk+1]/per, (acc[li*nk+2]+acc[li*nk+3])/per, acc[li*nk+5]/per,
                    acc[li*nk+6]/per, acc[li*nk+7]/per, acc[li*nk+8]/per,
                    acc[li*nk+9]/per, acc[li*nk+10]/per, acc[li*nk+11]/per);
            std::fclose(csv);
            std::printf("per-length timings written to build/race/per_length.csv\n");
        }
    }
    std::printf("\nreference points\n");
    std::printf("  %-8s %10s %10s %10s %10s %10s\n", "columns",
                "DDA C", "BAND C", "DDA asm", "PACK asm", "asm gap");
    for (unsigned li = 0; li < nlen; ++li) {
        double d  = (double)acc[li * nk + 1] / per;
        double b  = (double)(acc[li * nk + 2] + acc[li * nk + 3]) / per;
        double da = (double)acc[li * nk + 5] / per;
        double pa = (double)acc[li * nk + 6] / per;
        std::printf("  %-8d %10.1f %10.1f %10.1f %10.1f %10.1f\n", LEN[li], d, b, da, pa, da - pa);
    }
    std::printf("\nthe selector ladder, whole path charged (bank select, address\n"
                "formation, search, body fetch, replay, chunk chaining)\n");
    std::printf("  %-8s %10s %10s %10s %10s %10s\n", "columns",
                "A0 oracle", "A1 honest", "B fix lin", "B fix bin", "B pak lin");
    for (unsigned li = 0; li < nlen; ++li) {
        std::printf("  %-8d %10.1f %10.1f %10.1f %10.1f %10.1f\n", LEN[li],
                    (double)acc[li * nk + 7] / per, (double)acc[li * nk + 8] / per,
                    (double)acc[li * nk + 9] / per, (double)acc[li * nk + 10] / per,
                    (double)acc[li * nk + 11] / per);
    }
    std::printf("\nspeedup against the hand-written DDA, which is the thing to beat\n");
    std::printf("  %-8s %10s %10s %10s %10s %10s\n", "columns",
                "A0 oracle", "A1 honest", "B fix lin", "B fix bin", "B pak lin");
    for (unsigned li = 0; li < nlen; ++li) {
        double da = (double)acc[li * nk + 5] / per;
        std::printf("  %-8d", LEN[li]);
        for (int k = 7; k <= 11; ++k) {
            double v = (double)acc[li * nk + k] / per;
            std::printf(" %9.2fx", v > 0.0 ? da / v : 0.0);
        }
        std::printf("\n");
    }
    std::printf("\neverything beyond the bare copy: each selector minus the packed-replay\n"
                "floor, which is handed both the stream and its length. This is the\n"
                "search AND the address formation AND the chunk chaining, not naming alone.\n");
    std::printf("  %-8s %10s %10s %10s %10s %10s\n", "columns",
                "A0 oracle", "A1 honest", "B fix lin", "B fix bin", "B pak lin");
    for (unsigned li = 0; li < nlen; ++li) {
        double pa = (double)acc[li * nk + 6] / per;
        std::printf("  %-8d", LEN[li]);
        for (int k = 7; k <= 11; ++k)
            std::printf(" %10.1f", (double)acc[li * nk + k] / per - pa);
        std::printf("\n");
    }
    std::printf("\n  \"asm gap\" is the real selector budget: what a scheme that names a\n");
    std::printf("  stream must cost LESS than, to beat deriving it with the hand DDA.\n");
    std::printf("  The C columns are a shape comparison and overstate that budget.\n");
    std::printf("  Every selector calls its lookup indirectly, which costs about twenty\n");
    std::printf("  T-states a chunk more than a direct call; all five pay it and the DDA\n");
    std::printf("  pays none of it, so the ladder is if anything understated.\n");
    std::printf("\n  driver and comparison overhead, excluded above: %llu T-states\n",
                (unsigned long long)acc[0]);
    std::printf("  PACKED is charged nothing for identifying which stream it needs;\n");
    std::printf("  it is a floor on table-driven replay, not a proposal.\n");
    return 0;
}
