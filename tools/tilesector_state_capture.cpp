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

static bool any_symbol(const char* sym,const char* a,const char* b,u16& out) {
    return find_symbol(sym,a,out)||find_symbol(sym,b,out);
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

static uint16_t rd16(Memory* mem,u16 a) {
    return (uint16_t)mem->DebugRetrieve(a)|((uint16_t)mem->DebugRetrieve((u16)(a+1u))<<8);
}

static uint64_t map_hash(Memory* mem,u16 map_addr,bool have_map) {
    uint64_t h=1469598103934665603ull;
    if(!have_map) return 0ull;
    for(unsigned i=0;i<720u;++i) {
        h^=(uint64_t)mem->DebugRetrieve((u16)(map_addr+i));
        h*=1099511628211ull;
    }
    return h;
}

static bool press_scenario(GearsystemCore& core,const std::string& scenario) {
    if(scenario=="demo") return true;
    if(scenario=="roomA-turn") core.KeyPressed(Joypad_1,Key_Right);
    else if(scenario=="roomA-forward") core.KeyPressed(Joypad_1,Key_Up);
    else if(scenario=="roomA-back") core.KeyPressed(Joypad_1,Key_Down);
    else if(scenario=="roomA-button1") core.KeyPressed(Joypad_1,Key_1);
    else if(scenario=="roomA-button2") core.KeyPressed(Joypad_1,Key_2);
    else return false;
    return true;
}

static int vblank_sequence(int argc,char**argv) {
    if(argc<9) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym --vblank-sequence scenario frames warmup_vblanks frame_prefix state.csv\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1]; const char* sym=argv[2];
    const std::string scenario=argv[4];
    const unsigned frames=(unsigned)std::strtoul(argv[5],nullptr,0);
    const unsigned warmup=(unsigned)std::strtoul(argv[6],nullptr,0);
    const std::string prefix=argv[7]; const char* csv_path=argv[8];
    if(!frames) return 2;

    u16 state=0,phase=0,map=0,loops=0,dirty=0;
    if(!any_symbol(sym,"_g_state","g_state",state)) {
        std::fprintf(stderr,"state symbol missing\n"); return 3;
    }
    const bool have_phase=any_symbol(sym,"_g_ts_prof_phase","g_ts_prof_phase",phase);
    const bool have_map=any_symbol(sym,"_g_map","g_map",map);
    const bool have_loops=any_symbol(sym,"_g_ts_loop_count","g_ts_loop_count",loops);
    const bool have_dirty=any_symbol(sym,"_g_ts_dirty_words","g_ts_dirty_words",dirty);

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)){std::fprintf(stderr,"LoadROM failed\n");return 4;}
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384); int samples=0;
    Memory* mem=core.GetMemory();

    /* Warm up with no external input so baseline and lit evidence begin from
     * the same stable displayed room state. This is a presentation capture,
     * not a profiler run: one sample is one real emulated display VBlank. */
    for(unsigned i=0;i<warmup;++i) {
        samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true);
    }
    if(!press_scenario(core,scenario)) {
        std::fprintf(stderr,"unknown scenario: %s\n",scenario.c_str()); return 2;
    }

    std::ofstream csv(csv_path,std::ios::trunc);
    if(!csv){std::fprintf(stderr,"cannot open csv: %s\n",csv_path);return 6;}
    csv << "frame,vblank,x_q4,y_q4,x,y,yaw,speed_q4,turn_q4,phase,loop_count,dirty_words,map_fnv64\n";

    GS_RuntimeInfo ri{}; core.GetRuntimeInfo(ri);
    int16_t first_x=0,first_y=0,last_x=0,last_y=0; uint8_t first_yaw=0,last_yaw=0;
    for(unsigned i=0;i<frames;++i) {
        samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true);
        char path[1024]; std::snprintf(path,sizeof(path),"%s-%04u.ppm",prefix.c_str(),i);
        save_ppm(path,fb,ri.screen_width,ri.screen_height);

        const int16_t xq=(int16_t)rd16(mem,state+0u);
        const int16_t yq=(int16_t)rd16(mem,(u16)(state+2u));
        const uint8_t yaw=mem->DebugRetrieve((u16)(state+4u));
        const int16_t speed=(int16_t)rd16(mem,(u16)(state+5u));
        const int16_t turn=(int16_t)rd16(mem,(u16)(state+9u));
        const unsigned p=have_phase?mem->DebugRetrieve(phase):0u;
        const unsigned lc=have_loops?rd16(mem,loops):0u;
        const unsigned dw=have_dirty?rd16(mem,dirty):0u;
        const uint64_t mh=map_hash(mem,map,have_map);
        if(i==0u){first_x=xq;first_y=yq;first_yaw=yaw;}
        last_x=xq;last_y=yq;last_yaw=yaw;
        csv << i << ',' << (warmup+i+1u) << ',' << xq << ',' << yq << ','
            << (xq/16.0) << ',' << (yq/16.0) << ',' << (unsigned)yaw << ','
            << speed << ',' << turn << ',' << p << ',' << lc << ',' << dw << ',';
        char hs[32]; std::snprintf(hs,sizeof(hs),"%016llX",(unsigned long long)mh); csv << hs << '\n';
    }
    std::printf("vblank-sequence scenario=%s frames=%u warmup=%u screen=%dx%d start=(%.2f,%.2f,%u) end=(%.2f,%.2f,%u) csv=%s prefix=%s\n",
                scenario.c_str(),frames,warmup,ri.screen_width,ri.screen_height,
                first_x/16.0,first_y/16.0,(unsigned)first_yaw,
                last_x/16.0,last_y/16.0,(unsigned)last_yaw,csv_path,prefix.c_str());
    return 0;
}

int main(int argc,char**argv) {
    if(argc>=4 && std::strcmp(argv[3],"--vblank-sequence")==0)
        return vblank_sequence(argc,argv);
    if(argc<5) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym demo_ticks out.ppm\n",argv[0]);
        std::fprintf(stderr,"   or: %s rom.gg rom.sym --vblank-sequence scenario frames warmup_vblanks frame_prefix state.csv\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1]; const char* sym=argv[2];
    const unsigned target=(unsigned)std::strtoul(argv[3],nullptr,0);
    const char* out=argv[4];
    u16 state=0,phase=0;
    if(!any_symbol(sym,"_g_state","g_state",state) ||
       !any_symbol(sym,"_g_ts_prof_phase","g_ts_prof_phase",phase)) {
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
        const uint16_t t=rd16(mem,ticks);
        const uint8_t p=mem->DebugRetrieve(phase);
        if(t>=target && p==5u) break;
    }
    if(ins>=limit){std::fprintf(stderr,"capture target not reached\n");return 5;}
    samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true);
    GS_RuntimeInfo ri{}; core.GetRuntimeInfo(ri);
    save_ppm(out,fb,ri.screen_width,ri.screen_height);
    const int16_t xq=(int16_t)rd16(mem,state);
    const int16_t yq=(int16_t)rd16(mem,(u16)(state+2u));
    const uint8_t yaw=mem->DebugRetrieve((u16)(state+4u));
    std::printf("capture=%s target_ticks=%u actual_ticks=%u x=%.2f y=%.2f yaw=%u instructions=%llu screen=%dx%d\n",
                out,target,(unsigned)rd16(mem,ticks),xq/16.0,yq/16.0,yaw,
                (unsigned long long)ins,ri.screen_width,ri.screen_height);
    return 0;
}
