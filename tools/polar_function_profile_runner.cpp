#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <vector>
#include "GearsystemCore.h"
#include "Memory.h"
#include "Processor.h"

bool g_mcp_stdio_mode = false;

struct Range {
    u16 lo,hi;
    std::string name;
    uint64_t cycles=0;
    uint64_t instructions=0;
    uint64_t entries=0;
};

struct OwnershipProbe {
    const char* symbol;
    const char* label;
    u16 addr=0;
    bool found=false;
    uint64_t checks=0;
    uint64_t rejected=0;
};

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

static std::vector<std::pair<u16,std::string>> load_fixed_symbols(const char* path) {
    std::ifstream f(path);
    std::string line;
    std::vector<std::pair<u16,std::string>> out;
    while(std::getline(f,line)) {
        unsigned bank=0,a=0;
        char sym[256]={0};
        if(std::sscanf(line.c_str(),"%x:%x %255s",&bank,&a,sym)==3) {
            if(bank==0 && a<0x4000u) out.push_back({(u16)a,sym});
        } else if(std::sscanf(line.c_str(),"%x %255s",&a,sym)==2) {
            if(a<0x4000u) out.push_back({(u16)a,sym});
        }
    }
    std::sort(out.begin(),out.end(),[](const auto&a,const auto&b){return a.first<b.first;});
    return out;
}

static std::vector<Range> load_polar_ranges(const char* noi,const char* sym) {
    std::ifstream f(noi);
    std::string line;
    std::vector<std::pair<u16,std::string>> starts;
    std::regex re("^DEF Ftilesector_polar_renderer\\$([^$]+)\\$0\\$0 0x([0-9A-Fa-f]+)");
    std::regex render_re("^DEF G\\$tsp_polar_render\\$0\\$0 0x([0-9A-Fa-f]+)");
    std::smatch m;
    u16 render_start=0,render_end=0;
    while(std::getline(f,line)) {
        if(std::regex_search(line,m,re)) {
            unsigned long a=std::strtoul(m[2].str().c_str(),nullptr,16);
            // Only actual banked renderer code, not absolute constants/data aliases.
            if(a>=0x14000ul && a<0x18000ul)
                starts.push_back({(u16)a,m[1].str()});
        } else if(std::regex_search(line,m,render_re)) {
            render_start=(u16)std::strtoul(m[1].str().c_str(),nullptr,16);
        } else if(line.rfind("DEF XG$tsp_polar_render$0$0 ",0)==0) {
            const char* p=std::strstr(line.c_str(),"0x");
            if(p) render_end=(u16)std::strtoul(p,nullptr,16);
        }
    }
    std::sort(starts.begin(),starts.end());
    std::vector<Range> out;
    for(size_t i=0;i<starts.size();++i) {
        u16 lo=starts[i].first;
        u16 hi=(i+1<starts.size())?starts[i+1].first:render_start;
        if(hi>lo) out.push_back({lo,hi,starts[i].second,0,0});
    }
    if(render_start && render_end>render_start)
        out.push_back({render_start,render_end,"tsp_polar_render(self)",0,0});

    // Fixed-bank attribution must follow the CURRENT link. The previous profiler
    // hardcoded addresses from an older ROM and eventually mislabeled unrelated
    // assembly as "__mullong" even when no __mullong symbol was linked.
    u16 a=0;
    if(find_symbol(sym,"__mulint",a)) out.push_back({a,(u16)(a+2u),"__mulint(wrapper)",0,0});
    if(find_symbol(sym,"__mul16",a)) out.push_back({a,(u16)(a+0x15u),"__mul16",0,0});
    if(find_symbol(sym,".memset_simple",a)) out.push_back({a,(u16)(a+0x15u),"memset",0,0});
    if(find_symbol(sym,"__mullong",a)) out.push_back({a,(u16)(a+0x72u),"__mullong",0,0});

    // Attribute exported polar assembly entrypoints by linked symbol order.
    // This captures the run/materializer/name-table modules without assuming
    // that SDCC/linker placement is stable from one optimization pass to the next.
    auto fixed=load_fixed_symbols(sym);
    for(size_t i=0;i<fixed.size();++i) {
        const auto &name=fixed[i].second;
        if(name.rfind("_tsp_polar_",0)!=0) continue;
        u16 lo=fixed[i].first,hi=(u16)(lo+1u);
        for(size_t j=i+1;j<fixed.size();++j) {
            if(fixed[j].first>lo) { hi=fixed[j].first; break; }
        }
        if(hi>lo) out.push_back({lo,hi,name.substr(1),0,0});
    }
    return out;
}

int main(int argc,char**argv) {
    if(argc<4) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym rom.noi [updates=60] [warmup=8]\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1];
    const char* sym=argv[2];
    const char* noi=argv[3];
    unsigned target=(argc>4)?(unsigned)std::strtoul(argv[4],nullptr,0):60u;
    unsigned warmup=(argc>5)?(unsigned)std::strtoul(argv[5],nullptr,0):8u;

    u16 phase_addr=0;
    if(!find_symbol(sym,"_g_ts_prof_phase",phase_addr)&&!find_symbol(sym,"g_ts_prof_phase",phase_addr)) {
        std::fprintf(stderr,"phase symbol missing\n"); return 3;
    }
    auto ranges=load_polar_ranges(noi,sym);
    std::vector<OwnershipProbe> ownership = {
        {"_tsp_polar_p_span","whole-span"},
        {"_tsp_polar_p_edge","generic-edge-row"},
        {"_tsp_polar_p_cap","generic-cap-row"},
        {"_tsp_polar_p_fill","interior-row"},
        {"_tsp_polar_p_symtop","sym-top-row"},
        {"_tsp_polar_p_symbot","sym-bottom-row"},
    };
    for(auto &p:ownership) p.found=find_symbol(sym,p.symbol,p.addr);

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
    Processor* cpu=core.GetProcessor();

    unsigned loops=0,measured=0;
    uint8_t last_phase=mem->DebugRetrieve(phase_addr);
    uint64_t total_render_cycles=0,total_render_ins=0,unassigned_cycles=0,unassigned_ins=0;
    const uint64_t instruction_limit=100000000ull;
    uint64_t instructions=0;

    while(measured<target && instructions<instruction_limit) {
        auto *st=cpu->GetState();
        const u16 pc=st->PC->GetValue();
        const uint8_t p_before=mem->DebugRetrieve(phase_addr);
        const uint64_t before=core.GetMasterClockCycles();

        samples=0;
        core.RunToVBlank(fb.data(),audio.data(),&samples,&dbg,false);
        ++instructions;
        const uint64_t after=core.GetMasterClockCycles();
        const uint64_t dt=after-before;

        if(p_before==2u && loops>=warmup) {
            const uint8_t reg_a=(uint8_t)(st->AF->GetValue()>>8);
            for(auto &p:ownership) if(p.found && pc==p.addr) {
                ++p.checks;
                if(reg_a==0u) ++p.rejected;
            }
            total_render_cycles+=dt; ++total_render_ins;
            bool hit=false;
            for(auto &r:ranges) {
                if(pc>=r.lo && pc<r.hi) {
                    r.cycles+=dt; ++r.instructions;
                    if(pc==r.lo) ++r.entries;
                    hit=true; break;
                }
            }
            if(!hit) { unassigned_cycles+=dt; ++unassigned_ins; }
        }

        const uint8_t p=mem->DebugRetrieve(phase_addr);
        if(p!=last_phase) {
            if(p==1u) {
                if(loops>=warmup) ++measured;
                ++loops;
            }
            last_phase=p;
        }
    }

    if(!total_render_cycles) { std::fprintf(stderr,"no render cycles captured\n"); return 5; }
    std::sort(ranges.begin(),ranges.end(),[](const Range&a,const Range&b){return a.cycles>b.cycles;});
    std::printf("Polar render PC profile: measured_updates=%u render_cycles=%llu render_instructions=%llu\n",
                measured,(unsigned long long)total_render_cycles,(unsigned long long)total_render_ins);
    for(const auto&r:ranges) if(r.cycles) {
        std::printf("  %-28s total=%12llu T avg/update=%10.1f T share=%6.2f%% calls/update=%7.2f ins=%llu\n",
                    r.name.c_str(),(unsigned long long)r.cycles,(double)r.cycles/measured,
                    100.0*(double)r.cycles/total_render_cycles,(double)r.entries/measured,
                    (unsigned long long)r.instructions);
    }
    std::printf("  %-28s total=%12llu T avg/update=%10.1f T share=%6.2f%% ins=%llu\n",
                "unassigned / callees",(unsigned long long)unassigned_cycles,
                (double)unassigned_cycles/measured,
                100.0*(double)unassigned_cycles/total_render_cycles,
                (unsigned long long)unassigned_ins);
    std::printf("Polar ownership-mask probes (zero ROM instructions; sampled at exported labels):\n");
    for(const auto &p:ownership) if(p.found) {
        const double pct=p.checks?100.0*(double)p.rejected/(double)p.checks:0.0;
        std::printf("  %-18s checks/update=%7.2f rejected/update=%7.2f reject=%6.2f%%\n",
                    p.label,(double)p.checks/measured,(double)p.rejected/measured,pct);
    }
    return 0;
}
