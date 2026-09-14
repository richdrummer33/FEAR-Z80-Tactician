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
static bool any_symbol(const char* path,const char* a,const char* b,u16& out) {
    return find_symbol(path,a,out)||find_symbol(path,b,out);
}
static uint16_t rd16(Memory* mem,u16 a) {
    return (uint16_t)mem->DebugRetrieve(a)|((uint16_t)mem->DebugRetrieve((u16)(a+1u))<<8);
}
static uint64_t map_hash(Memory* mem,u16 map) {
    uint64_t h=1469598103934665603ull;
    for(unsigned i=0;i<720u;++i){h^=mem->DebugRetrieve((u16)(map+i));h*=1099511628211ull;}
    return h;
}
static bool press(GearsystemCore& core,const std::string& s) {
    if(s=="roomA-turn") core.KeyPressed(Joypad_1,Key_Right);
    else if(s=="roomA-forward") core.KeyPressed(Joypad_1,Key_Up);
    else if(s=="static") return true;
    else return false;
    return true;
}

int main(int argc,char**argv) {
    if(argc<8) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym mode scenario loops warmup out.csv\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1]; const char* sym=argv[2];
    const unsigned wanted_mode=(unsigned)std::strtoul(argv[3],nullptr,0);
    const std::string scenario=argv[4];
    const unsigned target=(unsigned)std::strtoul(argv[5],nullptr,0);
    const unsigned warmup=(unsigned)std::strtoul(argv[6],nullptr,0);
    const char* out=argv[7];
    if(wanted_mode>2u || !target) return 2;

    u16 phase=0,mode=0,map=0,state=0;
    if(!any_symbol(sym,"_g_ts_prof_phase","g_ts_prof_phase",phase) ||
       !any_symbol(sym,"_g_tspf_appearance_mode","g_tspf_appearance_mode",mode) ||
       !any_symbol(sym,"_g_map","g_map",map) ||
       !any_symbol(sym,"_g_state","g_state",state)) {
        std::fprintf(stderr,"required symbols missing\n"); return 3;
    }

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)){std::fprintf(stderr,"LoadROM failed\n");return 4;}
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384); int samples=0;
    GearsystemCore::GS_Debug_Run dbg{};
    dbg.step_debugger=true; dbg.stop_on_breakpoint=false; dbg.stop_on_run_to_breakpoint=false; dbg.stop_on_irq=false;
    Memory* mem=core.GetMemory();

    std::ofstream csv(out,std::ios::trunc);
    if(!csv){std::fprintf(stderr,"cannot open %s\n",out);return 5;}
    csv << "frame,loop_T,x_q4,y_q4,yaw,map_fnv64\n";

    uint8_t last_phase=mem->DebugRetrieve(phase);
    bool armed=false,pressed=false,have_start=false;
    unsigned seen=0,measured=0;
    uint64_t start=0,instructions=0;
    const uint64_t limit=250000000ull;

    while(measured<target && instructions<limit) {
        samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,&dbg,false); ++instructions;
        const uint8_t p=mem->DebugRetrieve(phase);
        if(p!=last_phase) {
            const uint64_t now=core.GetMasterClockCycles();
            if(p==1u) {
                if(!armed) {
                    mem->Write(mode,(uint8_t)wanted_mode);
                    if(mem->DebugRetrieve(mode)!=(uint8_t)wanted_mode){std::fprintf(stderr,"mode write failed\n");return 6;}
                    if(!press(core,scenario)){std::fprintf(stderr,"bad scenario\n");return 2;}
                    pressed=true; armed=true; seen=0; measured=0; start=now; have_start=true;
                } else if(have_start) {
                    const uint64_t loop_t=now-start;
                    if(seen>=warmup) {
                        const int16_t x=(int16_t)rd16(mem,state);
                        const int16_t y=(int16_t)rd16(mem,(u16)(state+2u));
                        const unsigned yaw=mem->DebugRetrieve((u16)(state+4u));
                        const uint64_t h=map_hash(mem,map);
                        char hs[32]; std::snprintf(hs,sizeof(hs),"%016llX",(unsigned long long)h);
                        csv << measured << ',' << loop_t << ',' << x << ',' << y << ',' << yaw << ',' << hs << '\n';
                        ++measured;
                    }
                    ++seen; start=now;
                }
            }
            last_phase=p;
        }
    }
    if(pressed) {
        if(scenario=="roomA-turn") core.KeyReleased(Joypad_1,Key_Right);
        else if(scenario=="roomA-forward") core.KeyReleased(Joypad_1,Key_Up);
    }
    if(measured!=target){std::fprintf(stderr,"only %u/%u loops, instructions=%llu\n",measured,target,(unsigned long long)instructions);return 7;}
    std::printf("AB_PROFILE rom=%s mode=%u scenario=%s loops=%u warmup=%u out=%s\n",rom,wanted_mode,scenario.c_str(),measured,warmup,out);
    return 0;
}
