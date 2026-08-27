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
    u16 phase_addr=0,dirty_addr=0,stage_addr=0;
    u16 ret_full_total_addr=0,ret_full_skip_addr=0,ret_full_edge_addr=0;
    u16 ret_span_total_addr=0,ret_span_skip_addr=0;
    u16 cand_total_addr=0,cand_uniform_addr=0,cand_reject_addr=0,cand_replace_addr=0,cand_fallback_addr=0;
    if(!find_symbol(sym,"_g_ts_prof_phase",phase_addr)&&!find_symbol(sym,"g_ts_prof_phase",phase_addr)) {
        std::fprintf(stderr,"phase symbol not found in %s\n",sym); return 3;
    }
    bool have_dirty=find_symbol(sym,"_g_ts_dirty_words",dirty_addr)||find_symbol(sym,"g_ts_dirty_words",dirty_addr);
    bool have_stage=find_symbol(sym,"_g_ts_render_stage",stage_addr)||find_symbol(sym,"g_ts_render_stage",stage_addr);
    bool have_cand_fast=
        (find_symbol(sym,"_g_ts_cand_total",cand_total_addr)||find_symbol(sym,"g_ts_cand_total",cand_total_addr)) &&
        (find_symbol(sym,"_g_ts_cand_uniform",cand_uniform_addr)||find_symbol(sym,"g_ts_cand_uniform",cand_uniform_addr)) &&
        (find_symbol(sym,"_g_ts_cand_reject",cand_reject_addr)||find_symbol(sym,"g_ts_cand_reject",cand_reject_addr)) &&
        (find_symbol(sym,"_g_ts_cand_replace",cand_replace_addr)||find_symbol(sym,"g_ts_cand_replace",cand_replace_addr)) &&
        (find_symbol(sym,"_g_ts_cand_fallback",cand_fallback_addr)||find_symbol(sym,"g_ts_cand_fallback",cand_fallback_addr));
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

    uint8_t last_phase=mem->DebugRetrieve(phase_addr);
    uint8_t last_stage=have_stage?mem->DebugRetrieve(stage_addr):0u;
    uint64_t last_cycle=core.GetMasterClockCycles();
    uint64_t last_stage_cycle=last_cycle;
    uint64_t loop_start=0;
    bool have_loop_start=false;
    std::array<Stat,6> phase_stats{};
    std::array<Stat,7> stage_stats{};
    Stat loop_stats,dirty_stats;
    Stat ret_full_total_stats,ret_full_skip_stats,ret_full_edge_stats;
    Stat ret_span_total_stats,ret_span_skip_stats;
    Stat cand_total_stats,cand_uniform_stats,cand_reject_stats,cand_replace_stats,cand_fallback_stats;
    unsigned loops_seen=0,loops_measured=0;
    uint64_t instructions=0;
    const uint64_t instruction_limit=50000000ull;

    while(loops_measured<target&&instructions<instruction_limit) {
        samples=0;
        core.RunToVBlank(fb.data(),audio.data(),&samples,&dbg,false);
        ++instructions;
        uint64_t now=core.GetMasterClockCycles();
        uint8_t p=mem->DebugRetrieve(phase_addr);
        uint8_t st=have_stage?mem->DebugRetrieve(stage_addr):0u;

        if(have_stage&&st!=last_stage) {
            if(last_stage>=1u&&last_stage<=6u&&loops_seen>=warmup)
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
                    if(loops_seen>=warmup) { loop_stats.add(now-loop_start); ++loops_measured; }
                    ++loops_seen;
                } else have_loop_start=true;
                loop_start=now;
            }
            if(p==3u&&have_cand_fast&&loops_seen>=warmup) {
                cand_total_stats.add(mem->DebugRetrieve(cand_total_addr));
                cand_uniform_stats.add(mem->DebugRetrieve(cand_uniform_addr));
                cand_reject_stats.add(mem->DebugRetrieve(cand_reject_addr));
                cand_replace_stats.add(mem->DebugRetrieve(cand_replace_addr));
                cand_fallback_stats.add(mem->DebugRetrieve(cand_fallback_addr));
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
    std::printf("TileSector Gearsystem profile: loops=%u warmup=%u instructions=%llu phase_addr=%04X",
                loop_stats.n,warmup,(unsigned long long)instructions,phase_addr);
    if(have_stage) std::printf(" render_stage_addr=%04X",stage_addr);
    std::printf("\n");
    const char* names[6]={"startup","input+motion","render/build","vsync-wait","VRAM-upload","loop-tail"};
    for(unsigned pidx=1;pidx<=5;++pidx) if(phase_stats[pidx].n) {
        std::printf("  %-12s avg=%8.1f T min=%5llu max=%5llu n=%u\n",names[pidx],phase_stats[pidx].avg(),
                    (unsigned long long)phase_stats[pidx].min,(unsigned long long)phase_stats[pidx].max,phase_stats[pidx].n);
    }
    if(have_stage) {
        const char* snames[7]={"idle","clear-map","q4-transform","candidate-build","solid-raster","portal-control","portal-face"};
        std::printf("  render/build subphases:\n");
        for(unsigned s=1;s<=6;++s) if(stage_stats[s].n) {
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
    if(cand_total_stats.n) {
        double total=(double)cand_total_stats.sum;
        std::printf("  candidate fast spans avg=%5.2f uniform=%5.2f (%.1f%%) reject=%5.2f (%.1f%%) replace=%5.2f (%.1f%%) fallback=%5.2f (%.1f%%)\n",
                    cand_total_stats.avg(),cand_uniform_stats.avg(),
                    total?100.0*(double)cand_uniform_stats.sum/total:0.0,
                    cand_reject_stats.avg(),
                    total?100.0*(double)cand_reject_stats.sum/total:0.0,
                    cand_replace_stats.avg(),
                    total?100.0*(double)cand_replace_stats.sum/total:0.0,
                    cand_fallback_stats.avg(),
                    total?100.0*(double)cand_fallback_stats.sum/total:0.0);
    }
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
    return 0;
}
