#include <cstdint>
#include <cstdio>
#include <cstring>
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
static void save_ppm(const char* path,const std::vector<u8>& fb,int w,int h) {
    FILE* f=std::fopen(path,"wb"); if(!f){std::perror("fopen");std::exit(6);}
    std::fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        const u8* p=&fb[(y*GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN+x)*4];
        std::fwrite(p,1,3,f);
    }
    std::fclose(f);
}
int main(int argc,char**argv) {
    if(argc<5) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym demo_ticks out.ppm\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1]; const char* sym=argv[2];
    const unsigned target=(unsigned)std::strtoul(argv[3],nullptr,0);
    const char* out=argv[4];
    u16 state=0,phase=0;
    if((!find_symbol(sym,"_g_state",state)&&!find_symbol(sym,"g_state",state)) ||
       (!find_symbol(sym,"_g_ts_prof_phase",phase)&&!find_symbol(sym,"g_ts_prof_phase",phase))) {
        std::fprintf(stderr,"required state/phase symbols missing\n"); return 3;
    }

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)){std::fprintf(stderr,"LoadROM failed\n");return 4;}
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384); int samples=0;
    GearsystemCore::GS_Debug_Run dbg{};
    dbg.step_debugger=true; dbg.stop_on_breakpoint=false; dbg.stop_on_run_to_breakpoint=false; dbg.stop_on_irq=false;
    Memory* mem=core.GetMemory();
    const u16 ticks=(u16)(state+14u);
    const uint64_t limit=120000000ull; uint64_t ins=0;
    while(ins++<limit) {
        samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,&dbg,false);
        const uint16_t t=(uint16_t)mem->DebugRetrieve(ticks)|((uint16_t)mem->DebugRetrieve((u16)(ticks+1u))<<8);
        const uint8_t p=mem->DebugRetrieve(phase);
        if(t>=target && p==5u) break; // desired update has rendered and uploaded
    }
    if(ins>=limit){std::fprintf(stderr,"capture target not reached\n");return 5;}
    // One display VBlank to materialize the just-uploaded name table into framebuffer.
    samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true);
    GS_RuntimeInfo ri{}; core.GetRuntimeInfo(ri);
    save_ppm(out,fb,ri.screen_width,ri.screen_height);
    const int16_t xq=(int16_t)((uint16_t)mem->DebugRetrieve(state)|((uint16_t)mem->DebugRetrieve((u16)(state+1u))<<8));
    const int16_t yq=(int16_t)((uint16_t)mem->DebugRetrieve((u16)(state+2u))|((uint16_t)mem->DebugRetrieve((u16)(state+3u))<<8));
    const uint8_t yaw=mem->DebugRetrieve((u16)(state+4u));
    std::printf("capture=%s target_ticks=%u actual_ticks=%u x=%.2f y=%.2f yaw=%u instructions=%llu screen=%dx%d\n",
                out,target,(unsigned)((uint16_t)mem->DebugRetrieve(ticks)|((uint16_t)mem->DebugRetrieve((u16)(ticks+1u))<<8)),
                xq/16.0,yq/16.0,yaw,(unsigned long long)ins,ri.screen_width,ri.screen_height);
    return 0;
}
