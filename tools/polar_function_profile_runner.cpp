#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <utility>
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
    /* 0xff = fixed/any mapped bank. For 0x4000..0x7fff code this is
       the Sega mapper bank that must be active for the range to match. */
    uint8_t bank=0xffu;
};

struct OwnershipProbe {
    const char* symbol;
    const char* label;
    u16 addr=0;
    bool found=false;
    uint64_t checks=0;
    uint64_t rejected=0;
};

/* Zero-ROM-overhead source-line profiling. SDCC -debug emits C$file$line$
 * labels for statements; use those labels only in the host profiler, rather
 * than planting timing stores in the Z80 hot path. This is especially useful
 * for project_envelope_span(), whose exclusive body is currently large enough
 * that function-level totals hide whether FOV clipping, coarse-column mapping,
 * or endpoint/depth setup is responsible. */
struct SourceLineRange {
    u16 lo=0,hi=0;
    unsigned line=0;
    uint64_t cycles=0;
    uint64_t instructions=0;
    uint64_t entries=0;
};
struct SourceProfile {
    std::string function;
    u16 lo=0,hi=0;
    uint8_t bank=0xffu;
    std::vector<SourceLineRange> lines;
};

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

/* The CPU only exposes a 16-bit PC, so code in switchable ROM banks aliases
 * the renderer's 0x4000..0x7fff addresses. The old profiler therefore charged
 * all bank-254 thin-face work to whichever bank-1 renderer symbol occupied the
 * same PC range (usually clamp_s8). Parse the thin-face bank from NoICE and
 * keep that bank identity alongside each PC range. */
static std::vector<Range> load_thinface_ranges(const char* noi) {
    std::ifstream f(noi);
    std::string line;
    struct Start { unsigned long raw; std::string name; };
    std::vector<Start> starts;
    std::regex local_re("^DEF Ftilesector_polar_thinface\\$([^$]+)\\$0\\$0 0x([0-9A-Fa-f]+)");
    std::regex global_re("^DEF G\\$(tsp_polar_(?:extra_seam_mask|record_subcolumn_boundary|seam_prepare_dirty|refine_seam_heights|subcolumn_seams_fast))\\$0\\$0 0x([0-9A-Fa-f]+)");
    std::regex end_re("^DEF XG\\$tsp_polar_subcolumn_seams_fast\\$0\\$0 0x([0-9A-Fa-f]+)");
    std::smatch m;
    unsigned long final_end=0;
    while(std::getline(f,line)) {
        if(std::regex_search(line,m,local_re)) {
            const unsigned long raw=std::strtoul(m[2].str().c_str(),nullptr,16);
            starts.push_back({raw,"thinface/"+m[1].str()});
        } else if(std::regex_search(line,m,global_re)) {
            const unsigned long raw=std::strtoul(m[2].str().c_str(),nullptr,16);
            starts.push_back({raw,"thinface/"+m[1].str()});
        } else if(std::regex_search(line,m,end_re)) {
            final_end=std::strtoul(m[1].str().c_str(),nullptr,16);
        }
    }
    std::sort(starts.begin(),starts.end(),[](const Start&a,const Start&b){return a.raw<b.raw;});
    starts.erase(std::unique(starts.begin(),starts.end(),[](const Start&a,const Start&b){return a.raw==b.raw;}),starts.end());

    std::vector<Range> out;
    for(size_t i=0;i<starts.size();++i) {
        const unsigned long raw=starts[i].raw;
        const uint8_t bank=(uint8_t)(raw>>16);
        if(bank==0u || bank==0xffu) continue;
        unsigned long next=(i+1<starts.size())?starts[i+1].raw:final_end;
        if((next>>16)!=bank || next<=raw) continue;
        Range r{(u16)raw,(u16)next,starts[i].name,0,0,0};
        r.bank=bank;
        out.push_back(std::move(r));
    }
    return out;
}

static std::vector<SourceLineRange> load_source_lines(const char* noi,u16 fn_lo,u16 fn_hi) {
    std::ifstream f(noi);
    std::string line;
    std::vector<std::pair<u16,unsigned>> starts;
    /* Typical SDCC NoICE form:
       DEF C$tilesector_polar_renderer.c$830$... 0x00014ABC */
    std::regex re("^DEF C\\$[^$]+\\$([0-9]+)\\$.* 0x([0-9A-Fa-f]+)");
    std::smatch m;
    while(std::getline(f,line)) {
        if(!std::regex_search(line,m,re)) continue;
        const unsigned src_line=(unsigned)std::strtoul(m[1].str().c_str(),nullptr,10);
        const unsigned long raw=std::strtoul(m[2].str().c_str(),nullptr,16);
        const u16 a=(u16)raw;
        if(a>=fn_lo && a<fn_hi) starts.push_back({a,src_line});
    }
    std::sort(starts.begin(),starts.end(),[](const auto&a,const auto&b){
        if(a.first!=b.first) return a.first<b.first;
        return a.second<b.second;
    });
    /* Several debug labels may share an address. One range per address is
       sufficient; choose the lowest source line as its stable display label. */
    std::vector<std::pair<u16,unsigned>> uniq;
    for(const auto &p:starts) {
        if(uniq.empty() || uniq.back().first!=p.first) uniq.push_back(p);
        else if(p.second<uniq.back().second) uniq.back().second=p.second;
    }
    std::vector<SourceLineRange> out;
    for(size_t i=0;i<uniq.size();++i) {
        const u16 lo=uniq[i].first;
        const u16 hi=(i+1<uniq.size())?uniq[i+1].first:fn_hi;
        if(hi>lo) out.push_back({lo,hi,uniq[i].second,0,0,0});
    }
    return out;
}

static bool source_profile_tick(SourceProfile& sp,u16 pc,uint8_t mapped_bank,uint64_t dt) {
    if(pc<sp.lo || pc>=sp.hi || sp.lines.empty()) return false;
    if(sp.bank!=0xffu && sp.bank!=mapped_bank) return false;
    size_t lo=0,hi=sp.lines.size();
    while(lo<hi) {
        const size_t m=(lo+hi)>>1;
        if(pc<sp.lines[m].lo) hi=m;
        else if(pc>=sp.lines[m].hi) lo=m+1;
        else {
            sp.lines[m].cycles+=dt;
            ++sp.lines[m].instructions;
            if(pc==sp.lines[m].lo) ++sp.lines[m].entries;
            return true;
        }
    }
    return false;
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
        if(hi>lo) {
            Range r{lo,hi,starts[i].second,0,0,0};
            r.bank=1u;
            out.push_back(std::move(r));
        }
    }
    if(render_start && render_end>render_start) {
        Range r{render_start,render_end,"tsp_polar_render(self)",0,0,0};
        r.bank=1u;
        out.push_back(std::move(r));
    }

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
        if(name.rfind("_tsp_polar_",0)!=0 && name.rfind("_tsp_h_",0)!=0 && name.rfind("_e1env_",0)!=0) continue;
        u16 lo=fixed[i].first,hi=(u16)(lo+1u);
        for(size_t j=i+1;j<fixed.size();++j) {
            if(fixed[j].first>lo) { hi=fixed[j].first; break; }
        }
        if(hi>lo) out.push_back({lo,hi,name.substr(1),0,0});
    }
    auto thin=load_thinface_ranges(noi);
    out.insert(out.end(),thin.begin(),thin.end());
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
    std::vector<SourceProfile> source_profiles;
    const char* source_targets[] = {
        "project_envelope_span", "envelope_focus_span", "envelope_add_span", "draw_run"
    };
    for(const char* wanted:source_targets) {
        for(const auto &r:ranges) {
            if(r.name==wanted) {
                SourceProfile sp;
                sp.function=wanted; sp.lo=r.lo; sp.hi=r.hi; sp.bank=r.bank;
                sp.lines=load_source_lines(noi,r.lo,r.hi);
                if(!sp.lines.empty()) source_profiles.push_back(std::move(sp));
                break;
            }
        }
    }
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
            /* Sega mapper slot 1 (0x4000..0x7fff) is selected by 0xfffe.
               Reading the mirrored mapper register lets the host distinguish
               bank-1 renderer code from bank-254 thin-face code at the same PC. */
            const uint8_t mapped_bank=(pc>=0x4000u && pc<0x8000u) ?
                                      mem->DebugRetrieve(0xfffeu) : 0xffu;
            for(auto &p:ownership) if(p.found && pc==p.addr) {
                ++p.checks;
                if(reg_a==0u) ++p.rejected;
            }
            total_render_cycles+=dt; ++total_render_ins;
            bool hit=false;
            for(auto &r:ranges) {
                if(pc>=r.lo && pc<r.hi && (r.bank==0xffu || r.bank==mapped_bank)) {
                    r.cycles+=dt; ++r.instructions;
                    if(pc==r.lo) ++r.entries;
                    hit=true; break;
                }
            }
            for(auto &sp:source_profiles) source_profile_tick(sp,pc,mapped_bank,dt);
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
    for(const auto &sp:source_profiles) {
        uint64_t fn_cycles=0;
        for(const auto &lr:sp.lines) fn_cycles+=lr.cycles;
        if(!fn_cycles) continue;
        auto ranked=sp.lines;
        std::sort(ranked.begin(),ranked.end(),[](const SourceLineRange&a,const SourceLineRange&b){
            return a.cycles>b.cycles;
        });
        std::printf("Source-line profile: %s (zero ROM instrumentation)\n",sp.function.c_str());
        unsigned shown=0;
        for(const auto &lr:ranked) {
            if(!lr.cycles) continue;
            std::printf("  line %-5u avg/update=%10.1f T share(fn)=%6.2f%% entries/update=%7.2f ins=%llu\n",
                        lr.line,(double)lr.cycles/measured,
                        100.0*(double)lr.cycles/(double)fn_cycles,
                        (double)lr.entries/measured,
                        (unsigned long long)lr.instructions);
            if(++shown>=16u) break;
        }
    }
    std::printf("Polar ownership-mask probes (zero ROM instructions; sampled at exported labels):\n");
    for(const auto &p:ownership) if(p.found) {
        const double pct=p.checks?100.0*(double)p.rejected/(double)p.checks:0.0;
        std::printf("  %-18s checks/update=%7.2f rejected/update=%7.2f reject=%6.2f%%\n",
                    p.label,(double)p.checks/measured,(double)p.rejected/measured,pct);
    }
    return 0;
}
