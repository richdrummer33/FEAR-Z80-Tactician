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
        const char* p=line.c_str();
        while(*p==' '||*p=='\t') ++p;
        if(*p=='\0'||*p==';') continue;
        unsigned bank=0,a=0; char sym[256]={0};
        if(std::strncmp(p,"DEF ",4)==0) {
            /* no$gmb/NoICE ".noi": DEF <symbol> <value>. This must be matched
               before the hex-first forms: "DEF" is itself valid hex, so a plain
               "%x %255s" parse silently resolves every .noi symbol to 0x0DEF. */
            if(std::sscanf(p,"DEF %255s %x",sym,&a)==2 && std::strcmp(sym,wanted)==0) { addr=(u16)a; return true; }
            continue;
        }
        /* makebin ".sym": <bank>:<addr> <symbol> */
        if(std::sscanf(p,"%x:%x %255s",&bank,&a,sym)==3 && std::strcmp(sym,wanted)==0) { addr=(u16)a; return true; }
        if(std::sscanf(p,"%x %255s",&a,sym)==2 && std::strcmp(sym,wanted)==0) { addr=(u16)a; return true; }
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
        /* Gearsystem's GG render buffer is packed at runtime width (160),
         * despite the caller allocating the maximum 320x288 backing store. */
        const u8* p=&fb[(y*w+x)*4];
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
        std::fprintf(stderr,"usage: %s rom.gg rom.sym --vblank-sequence scenario frames settle_vblanks frame_prefix state.csv\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1]; const char* sym=argv[2];
    const std::string scenario=argv[4];
    const unsigned frames=(unsigned)std::strtoul(argv[5],nullptr,0);
    const unsigned settle=(unsigned)std::strtoul(argv[6],nullptr,0);
    const std::string prefix=argv[7]; const char* csv_path=argv[8];
    if(!frames) return 2;

    u16 state=0,phase=0,map=0,loops=0,dirty=0,opt_layout=0,opt_loops=0;
    if(!any_symbol(sym,"_g_state","g_state",state)) {
        std::fprintf(stderr,"state symbol missing\n"); return 3;
    }
    const bool have_phase=any_symbol(sym,"_g_ts_prof_phase","g_ts_prof_phase",phase);
    const bool have_map=any_symbol(sym,"_g_map","g_map",map);
    const bool have_loops=any_symbol(sym,"_g_ts_loop_count","g_ts_loop_count",loops);
    const bool have_dirty=any_symbol(sym,"_g_ts_dirty_words","g_ts_dirty_words",dirty);
    const bool opt_state=any_symbol(sym,"_g_opt_state_layout","g_opt_state_layout",opt_layout);
    const bool have_opt_loops=any_symbol(sym,"_g_opt_loop_count","g_opt_loop_count",opt_loops);

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)){std::fprintf(stderr,"LoadROM failed\n");return 4;}
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384); int samples=0;
    Memory* mem=core.GetMemory();

    /* Tile upload + first renderer boot is much longer than a handful of display
     * frames. Wait for the profiled main loop itself to prove that initialization
     * is complete, then give the display a short neutral settle period. */
    unsigned boot_vblanks=0u;
    const unsigned boot_limit=1200u;
    bool booted=false;
    while(boot_vblanks<boot_limit) {
        samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true); ++boot_vblanks;
        if(opt_state && have_opt_loops) booted=(rd16(mem,opt_loops)>=2u);
        else if(have_loops) booted=(rd16(mem,loops)>=2u);
        else booted=(rd16(mem,state)!=0u || rd16(mem,(u16)(state+2u))!=0u);
        if(booted) break;
    }
    if(!booted) {
        std::fprintf(stderr,"ROM did not reach initialized main loop after %u VBlanks; x=%d y=%d phase=%u loops=%u\n",
                     boot_vblanks,(int16_t)rd16(mem,state),(int16_t)rd16(mem,(u16)(state+2u)),
                     have_phase?mem->DebugRetrieve(phase):0u,have_loops?rd16(mem,loops):0u);
        return 5;
    }
    for(unsigned i=0;i<settle;++i) {
        samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true);
    }
    if(!press_scenario(core,scenario)) {
        std::fprintf(stderr,"unknown scenario: %s\n",scenario.c_str()); return 2;
    }

    std::ofstream csv(csv_path,std::ios::trunc);
    if(!csv){std::fprintf(stderr,"cannot open csv: %s\n",csv_path);return 6;}
    csv << "frame,vblank,x_q4,y_q4,z_q4,x,y,z,yaw,speed_q4,turn_q4,phase,loop_count,dirty_words,map_fnv64\n";

    GS_RuntimeInfo ri{}; core.GetRuntimeInfo(ri);
    int16_t first_x=0,first_y=0,last_x=0,last_y=0; uint8_t first_yaw=0,last_yaw=0;
    for(unsigned i=0;i<frames;++i) {
        samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true);
        char path[1024]; std::snprintf(path,sizeof(path),"%s-%04u.ppm",prefix.c_str(),i);
        save_ppm(path,fb,ri.screen_width,ri.screen_height);

        const int16_t xq=(int16_t)rd16(mem,state+0u);
        const int16_t yq=(int16_t)rd16(mem,(u16)(state+2u));
        const int16_t zq=opt_state?(int16_t)rd16(mem,(u16)(state+4u)):0;
        const uint8_t yaw=mem->DebugRetrieve((u16)(state+(opt_state?6u:4u)));
        const int16_t speed=(int16_t)rd16(mem,(u16)(state+(opt_state?7u:5u)));
        const int16_t turn=(int16_t)rd16(mem,(u16)(state+(opt_state?11u:9u)));
        const unsigned p=have_phase?mem->DebugRetrieve(phase):0u;
        const unsigned lc=have_loops?rd16(mem,loops):0u;
        const unsigned dw=have_dirty?rd16(mem,dirty):0u;
        const uint64_t mh=map_hash(mem,map,have_map);
        if(i==0u){first_x=xq;first_y=yq;first_yaw=yaw;}
        last_x=xq;last_y=yq;last_yaw=yaw;
        csv << i << ',' << (boot_vblanks+settle+i+1u) << ',' << xq << ',' << yq << ',' << zq << ','
            << (xq/16.0) << ',' << (yq/16.0) << ',' << (zq/16.0) << ',' << (unsigned)yaw << ','
            << speed << ',' << turn << ',' << p << ',' << lc << ',' << dw << ',';
        char hs[32]; std::snprintf(hs,sizeof(hs),"%016llX",(unsigned long long)mh); csv << hs << '\n';
    }
    std::printf("vblank-sequence scenario=%s frames=%u boot_vblanks=%u settle=%u screen=%dx%d start=(%.2f,%.2f,%u) end=(%.2f,%.2f,%u) loops=%u csv=%s prefix=%s\n",
                scenario.c_str(),frames,boot_vblanks,settle,ri.screen_width,ri.screen_height,
                first_x/16.0,first_y/16.0,(unsigned)first_yaw,
                last_x/16.0,last_y/16.0,(unsigned)last_yaw,have_loops?rd16(mem,loops):0u,csv_path,prefix.c_str());
    return 0;
}


struct SettledPose {
    int16_t xq=0,yq=0,zq=0;
    uint8_t yaw=0;
};
static bool same_pose(const SettledPose& a,const SettledPose& b) {
    return a.xq==b.xq && a.yq==b.yq && a.zq==b.zq && a.yaw==b.yaw;
}
static bool already_captured(const std::vector<SettledPose>& v,const SettledPose& p) {
    for(const auto& q:v) if(same_pose(q,p)) return true;
    return false;
}

static int settled_trace_capture(int argc,char**argv) {
    if(argc<8) {
        std::fprintf(stderr,
            "usage: %s rom.gg rom.sym --settled-trace out_dir target_captures max_vblanks stable_updates\n",
            argv[0]);
        return 2;
    }
    const char* rom=argv[1];
    const char* sym=argv[2];
    const std::string out_dir=argv[4];
    const unsigned target=(unsigned)std::strtoul(argv[5],nullptr,0);
    const unsigned max_vblanks=(unsigned)std::strtoul(argv[6],nullptr,0);
    const unsigned stable_need=(unsigned)std::strtoul(argv[7],nullptr,0);
    if(!target || !max_vblanks || !stable_need) return 2;

    u16 state_addr=0,loops=0,row_min=0,mix_pending=0,mix_publish=0,opt_layout=0;
    if(!any_symbol(sym,"_g_state","g_state",state_addr) ||
       !any_symbol(sym,"_g_ts_loop_count","g_ts_loop_count",loops) ||
       !any_symbol(sym,"_g_polar_nt_row_min","g_polar_nt_row_min",row_min)) {
        std::fprintf(stderr,"settled trace required state/loop/dirty-row symbols missing\n");
        return 3;
    }
    const bool opt_state=any_symbol(sym,"_g_opt_state_layout","g_opt_state_layout",opt_layout);
    const bool have_mix_pending=
        any_symbol(sym,"_g_tspf_mixed_patterns_pending","g_tspf_mixed_patterns_pending",mix_pending);
    const bool have_mix_publish=
        any_symbol(sym,"_g_tspf_mixed_publish_rows","g_tspf_mixed_publish_rows",mix_publish);

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)){std::fprintf(stderr,"LoadROM failed\n");return 4;}
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384); int samples=0;
    Memory* mem=core.GetMemory();
    GS_RuntimeInfo ri{}; core.GetRuntimeInfo(ri);

    auto read_pose=[&]() {
        SettledPose p;
        p.xq=(int16_t)rd16(mem,state_addr+0u);
        p.yq=(int16_t)rd16(mem,(u16)(state_addr+2u));
        p.zq=opt_state?(int16_t)rd16(mem,(u16)(state_addr+4u)):0;
        p.yaw=mem->DebugRetrieve((u16)(state_addr+(opt_state?6u:4u)));
        return p;
    };
    auto publication_clear=[&]() {
        for(unsigned r=0;r<18u;++r)
            if(mem->DebugRetrieve((u16)(row_min+r))!=0xffu) return false;
        if(have_mix_pending && mem->DebugRetrieve(mix_pending)) return false;
        if(have_mix_publish)
            for(unsigned r=0;r<18u;++r)
                if(mem->DebugRetrieve((u16)(mix_publish+r))) return false;
        return true;
    };

    /* Wait for the profiled main loop, not an arbitrary VBlank count. */
    unsigned vb=0u;
    while(vb<1200u && rd16(mem,loops)<2u) {
        samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true); ++vb;
    }
    if(rd16(mem,loops)<2u) {
        std::fprintf(stderr,"settled trace never reached main loop\n");
        return 5;
    }

    std::string csv_path=out_dir+"/poses.csv";
    std::ofstream csv(csv_path,std::ios::trunc);
    if(!csv){std::fprintf(stderr,"cannot open %s\n",csv_path.c_str());return 6;}
    csv<<"capture,file,vblank,loop_count,x_q4,y_q4,z_q4,yaw\n";

    uint16_t last_loop=rd16(mem,loops);
    SettledPose last_pose=read_pose();
    unsigned same_updates=1u;
    std::vector<SettledPose> captured;

    while(vb<max_vblanks && captured.size()<target) {
        samples=0;
        core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true);
        ++vb;
        const uint16_t lc=rd16(mem,loops);
        if(lc!=last_loop) {
            const SettledPose p=read_pose();
            if(same_pose(p,last_pose)) ++same_updates;
            else { last_pose=p; same_updates=1u; }
            last_loop=lc;
        }

        if(same_updates>=stable_need && publication_clear() &&
           !already_captured(captured,last_pose)) {
            char name[64];
            std::snprintf(name,sizeof(name),"pose-%02u.ppm",(unsigned)captured.size());
            std::string path=out_dir+"/"+name;
            save_ppm(path.c_str(),fb,ri.screen_width,ri.screen_height);
            csv<<captured.size()<<','<<name<<','<<vb<<','<<last_loop<<','
               <<last_pose.xq<<','<<last_pose.yq<<','<<last_pose.zq<<','
               <<(unsigned)last_pose.yaw<<'\n';
            captured.push_back(last_pose);
            std::printf("SETTLED_CAPTURE idx=%zu pose=(%d,%d,%d,%u) vblank=%u loop=%u file=%s\n",
                captured.size()-1,(int)last_pose.xq,(int)last_pose.yq,(int)last_pose.zq,
                (unsigned)last_pose.yaw,vb,(unsigned)last_loop,path.c_str());
        }
    }

    std::printf("SETTLED_TRACE captures=%zu target=%u vblanks=%u screen=%dx%d csv=%s\n",
        captured.size(),target,vb,ri.screen_width,ri.screen_height,csv_path.c_str());
    if(captured.size()<target) {
        std::fprintf(stderr,"only %zu/%u settled poses captured\n",captured.size(),target);
        return 7;
    }
    return 0;
}

int main(int argc,char**argv) {
    if(argc>=4 && std::strcmp(argv[3],"--settled-trace")==0)
        return settled_trace_capture(argc,argv);
    if(argc>=4 && std::strcmp(argv[3],"--vblank-sequence")==0)
        return vblank_sequence(argc,argv);
    if(argc<5) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym demo_ticks out.ppm\n",argv[0]);
        std::fprintf(stderr,"   or: %s rom.gg rom.sym --vblank-sequence scenario frames settle_vblanks frame_prefix state.csv\n",argv[0]);
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
