#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
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

static double percentile(std::vector<uint64_t> v,double q) {
    if(v.empty()) return 0.0;
    std::sort(v.begin(),v.end());
    double pos=q*(double)(v.size()-1);
    size_t lo=(size_t)pos,hi=std::min(lo+1,v.size()-1);
    double f=pos-(double)lo;
    return (double)v[lo]+((double)v[hi]-(double)v[lo])*f;
}

int main(int argc,char**argv) {
    if(argc<3) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym [samples=120] [warmup=8] [csv] [mapdump] [marker_symbol] [marker_width=8]\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1];
    const char* sym=argv[2];
    unsigned target=(argc>3)?(unsigned)std::strtoul(argv[3],nullptr,0):120u;
    unsigned warmup=(argc>4)?(unsigned)std::strtoul(argv[4],nullptr,0):8u;
    const char* csv_path=(argc>5)?argv[5]:nullptr;
    const char* mapdump_path=(argc>6)?argv[6]:nullptr;
    std::ofstream mapdump;
    if(mapdump_path) {
        mapdump.open(mapdump_path,std::ios::binary|std::ios::trunc);
        if(!mapdump) {
            std::fprintf(stderr,"cannot open map dump: %s\n",mapdump_path);
            return 2;
        }
    }

    u16 state_addr=0,map_addr=0;
    if(!find_symbol(sym,"_g_state",state_addr)&&!find_symbol(sym,"g_state",state_addr)) {
        std::fprintf(stderr,"g_state not found in %s\n",sym);
        return 3;
    }
    const bool have_map=find_symbol(sym,"_g_map",map_addr)||find_symbol(sym,"g_map",map_addr);

    GearsystemCore core;
    core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)) {
        std::fprintf(stderr,"LoadROM failed\n");
        return 4;
    }
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

    // Default zero-hook marker: TSPState.demo_ticks low byte at packed offset
    // 14. For host-compiled patch-player ROMs we can instead watch an existing
    // runtime variable such as _g_patch_index. This remains zero-hook: the
    // profiler only observes normal program state and adds no ROM stores.
    u16 marker_addr=(u16)(state_addr+14u);
    const char* marker_name="g_state.demo_ticks";
    unsigned marker_width=8u;
    if(argc>7 && std::strcmp(argv[7],"-")!=0) {
        marker_name=argv[7];
        if(!find_symbol(sym,marker_name,marker_addr)) {
            std::fprintf(stderr,"marker symbol %s not found in %s\n",marker_name,sym);
            return 3;
        }
        marker_width=(argc>8)?(unsigned)std::strtoul(argv[8],nullptr,0):16u;
        if(marker_width!=8u && marker_width!=16u) {
            std::fprintf(stderr,"marker_width must be 8 or 16\n");
            return 2;
        }
    }
    auto read_marker=[&]()->uint16_t {
        if(marker_width==8u) return mem->DebugRetrieve(marker_addr);
        return (uint16_t)mem->DebugRetrieve(marker_addr) |
               ((uint16_t)mem->DebugRetrieve((u16)(marker_addr+1u))<<8);
    };
    uint16_t marker=read_marker();
    uint64_t last_cycle=0;
    bool have_last=false;
    unsigned events=0;
    std::vector<uint64_t> dt;
    dt.reserve(target);
    uint64_t instructions=0;
    const uint64_t instruction_limit=100000000ull;
    uint64_t map_hash=1469598103934665603ull;
    unsigned hash_frames=0;

    while(dt.size()<target && instructions<instruction_limit) {
        samples=0;
        core.RunToVBlank(fb.data(),audio.data(),&samples,&dbg,false);
        ++instructions;
        const uint16_t m=read_marker();
        if(m!=marker) {
            const uint64_t now=core.GetMasterClockCycles();
            marker=m;
            if(have_last) {
                if(events>=warmup) {
                    dt.push_back(now-last_cycle);
                    if(have_map) {
                        for(unsigned i=0;i<720u;++i) {
                            const uint8_t v=mem->DebugRetrieve((u16)(map_addr+i));
                            map_hash^=(uint64_t)v;
                            map_hash*=1099511628211ull;
                            if(mapdump) mapdump.put((char)v);
                        }
                        ++hash_frames;
                    }
                }
                ++events;
            } else {
                have_last=true;
            }
            last_cycle=now;
        }
    }

    if(dt.size()!=target) {
        std::fprintf(stderr,"only %zu/%u update intervals captured; marker=%s@%04X value=%u instructions=%llu\n",
                     dt.size(),target,marker_name,marker_addr,(unsigned)marker,(unsigned long long)instructions);
        return 5;
    }

    const double cpu_hz=3579545.0;
    const double mean=std::accumulate(dt.begin(),dt.end(),0.0)/(double)dt.size();
    double m2=0.0,m3=0.0;
    uint64_t minv=dt[0],maxv=dt[0];
    for(uint64_t v:dt) {
        const double d=(double)v-mean;
        m2+=d*d; m3+=d*d*d;
        minv=std::min(minv,v); maxv=std::max(maxv,v);
    }
    const double var=m2/(double)dt.size();
    const double sd=std::sqrt(var);
    const double skew=(sd>0.0)?(m3/(double)dt.size())/(sd*sd*sd):0.0;
    unsigned above2=0,above3=0;
    for(uint64_t v:dt) {
        if((double)v>mean+2.0*sd) ++above2;
        if((double)v>mean+3.0*sd) ++above3;
    }
    const double p50=percentile(dt,0.50),p90=percentile(dt,0.90);
    const double p95=percentile(dt,0.95),p99=percentile(dt,0.99);

    std::printf("Polar external zero-hook cadence profile: samples=%zu warmup=%u instructions=%llu state_addr=%04X marker=%s@%04X/%ubit\n",
                dt.size(),warmup,(unsigned long long)instructions,state_addr,marker_name,marker_addr,marker_width);
    std::printf("  update T     mean=%10.1f sd=%9.1f min=%llu p50=%.1f p90=%.1f p95=%.1f p99=%.1f max=%llu skew=%+.3f\n",
                mean,sd,(unsigned long long)minv,p50,p90,p95,p99,(unsigned long long)maxv,skew);
    std::printf("  update time  mean=%8.3f ms sd=%7.3f ms -> %.3f effective rendered FPS/updates-s\n",
                mean*1000.0/cpu_hz,sd*1000.0/cpu_hz,cpu_hz/mean);
    std::printf("  outliers     >mean+2sigma=%u (%.2f%%) >mean+3sigma=%u (%.2f%%)\n",
                above2,100.0*above2/dt.size(),above3,100.0*above3/dt.size());
    if(have_map)
        std::printf("  map-hash     frames=%u fnv64=%016llX\n",hash_frames,(unsigned long long)map_hash);
    const int16_t xq=(int16_t)rd16(state_addr+0u);
    const int16_t yq=(int16_t)rd16(state_addr+2u);
    const uint8_t yaw=mem->DebugRetrieve((u16)(state_addr+4u));
    std::printf("  motion last  x=%.2f y=%.2f yaw=%u demo_ticks=%u\n",
                xq/16.0,yq/16.0,yaw,mem->DebugRetrieve(marker_addr));

    if(csv_path) {
        std::ofstream csv(csv_path,std::ios::trunc);
        if(!csv) {
            std::fprintf(stderr,"cannot open csv: %s\n",csv_path);
            return 6;
        }
        csv<<"sample,tstates,ms\n";
        for(size_t i=0;i<dt.size();++i)
            csv<<i<<','<<dt[i]<<','<<((double)dt[i]*1000.0/cpu_hz)<<'\n';
    }
    return 0;
}
