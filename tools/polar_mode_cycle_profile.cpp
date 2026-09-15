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

static bool any_symbol(const char* path,const char* a,const char* b,u16& out) {
    return find_symbol(path,a,out)||find_symbol(path,b,out);
}

struct Stat {
    uint64_t sum=0,min=std::numeric_limits<uint64_t>::max(),max=0;
    unsigned n=0;
    void add(uint64_t v){sum+=v;min=std::min(min,v);max=std::max(max,v);++n;}
    double avg() const{return n?(double)sum/n:0.0;}
};

static double percentile(std::vector<uint64_t> v,double p) {
    if(v.empty()) return 0.0;
    std::sort(v.begin(),v.end());
    const double x=p*(double)(v.size()-1u);
    const size_t i=(size_t)x, j=std::min(i+1u,v.size()-1u);
    return (double)v[i]+(x-(double)i)*((double)v[j]-(double)v[i]);
}

int main(int argc,char**argv) {
    if(argc<4) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym mode[0..2] [loops=60] [warmup=8] [scenario=roomA-turn] [trace.csv]\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1]; const char* sym=argv[2];
    const unsigned target_mode=(unsigned)std::strtoul(argv[3],nullptr,0);
    const unsigned target=(argc>4)?(unsigned)std::strtoul(argv[4],nullptr,0):60u;
    const unsigned warmup=(argc>5)?(unsigned)std::strtoul(argv[5],nullptr,0):8u;
    const std::string scenario=(argc>6)?argv[6]:"roomA-turn";
    const char* csv_path=(argc>7)?argv[7]:nullptr;
    if(target_mode>2u || (scenario!="static" && scenario!="roomA-turn" && scenario!="roomA-forward")) {
        std::fprintf(stderr,"bad mode/scenario\n"); return 2;
    }

    u16 phase=0,stage=0,state=0,dirty=0,mode=0,active=0,touched=0,selectors=0;
    if(!any_symbol(sym,"_g_ts_prof_phase","g_ts_prof_phase",phase) ||
       !any_symbol(sym,"_g_tspf_appearance_mode","g_tspf_appearance_mode",mode)) {
        std::fprintf(stderr,"required phase/mode symbols missing\n"); return 3;
    }
    const bool have_stage=any_symbol(sym,"_g_ts_render_stage","g_ts_render_stage",stage);
    const bool have_state=any_symbol(sym,"_g_state","g_state",state);
    const bool have_dirty=any_symbol(sym,"_g_ts_dirty_words","g_ts_dirty_words",dirty);
    const bool have_active=any_symbol(sym,"_g_tspf_active_runs","g_tspf_active_runs",active);
    const bool have_touched=any_symbol(sym,"_g_tspf_touched_cells","g_tspf_touched_cells",touched);
    const bool have_selectors=any_symbol(sym,"_g_tspf_selector_tests","g_tspf_selector_tests",selectors);

    std::ofstream csv;
    if(csv_path) {
        csv.open(csv_path,std::ios::trunc);
        if(!csv){std::fprintf(stderr,"cannot open %s\n",csv_path);return 4;}
        csv << "frame,mode,scenario,loop_T,p1_input_T,p2_render_T,p3_vsync_T,p4_upload_T,p5_tail_T,x_q4,y_q4,yaw,active_runs,touched_cells,selector_tests,dirty_words\n";
    }

    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)){std::fprintf(stderr,"LoadROM failed\n");return 4;}
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384); int samples=0;
    GearsystemCore::GS_Debug_Run dbg{};
    dbg.step_debugger=true; dbg.stop_on_breakpoint=false; dbg.stop_on_run_to_breakpoint=false; dbg.stop_on_irq=false;
    Memory* mem=core.GetMemory();
    auto rd16=[&](u16 a)->uint16_t{return (uint16_t)mem->DebugRetrieve(a)|((uint16_t)mem->DebugRetrieve((u16)(a+1u))<<8);};

    uint8_t last_phase=mem->DebugRetrieve(phase);
    uint8_t last_stage=have_stage?mem->DebugRetrieve(stage):0u;
    uint64_t last_cycle=core.GetMasterClockCycles(), last_stage_cycle=last_cycle, loop_start=0;
    bool have_loop_start=false, mode_set=false, scenario_pressed=false;
    unsigned loops_seen=0,loops_measured=0;
    std::array<Stat,6> phase_stats{}; std::array<Stat,8> stage_stats{};
    std::array<uint64_t,6> frame_phase{};
    Stat loop_stats; std::vector<uint64_t> loop_values;
    unsigned frame_active=0,frame_touched=0,frame_selectors=0,frame_dirty=0;
    uint64_t instructions=0; const uint64_t instruction_limit=250000000ull;

    while(loops_measured<target && instructions<instruction_limit) {
        samples=0; core.RunToVBlank(fb.data(),audio.data(),&samples,&dbg,false); ++instructions;
        const uint64_t now=core.GetMasterClockCycles();
        const uint8_t p=mem->DebugRetrieve(phase);
        const uint8_t st=have_stage?mem->DebugRetrieve(stage):0u;

        if(have_stage && st!=last_stage) {
            const uint8_t leaving=last_stage;
            if(mode_set && loops_seen>=warmup && leaving>=1u && leaving<=7u)
                stage_stats[leaving].add(now-last_stage_cycle);
            last_stage=st; last_stage_cycle=now;
        }

        if(p!=last_phase) {
            if(mode_set && last_phase>=1u && last_phase<=5u) {
                const uint64_t dt=now-last_cycle;
                frame_phase[last_phase]+=dt;
                if(loops_seen>=warmup) phase_stats[last_phase].add(dt);
            }

            if(p==1u) {
                if(!mode_set) {
                    mem->Write(mode,(uint8_t)target_mode);
                    if(mem->DebugRetrieve(mode)!=(uint8_t)target_mode) {
                        std::fprintf(stderr,"mode write failed: wanted=%u got=%u addr=%04X\n",target_mode,(unsigned)mem->DebugRetrieve(mode),mode);
                        return 5;
                    }
                    if(scenario=="roomA-turn") core.KeyPressed(Joypad_1,Key_Right);
                    else if(scenario=="roomA-forward") core.KeyPressed(Joypad_1,Key_Up);
                    scenario_pressed=true;
                    mode_set=true; loops_seen=0; loops_measured=0; have_loop_start=true; loop_start=now;
                    frame_phase.fill(0u); last_stage_cycle=now;
                } else {
                    if(have_loop_start) {
                        const uint64_t loop_t=now-loop_start;
                        if(loops_seen>=warmup) {
                            loop_stats.add(loop_t); loop_values.push_back(loop_t);
                            int16_t xq=0,yq=0; unsigned yaw=0;
                            if(have_state){xq=(int16_t)rd16(state);yq=(int16_t)rd16((u16)(state+2u));yaw=mem->DebugRetrieve((u16)(state+4u));}
                            if(csv) csv << loops_measured << ',' << target_mode << ',' << scenario << ',' << loop_t << ','
                                << frame_phase[1] << ',' << frame_phase[2] << ',' << frame_phase[3] << ',' << frame_phase[4] << ',' << frame_phase[5] << ','
                                << xq << ',' << yq << ',' << yaw << ',' << frame_active << ',' << frame_touched << ',' << frame_selectors << ',' << frame_dirty << '\n';
                            ++loops_measured;
                        }
                        ++loops_seen;
                    }
                    loop_start=now; frame_phase.fill(0u);
                }
            }
            if(mode_set && p==3u && loops_seen>=warmup) {
                if(have_active) frame_active=mem->DebugRetrieve(active);
                if(have_touched) frame_touched=rd16(touched);
                if(have_selectors) frame_selectors=mem->DebugRetrieve(selectors);
            }
            if(mode_set && p==5u && loops_seen>=warmup && have_dirty) frame_dirty=rd16(dirty);
            last_phase=p; last_cycle=now;
        }
    }

    if(scenario_pressed) {
        if(scenario=="roomA-turn") core.KeyReleased(Joypad_1,Key_Right);
        else if(scenario=="roomA-forward") core.KeyReleased(Joypad_1,Key_Up);
    }
    if(!loop_stats.n) {
        std::fprintf(stderr,"no loops measured; mode=%u phase=%u instructions=%llu\n",target_mode,(unsigned)last_phase,(unsigned long long)instructions);
        return 6;
    }

    const double cpu_hz=3579545.0;
    const double pavg=phase_stats[1].avg()+phase_stats[2].avg()+phase_stats[3].avg()+phase_stats[4].avg()+phase_stats[5].avg();
    const double reconcile=loop_stats.avg()-pavg;
    std::printf("FULL_ROM_PROFILE mode=%u scenario=%s loops=%u warmup=%u instructions=%llu mode_addr=%04X\n",
                target_mode,scenario.c_str(),loop_stats.n,warmup,(unsigned long long)instructions,mode);
    const char* names[6]={"startup","input+motion","render/build","vsync-wait","VRAM-upload","loop-tail"};
    for(unsigned i=1;i<=5;++i)
        std::printf("  %-12s avg=%10.1f T min=%8llu max=%8llu n=%u\n",names[i],phase_stats[i].avg(),
                    (unsigned long long)phase_stats[i].min,(unsigned long long)phase_stats[i].max,phase_stats[i].n);
    if(have_stage) {
        std::printf("  render stages (transition chunks; sum/n is not per-frame):\n");
        for(unsigned i=1;i<=7;++i) if(stage_stats[i].n)
            std::printf("    stage%u total=%12llu T chunks=%u avg=%9.1f T\n",i,(unsigned long long)stage_stats[i].sum,stage_stats[i].n,stage_stats[i].avg());
    }
    std::printf("  loop         avg=%10.1f T p50=%10.1f p95=%10.1f min=%8llu max=%8llu -> %.2f updates/s\n",
                loop_stats.avg(),percentile(loop_values,0.50),percentile(loop_values,0.95),
                (unsigned long long)loop_stats.min,(unsigned long long)loop_stats.max,cpu_hz/loop_stats.avg());
    std::printf("  reconcile    phase-sum=%10.1f T delta=%+.1f T (%+.4f%%)\n",pavg,reconcile,100.0*reconcile/loop_stats.avg());
    if(std::llabs((long long)reconcile)>64ll) {
        std::fprintf(stderr,"RECONCILE_FAIL mode=%u scenario=%s delta=%.1f T\n",target_mode,scenario.c_str(),reconcile);
        return 7;
    }
    std::printf("RECONCILE_PASS mode=%u scenario=%s\n",target_mode,scenario.c_str());
    return 0;
}
