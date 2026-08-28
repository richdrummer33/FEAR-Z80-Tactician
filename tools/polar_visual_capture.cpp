#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "GearsystemCore.h"
#include "Memory.h"

bool g_mcp_stdio_mode = false;

static bool find_symbol(const char* path,const char* wanted,u16& addr) {
    std::ifstream f(path); std::string line;
    if(!f) return false;
    while(std::getline(f,line)) {
        unsigned bank=0,a=0; char sym[256]={0};
        if(std::sscanf(line.c_str(),"%x:%x %255s",&bank,&a,sym)==3 && std::strcmp(sym,wanted)==0) { addr=(u16)a; return true; }
        if(std::sscanf(line.c_str(),"%x %255s",&a,sym)==2 && std::strcmp(sym,wanted)==0) { addr=(u16)a; return true; }
    }
    return false;
}

static void save_ppm(const std::string& path,const std::vector<u8>& fb,int w,int h) {
    FILE* f=std::fopen(path.c_str(),"wb");
    if(!f){std::perror("fopen");std::exit(6);}
    std::fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        const u8* p=&fb[(y*GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN+x)*4];
        std::fwrite(p,1,3,f);
    }
    std::fclose(f);
}

int main(int argc,char**argv) {
    if(argc<6) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym frames warmup outdir\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1];
    const char* sym=argv[2];
    const unsigned target=(unsigned)std::strtoul(argv[3],nullptr,0);
    const unsigned warmup=(unsigned)std::strtoul(argv[4],nullptr,0);
    const std::filesystem::path outdir=argv[5];

    u16 state_addr=0,phase_addr=0;
    if((!find_symbol(sym,"_g_state",state_addr)&&!find_symbol(sym,"g_state",state_addr)) ||
       (!find_symbol(sym,"_g_ts_prof_phase",phase_addr)&&!find_symbol(sym,"g_ts_prof_phase",phase_addr))) {
        std::fprintf(stderr,"required state/phase symbols missing in %s\n",sym);
        return 3;
    }
    std::filesystem::create_directories(outdir);

    GearsystemCore core;
    core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)) {
        std::fprintf(stderr,"LoadROM failed\n");
        return 4;
    }
    GS_RuntimeInfo ri{}; core.GetRuntimeInfo(ri);
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384);
    int samples=0;
    GearsystemCore::GS_Debug_Run dbg{};
    dbg.step_debugger=true;
    dbg.stop_on_breakpoint=false;
    dbg.stop_on_run_to_breakpoint=false;
    dbg.stop_on_irq=false;
    Memory* mem=core.GetMemory();

    auto rd16=[&](u16 a)->uint16_t {
        return (uint16_t)mem->DebugRetrieve(a) |
               ((uint16_t)mem->DebugRetrieve((u16)(a+1u))<<8);
    };

    const u16 ticks_addr=(u16)(state_addr+14u);
    unsigned captured=0;
    uint16_t last_tick=0xffffu;
    uint64_t instructions=0;
    const uint64_t instruction_limit=220000000ull;

    std::ofstream meta(outdir/"frames.csv",std::ios::trunc);
    meta<<"frame,tick,x,y,yaw,cycles\n";

    while(captured<target && instructions<instruction_limit) {
        samples=0;
        core.RunToVBlank(fb.data(),audio.data(),&samples,&dbg,false);
        ++instructions;

        const uint8_t phase=mem->DebugRetrieve(phase_addr);
        const uint16_t tick=rd16(ticks_addr);
        if(phase==5u && tick>(uint16_t)warmup && tick!=last_tick) {
            /* Phase 5 means this update has rendered and uploaded its name table.
             * One real display VBlank materializes that VRAM state into Gearsystem's
             * framebuffer, matching tilesector_state_capture.cpp's proven path. */
            samples=0;
            core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true);
            last_tick=tick;

            char name[64];
            std::snprintf(name,sizeof(name),"frame%04u.ppm",captured);
            save_ppm((outdir/name).string(),fb,ri.screen_width,ri.screen_height);
            const int16_t xq=(int16_t)rd16(state_addr+0u);
            const int16_t yq=(int16_t)rd16(state_addr+2u);
            const uint8_t yaw=mem->DebugRetrieve((u16)(state_addr+4u));
            meta<<captured<<','<<tick<<','<<(xq/16.0)<<','<<(yq/16.0)<<','<<(unsigned)yaw<<','<<core.GetMasterClockCycles()<<"\n";
            ++captured;
        }
    }

    if(captured!=target) {
        std::fprintf(stderr,"only %u/%u visual frames captured after %llu instructions\n",
                     captured,target,(unsigned long long)instructions);
        return 5;
    }
    std::printf("captured=%u warmup=%u outdir=%s screen=%dx%d instructions=%llu phase_addr=%04X\n",
                captured,warmup,outdir.string().c_str(),ri.screen_width,ri.screen_height,
                (unsigned long long)instructions,phase_addr);
    return 0;
}
