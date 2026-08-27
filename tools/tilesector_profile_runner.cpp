#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include "GearsystemCore.h"
#include "Memory.h"

bool g_mcp_stdio_mode = false;

static bool find_symbol(const char* path,const char* wanted,u16& addr) {
    std::ifstream f(path);
    std::string line;
    if(!f) return false;
    while(std::getline(f,line)) {
        unsigned bank=0,a=0;
        char sym[256]={0};
        if(std::sscanf(line.c_str(),"%x:%x %255s",&bank,&a,sym)==3) {
            if(std::strcmp(sym,wanted)==0) { addr=(u16)a; return true; }
        }
        if(std::sscanf(line.c_str(),"%x %255s",&a,sym)==2) {
            if(std::strcmp(sym,wanted)==0) { addr=(u16)a; return true; }
        }
    }
    return false;
}

struct Stat {
    uint64_t sum=0,min=std::numeric_limits<uint64_t>::max(),max=0;
    unsigned n=0;
    void add(uint64_t v) { sum+=v; min=std::min(min,v); max=std::max(max,v); ++n; }
    double avg() const { return n?(double)sum/n:0.0; }
};

int main(int argc,char**argv) {
    if(argc<3) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym [loops=120] [warmup=8]\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1];
    const char* sym=argv[2];
    unsigned target=(argc>3)?(unsigned)std::strtoul(argv[3],nullptr,0):120u;
    unsigned warmup=(argc>4)?(unsigned)std::strtoul(argv[4],nullptr,0):8u;
    std::string scenario=(argc>5)?argv[5]:"demo";
    if(scenario!="demo" && scenario!="roomA-turn") {
        std::fprintf(stderr,"unknown scenario: %s\n",scenario.c_str());
        return 2;
    }
    const char* mapdump_path=(argc>6)?argv[6]:nullptr;
    std::ofstream mapdump;
    if(mapdump_path) {
        mapdump.open(mapdump_path,std::ios::binary|std::ios::trunc);
        if(!mapdump) {
            std::fprintf(stderr,"cannot open map dump: %s\n",mapdump_path);
            return 2;
        }
    }
    u16 phase_addr=0,dirty_addr=0,stage_addr=0,state_addr=0,map_addr=0;
    u16 ret_full_total_addr=0,ret_full_skip_addr=0,ret_full_edge_addr=0;
    u16 ret_span_total_addr=0,ret_span_skip_addr=0;
    if(!find_symbol(sym,"_g_ts_prof_phase",phase_addr)&&!find_symbol(sym,"g_ts_prof_phase",phase_addr)) {
        std::fprintf(stderr,"phase symbol not found in %s\n",sym); return 3;
    }
    bool have_dirty=find_symbol(sym,"_g_ts_dirty_words",dirty_addr)||find_symbol(sym,"g_ts_dirty_words",dirty_addr);
    bool have_stage=find_symbol(sym,"_g_ts_render_stage",stage_addr)||find_symbol(sym,"g_ts_render_stage",stage_addr);
    bool have_state=find_symbol(sym,"_g_state",state_addr)||find_symbol(sym,"g_state",state_addr);
    bool have_map=find_symbol(sym,"_g_map",map_addr)||find_symbol(sym,"g_map",map_addr);
    bool have_ret=
        (find_symbol(sym,"_g_ts_ret_full_total",ret_full_total_addr)||find_symbol(sym,"g_ts_ret_full_total",ret_full_total_addr)) &&
        (find_symbol(sym,"_g_ts_ret_full_skip",ret_full_skip_addr)||find_symbol(sym,"g_ts_ret_full_skip",ret_full_skip_addr)) &&
        (find_symbol(sym,"_g_ts_ret_full_edgeonly",ret_full_edge_addr)||find_symbol(sym,"g_ts_ret_full_edgeonly",ret_full_edge_addr)) &&
        (find_symbol(sym,"_g_ts_ret_span_total",ret_span_total_addr)||find_symbol(sym,"g_ts_ret_span_total",ret_span_total_addr)) &&
        (find_symbol(sym,"_g_ts_ret_span_skip",ret_span_skip_addr)||find_symbol(sym,"g_ts_ret_span_skip",ret_span_skip_addr));

    GearsystemCore core;
    core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)) { std::fprintf(stderr,"LoadROM failed\n"); return 4; }
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
    struct MotionSample {
        int16_t x_q4=0,y_q4=0,speed_q4=0,turn_q4=0;
        uint8_t yaw=0,demo_phase=0;
        uint16_t demo_ticks=0;
    };
    auto sample_motion=[&]()->MotionSample {
        MotionSample m{};
        if(!have_state) return m;
        // TSState is naturally packed by SDCC/Z80 in declaration order.
        m.x_q4=(int16_t)rd16(state_addr+0u);
        m.y_q4=(int16_t)rd16(state_addr+2u);
        m.yaw=mem->DebugRetrieve((u16)(state_addr+4u));
        m.speed_q4=(int16_t)rd16((u16)(state_addr+5u));
        m.turn_q4=(int16_t)rd16((u16)(state_addr+9u));
        m.demo_phase=mem->DebugRetrieve((u16)(state_addr+13u));
        m.demo_ticks=rd16((u16)(state_addr+14u));
        return m;
    };

    uint8_t last_phase=mem->DebugRetrieve(phase_addr);
    uint8_t last_stage=have_stage?mem->DebugRetrieve(stage_addr):0u;
    uint64_t last_cycle=core.GetMasterClockCycles();
    uint64_t last_stage_cycle=last_cycle;
    uint64_t loop_start=0;
    bool have_loop_start=false;
    std::array<Stat,6> phase_stats{};
    std::array<Stat,8> stage_stats{};
    Stat loop_stats,dirty_stats;
    Stat ret_full_total_stats,ret_full_skip_stats,ret_full_edge_stats;
    Stat ret_span_total_stats,ret_span_skip_stats;
    unsigned loops_seen=0,loops_measured=0;
    MotionSample motion_first{},motion_last{};
    bool have_motion_first=false;
    uint64_t instructions=0;
    uint64_t map_hash=1469598103934665603ull; // FNV-1a over measured frame shadows
    unsigned map_hash_frames=0;
    const uint64_t instruction_limit=50000000ull;

    // External deterministic input scenario: no ROM instrumentation or hot-path
    // branches. RIGHT-only forces manual in-place yaw while staying in Room A.
    if(scenario=="roomA-turn")
        core.KeyPressed(Joypad_1,Key_Right);

    while(loops_measured<target&&instructions<instruction_limit) {
        samples=0;
        core.RunToVBlank(fb.data(),audio.data(),&samples,&dbg,false);
        ++instructions;
        uint64_t now=core.GetMasterClockCycles();
        uint8_t p=mem->DebugRetrieve(phase_addr);
        uint8_t st=have_stage?mem->DebugRetrieve(stage_addr):0u;

        if(have_stage&&st!=last_stage) {
            if(last_stage>=1u&&last_stage<=7u&&loops_seen>=warmup)
                stage_stats[last_stage].add(now-last_stage_cycle);
            last_stage=st;
            last_stage_cycle=now;
        }

        if(p!=last_phase) {
            if(last_phase<=5u&&p<=5u&&last_phase!=0u) {
                if(loops_seen>=warmup) phase_stats[last_phase].add(now-last_cycle);
            }
            if(p==1u) {
                if(have_loop_start) {
                    if(loops_seen>=warmup) {
                        loop_stats.add(now-loop_start);
                        if(have_map) {
                            for(unsigned i=0;i<720u;++i) {
                                const uint8_t v=mem->DebugRetrieve((u16)(map_addr+i));
                                map_hash ^= (uint64_t)v;
                                map_hash *= 1099511628211ull;
                                if(mapdump) mapdump.put((char)v);
                            }
                            ++map_hash_frames;
                        }
                        ++loops_measured;
                    }
                    ++loops_seen;
                } else have_loop_start=true;
                loop_start=now;
                if(have_state && loops_seen>=warmup && loops_measured<target) {
                    MotionSample m=sample_motion();
                    if(!have_motion_first) { motion_first=m; have_motion_first=true; }
                    motion_last=m;
                }
            }
            if(p==3u&&have_ret&&loops_seen>=warmup) {
                ret_full_total_stats.add(mem->DebugRetrieve(ret_full_total_addr));
                ret_full_skip_stats.add(mem->DebugRetrieve(ret_full_skip_addr));
                ret_full_edge_stats.add(mem->DebugRetrieve(ret_full_edge_addr));
                ret_span_total_stats.add(mem->DebugRetrieve(ret_span_total_addr));
                ret_span_skip_stats.add(mem->DebugRetrieve(ret_span_skip_addr));
            }
            if(p==5u&&have_dirty&&loops_seen>=warmup) {
                uint16_t dirty=(uint16_t)mem->DebugRetrieve(dirty_addr)|
                               ((uint16_t)mem->DebugRetrieve((u16)(dirty_addr+1u))<<8);
                dirty_stats.add(dirty);
            }
            last_phase=p;
            last_cycle=now;
        }
    }

    if(!loop_stats.n) {
        std::fprintf(stderr,"no measured loops; instructions=%llu phase=%u addr=%04X\n",
                     (unsigned long long)instructions,last_phase,phase_addr);
        return 5;
    }

    const double cpu_hz=3579545.0;
    std::printf("TileSector Gearsystem profile: loops=%u warmup=%u scenario=%s instructions=%llu phase_addr=%04X",
                loop_stats.n,warmup,scenario.c_str(),(unsigned long long)instructions,phase_addr);
    if(have_stage) std::printf(" render_stage_addr=%04X",stage_addr);
    if(have_state) std::printf(" state_addr=%04X",state_addr);
    if(have_map) std::printf(" map_addr=%04X",map_addr);
    std::printf("\n");
    const char* names[6]={"startup","input+motion","render/build","vsync-wait","VRAM-upload","loop-tail"};
    for(unsigned pidx=1;pidx<=5;++pidx) if(phase_stats[pidx].n) {
        std::printf("  %-12s avg=%8.1f T min=%5llu max=%5llu n=%u\n",names[pidx],phase_stats[pidx].avg(),
                    (unsigned long long)phase_stats[pidx].min,(unsigned long long)phase_stats[pidx].max,phase_stats[pidx].n);
    }
    if(have_stage) {
        const char* snames[8]={"idle","clear/lifetime","q4-transform","candidate-build","solid-raster","portal-control","portal-face","retained-life"};
        std::printf("  render/build subphases:\n");
        for(unsigned s=1;s<=7;++s) if(stage_stats[s].n) {
            std::printf("    %-15s total=%10llu T avg-chunk=%8.1f T min=%5llu max=%5llu n=%u\n",
                        snames[s],(unsigned long long)stage_stats[s].sum,stage_stats[s].avg(),
                        (unsigned long long)stage_stats[s].min,(unsigned long long)stage_stats[s].max,stage_stats[s].n);
        }
    }
    std::printf("  loop         avg=%8.1f T min=%5llu max=%5llu -> %.2f updates/s\n",
                loop_stats.avg(),(unsigned long long)loop_stats.min,(unsigned long long)loop_stats.max,
                cpu_hz/loop_stats.avg());
    double work=phase_stats[1].avg()+phase_stats[2].avg()+phase_stats[4].avg()+phase_stats[5].avg();
    std::printf("  active-work  avg=%8.1f T (%.1f%% of loop)\n",work,100.0*work/loop_stats.avg());
    if(dirty_stats.n) std::printf("  dirty words  avg=%8.1f min=%5llu max=%5llu\n",dirty_stats.avg(),
                                  (unsigned long long)dirty_stats.min,(unsigned long long)dirty_stats.max);
    if(ret_full_total_stats.n) {
        double col_total=(double)ret_full_total_stats.sum;
        double span_total=(double)ret_span_total_stats.sum;
        std::printf("  retained FULL cols avg=%6.2f exact-skip=%6.2f (%.1f%%) edge-only=%6.2f (%.1f%%)\n",
                    ret_full_total_stats.avg(),ret_full_skip_stats.avg(),
                    col_total?100.0*(double)ret_full_skip_stats.sum/col_total:0.0,
                    ret_full_edge_stats.avg(),
                    col_total?100.0*(double)ret_full_edge_stats.sum/col_total:0.0);
        std::printf("  retained FULL spans avg=%5.2f whole-span-skip=%5.2f (%.1f%%)\n",
                    ret_span_total_stats.avg(),ret_span_skip_stats.avg(),
                    span_total?100.0*(double)ret_span_skip_stats.sum/span_total:0.0);
    }
    if(have_map) {
        std::printf("  map-hash     frames=%u fnv64=%016llX\n",
                    map_hash_frames,(unsigned long long)map_hash);
    }
    if(have_motion_first) {
        std::printf("  motion start x=%.2f y=%.2f yaw=%u speed_q4=%d turn_q4=%d demo_phase=%u demo_ticks=%u\n",
                    motion_first.x_q4/16.0,motion_first.y_q4/16.0,motion_first.yaw,
                    motion_first.speed_q4,motion_first.turn_q4,
                    motion_first.demo_phase,motion_first.demo_ticks);
        std::printf("  motion last  x=%.2f y=%.2f yaw=%u speed_q4=%d turn_q4=%d demo_phase=%u demo_ticks=%u\n",
                    motion_last.x_q4/16.0,motion_last.y_q4/16.0,motion_last.yaw,
                    motion_last.speed_q4,motion_last.turn_q4,
                    motion_last.demo_phase,motion_last.demo_ticks);
    }
    return 0;
}
