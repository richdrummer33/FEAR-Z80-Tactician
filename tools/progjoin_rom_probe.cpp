#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "GearsystemCore.h"
#include "Memory.h"

bool g_mcp_stdio_mode = false;

static bool find_symbol(const char* path, const char* wanted, u16& addr) {
    std::ifstream f(path); std::string line;
    if (!f) return false;
    while (std::getline(f,line)) {
        unsigned bank=0,a=0; char sym[256]={0};
        if (std::sscanf(line.c_str(),"%x:%x %255s",&bank,&a,sym)==3 && std::strcmp(sym,wanted)==0) { addr=(u16)a; return true; }
        if (std::sscanf(line.c_str(),"%x %255s",&a,sym)==2 && std::strcmp(sym,wanted)==0) { addr=(u16)a; return true; }
    }
    return false;
}
static bool any_symbol(const char* path,const char* a,const char* b,u16& out) {
    return find_symbol(path,a,out)||find_symbol(path,b,out);
}
static uint16_t rd16(Memory* m,u16 a) {
    return (uint16_t)m->DebugRetrieve(a)|((uint16_t)m->DebugRetrieve((u16)(a+1u))<<8);
}
static uint32_t rd32(Memory* m,u16 a) {
    return (uint32_t)rd16(m,a)|((uint32_t)rd16(m,(u16)(a+2u))<<16);
}

int main(int argc,char**argv) {
    if(argc<3){std::fprintf(stderr,"usage: %s probe.gg probe.sym\n",argv[0]);return 2;}
    u16 done=0,fail=0,miss=0,bounds=0,cases=0,last=0,got=0,want=0;
    const char* s=argv[2];
#define NEED(A,B,V) if(!any_symbol(s,A,B,V)){std::fprintf(stderr,"missing symbol %s\n",A);return 3;}
    NEED("_g_pj_probe_done","g_pj_probe_done",done)
    NEED("_g_pj_probe_fail","g_pj_probe_fail",fail)
    NEED("_g_pj_probe_lookup_miss","g_pj_probe_lookup_miss",miss)
    NEED("_g_pj_probe_bounds_fail","g_pj_probe_bounds_fail",bounds)
    NEED("_g_pj_probe_cases_done","g_pj_probe_cases_done",cases)
    NEED("_g_pj_probe_last_case","g_pj_probe_last_case",last)
    NEED("_g_pj_probe_last_hash","g_pj_probe_last_hash",got)
    NEED("_g_pj_probe_expect_hash","g_pj_probe_expect_hash",want)
#undef NEED

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(argv[1])){std::fprintf(stderr,"LoadROM failed\n");return 4;}
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384); int samples=0;
    GearsystemCore::GS_Debug_Run dbg{};
    dbg.step_debugger=true; dbg.stop_on_breakpoint=false; dbg.stop_on_run_to_breakpoint=false; dbg.stop_on_irq=false;
    Memory* mem=core.GetMemory();
    const uint64_t t0=core.GetMasterClockCycles();
    unsigned frames=0;
    while(!mem->DebugRetrieve(done) && frames<120u){
        samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,&dbg,false); ++frames;
    }
    const uint64_t dt=core.GetMasterClockCycles()-t0;
    const unsigned n=rd16(mem,cases), nf=rd16(mem,fail), nm=rd16(mem,miss), nb=rd16(mem,bounds);
    std::printf("GG_PROGJOIN_ROM_PROBE done=%u cases=%u fail=%u lookup_miss=%u bounds_fail=%u frames=%u cycles=%llu last=%u got=%08X expect=%08X\n",
        (unsigned)mem->DebugRetrieve(done),n,nf,nm,nb,frames,(unsigned long long)dt,(unsigned)rd16(mem,last),
        (unsigned)rd32(mem,got),(unsigned)rd32(mem,want));
    if(!mem->DebugRetrieve(done)) return 5;
    if(nf||nm||nb) return 6;
    std::puts("GG_PROGJOIN_ROM_PROBE_PASS");
    return 0;
}
