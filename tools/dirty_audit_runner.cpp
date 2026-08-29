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

static bool find_symbol(const char *path,const char *wanted,u16 &addr){
    std::ifstream f(path);std::string line;
    if(!f)return false;
    while(std::getline(f,line)){
        unsigned bank=0,a=0;char sym[256]={0};
        if(std::sscanf(line.c_str(),"%x:%x %255s",&bank,&a,sym)==3 &&
           std::strcmp(sym,wanted)==0){addr=(u16)a;return true;}
        if(std::sscanf(line.c_str(),"%x %255s",&a,sym)==2 &&
           std::strcmp(sym,wanted)==0){addr=(u16)a;return true;}
    }
    return false;
}
static bool sym2(const char *p,const char *a,const char *b,u16 &out){
    return find_symbol(p,a,out)||find_symbol(p,b,out);
}

int main(int argc,char **argv){
    if(argc<4){
        std::fprintf(stderr,"usage: %s rom.gg rom.sym target_patch_index\n",argv[0]);
        return 2;
    }
    const char *rom=argv[1],*syms=argv[2];
    const uint16_t target=(uint16_t)std::strtoul(argv[3],nullptr,0);

    u16 a_patch=0,a_frames=0,a_actual=0,a_missed=0,a_missed_frames=0;
    u16 a_row=0,a_col=0,a_min=0,a_max=0;
    if(!sym2(syms,"_g_patch_index","g_patch_index",a_patch) ||
       !sym2(syms,"_g_dirty_audit_frames","g_dirty_audit_frames",a_frames) ||
       !sym2(syms,"_g_dirty_audit_actual_changes","g_dirty_audit_actual_changes",a_actual) ||
       !sym2(syms,"_g_dirty_audit_missed_changes","g_dirty_audit_missed_changes",a_missed) ||
       !sym2(syms,"_g_dirty_audit_missed_frames","g_dirty_audit_missed_frames",a_missed_frames) ||
       !sym2(syms,"_g_dirty_audit_first_row","g_dirty_audit_first_row",a_row) ||
       !sym2(syms,"_g_dirty_audit_first_col","g_dirty_audit_first_col",a_col) ||
       !sym2(syms,"_g_dirty_audit_first_min","g_dirty_audit_first_min",a_min) ||
       !sym2(syms,"_g_dirty_audit_first_max","g_dirty_audit_first_max",a_max)){
        std::fprintf(stderr,"required dirty-audit symbols missing\n");
        return 3;
    }

    GearsystemCore core;core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)){std::fprintf(stderr,"LoadROM failed\n");return 4;}
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384);int samples=0;
    Memory *mem=core.GetMemory();

    auto rd16=[&](u16 a)->uint16_t{
        return (uint16_t)mem->DebugRetrieve(a)|
               ((uint16_t)mem->DebugRetrieve((u16)(a+1u))<<8);
    };

    unsigned vblanks=0;
    const unsigned limit=10000u;
    while(vblanks<limit){
        samples=0;
        core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true);
        ++vblanks;
        if(rd16(a_patch)>=target)break;
    }
    if(vblanks>=limit){
        std::fprintf(stderr,"target patch index not reached; current=%u\n",rd16(a_patch));
        return 5;
    }

    /* Let the target frame's post-VBlank upload/commit complete. */
    samples=0;core.RunToVBlank(fb.data(),audio.data(),&samples,nullptr,true);++vblanks;

    const uint16_t frames=rd16(a_frames);
    const uint16_t actual=rd16(a_actual);
    const uint16_t missed=rd16(a_missed);
    const uint16_t missed_frames=rd16(a_missed_frames);
    const uint8_t row=mem->DebugRetrieve(a_row);
    const uint8_t col=mem->DebugRetrieve(a_col);
    const uint8_t lo=mem->DebugRetrieve(a_min);
    const uint8_t hi=mem->DebugRetrieve(a_max);

    std::printf("DIRTY_AUDIT target_patch=%u reached_patch=%u vblanks=%u\n",
                target,rd16(a_patch),vblanks);
    std::printf("DIRTY_AUDIT frames=%u actual_changed_words=%u missed_changed_words=%u missed_frames=%u\n",
                frames,actual,missed,missed_frames);
    if(missed){
        std::printf("DIRTY_AUDIT first_miss row=%u col=%u advertised_min=%u advertised_max=%u\n",
                    row,col,lo,hi);
    }else{
        std::printf("DIRTY_AUDIT COVERAGE_EXACT_FOR_ALL_CHANGED_WORDS=1\n");
    }
    return 0;
}
