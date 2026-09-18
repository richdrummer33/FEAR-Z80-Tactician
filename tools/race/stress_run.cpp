/* Runner for the on-device selector stress sweep.
 *
 * No per-instruction sampling: this measures nothing, it only has to run a very
 * large number of cases and report whether any of them disagreed. Frames are
 * stepped at full speed until the ROM raises its done marker.
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
static u16 need(const char* path, const char* name) {
    u16 a = 0;
    if (!find_symbol(path, name, a)) { std::fprintf(stderr, "symbol %s missing\n", name); std::exit(3); }
    if (a < 0xC000u || a > 0xDFFFu) { std::fprintf(stderr, "%s outside GG work RAM: %04X\n", name, a); std::exit(3); }
    return a;
}
static uint16_t rd16(Memory* m, u16 a) {
    return (uint16_t)m->DebugRetrieve(a) | ((uint16_t)m->DebugRetrieve((u16)(a + 1u)) << 8);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s rom.gg rom.noi [max_frames]\n", argv[0]); return 2; }
    const unsigned long maxf = argc > 3 ? std::strtoul(argv[3], nullptr, 0) : 400000ul;

    const u16 s_done = need(argv[2], "_g_st_done"), s_bad = need(argv[2], "_g_st_bad");
    const u16 s_skip = need(argv[2], "_g_st_skip");
    const u16 s_lo = need(argv[2], "_g_st_cases_lo"), s_hi = need(argv[2], "_g_st_cases_hi");
    const u16 s_si = need(argv[2], "_g_st_step_i");
    const u16 s_bstep = need(argv[2], "_g_st_badstep"), s_bph = need(argv[2], "_g_st_badphase");
    const u16 s_blen = need(argv[2], "_g_st_badlen");
    const u16 s_bnw = need(argv[2], "_g_st_badnw"), s_bng = need(argv[2], "_g_st_badng");

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if (!core.LoadROM(argv[1])) { std::fprintf(stderr, "LoadROM failed\n"); return 4; }
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN * GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN * 4);
    std::vector<s16> audio(16384); int samples = 0;
    Memory* mem = core.GetMemory();

    unsigned long frames = 0;
    while (frames < maxf && rd16(mem, s_done) != 0xD01Eu) {
        samples = 0;
        core.RunToVBlank(fb.data(), audio.data(), &samples, nullptr, false);
        ++frames;
    }
    const unsigned long cases = (unsigned long)rd16(mem, s_lo) + 65536ul * rd16(mem, s_hi);
    const unsigned bad = rd16(mem, s_bad), skip = rd16(mem, s_skip);
    if (rd16(mem, s_done) != 0xD01Eu) {
        std::fprintf(stderr, "STRESS_INCOMPLETE stopped at step index %u after %lu frames, "
                             "%lu cases, %u mismatches\n",
                     rd16(mem, s_si), frames, cases, bad);
        return 5;
    }
    std::printf("frames %lu, step indices swept %u\n", frames, rd16(mem, s_si) + 1u);
    std::printf("cases compared %lu, skipped %u (span cannot fit the depth domain)\n", cases, skip);
    if (bad) {
        std::printf("STRESS_FAIL %u mismatches; first at step %d phase %u cols %u "
                    "(dda len %u, selector len %u)\n",
                    bad, (int16_t)rd16(mem, s_bstep), rd16(mem, s_bph),
                    mem->DebugRetrieve(s_blen), mem->DebugRetrieve(s_bnw),
                    mem->DebugRetrieve(s_bng));
        return 6;
    }
    std::printf("STRESS_OK %lu cases, hand DDA and full-domain selector byte-identical\n", cases);
    return 0;
}
