#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "GearsystemCore.h"
#include "Memory.h"
#include "Processor.h"

bool g_mcp_stdio_mode = false;

struct Sym { u16 addr; std::string name; };

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

static std::vector<Sym> load_symbols(const char* path) {
    std::ifstream f(path);
    std::string line;
    std::vector<Sym> out;
    while(std::getline(f,line)) {
        unsigned bank=0,a=0;
        char name[256]={0};
        if(std::sscanf(line.c_str(),"%x:%x %255s",&bank,&a,name)==3 && bank==0)
            out.push_back({(u16)a,name});
    }
    std::sort(out.begin(),out.end(),[](const Sym&a,const Sym&b){return a.addr<b.addr;});
    return out;
}

static std::string near_symbol(const std::vector<Sym>& syms,u16 pc) {
    const Sym* best=nullptr;
    for(const auto& s:syms) {
        if(s.addr>pc) break;
        best=&s;
    }
    if(!best) return "?";
    char buf[384];
    std::snprintf(buf,sizeof(buf),"%s+0x%X",best->name.c_str(),(unsigned)(pc-best->addr));
    return buf;
}

static uint16_t rd16(Memory* mem,u16 a) {
    return (uint16_t)mem->DebugRetrieve(a) |
           ((uint16_t)mem->DebugRetrieve((u16)(a+1u))<<8);
}

int main(int argc,char**argv) {
    if(argc<7) {
        std::fprintf(stderr,"usage: %s rom.gg rom.sym warmup scenario row col [max_writes=256]\n",argv[0]);
        return 2;
    }
    const char* rom=argv[1];
    const char* sym=argv[2];
    unsigned warmup=(unsigned)std::strtoul(argv[3],nullptr,0);
    std::string scenario=argv[4];
    unsigned row=(unsigned)std::strtoul(argv[5],nullptr,0);
    unsigned col=(unsigned)std::strtoul(argv[6],nullptr,0);
    unsigned max_writes=(argc>7)?(unsigned)std::strtoul(argv[7],nullptr,0):256u;
    if(row>=18u || col>=20u) { std::fprintf(stderr,"row/col out of range\n"); return 2; }
    if(scenario!="demo" && scenario!="roomA-turn") { std::fprintf(stderr,"bad scenario\n"); return 2; }

    u16 phase_addr=0,stage_addr=0,map_addr=0,raster_addr=0,name_addr=0,symfull_addr=0,nt_mark_addr=0;
    u16 ret_total_addr=0,ret_skip_addr=0,ret_edge_addr=0,span_total_addr=0,span_skip_addr=0;
    if(!find_symbol(sym,"_g_ts_prof_phase",phase_addr)&&!find_symbol(sym,"g_ts_prof_phase",phase_addr)) return 3;
    if(!find_symbol(sym,"_g_map",map_addr)&&!find_symbol(sym,"g_map",map_addr)) return 3;
    const bool have_stage=find_symbol(sym,"_g_ts_render_stage",stage_addr)||find_symbol(sym,"g_ts_render_stage",stage_addr);
    const bool have_ctx=find_symbol(sym,"_g_raster_ctx",raster_addr)&&find_symbol(sym,"_g_name_run_ctx",name_addr);
    const bool have_symfull=find_symbol(sym,"_ts_raster_symfull_column_fast",symfull_addr);
    const bool have_nt_mark=find_symbol(sym,"_ts_nt_mark_span",nt_mark_addr);
    const bool have_ret=
        find_symbol(sym,"_g_ts_ret_full_total",ret_total_addr)&&
        find_symbol(sym,"_g_ts_ret_full_skip",ret_skip_addr)&&
        find_symbol(sym,"_g_ts_ret_full_edgeonly",ret_edge_addr)&&
        find_symbol(sym,"_g_ts_ret_span_total",span_total_addr)&&
        find_symbol(sym,"_g_ts_ret_span_skip",span_skip_addr);
    const u16 target=(u16)(map_addr + (row*20u+col)*2u);
    auto syms=load_symbols(sym);

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

    if(scenario=="roomA-turn") core.KeyPressed(Joypad_1,Key_Right);

    uint8_t last_phase=mem->DebugRetrieve(phase_addr);
    unsigned phase1_seen=0;
    unsigned writes=0;
    bool target_call_active=false;
    u16 target_return_pc=0;
    unsigned target_calls=0;
    unsigned nt_marks_frame=0,nt_marks_total=0;
    uint32_t nt_mark_cols_frame=0;
    uint64_t instructions=0;
    const uint64_t instruction_limit=30000000ull;
    std::printf("TRACE target row=%u col=%u map=%04X addr=%04X initial=%04X warmup=%u scenario=%s\n",
                row,col,map_addr,target,rd16(mem,target),warmup,scenario.c_str());

    while(phase1_seen < warmup+2u && instructions < instruction_limit) {
        auto *st=cpu->GetState();
        const u16 pc=st->PC->GetValue();
        const u16 af=st->AF->GetValue(),bc=st->BC->GetValue(),de=st->DE->GetValue(),hl=st->HL->GetValue();
        const u16 ix=st->IX->GetValue(),iy=st->IY->GetValue(),sp=st->SP->GetValue();
        const u16 stack0=rd16(mem,sp);
        const uint8_t phase_before=mem->DebugRetrieve(phase_addr);
        const uint8_t stage_before=have_stage?mem->DebugRetrieve(stage_addr):0u;
        const uint16_t before=rd16(mem,target);
        const uint8_t op0=mem->DebugRetrieve(pc);
        const uint8_t op1=mem->DebugRetrieve((u16)(pc+1u));
        const uint8_t op2=mem->DebugRetrieve((u16)(pc+2u));
        const uint8_t op3=mem->DebugRetrieve((u16)(pc+3u));

        if(have_nt_mark && pc==nt_mark_addr) {
            ++nt_marks_frame;
            ++nt_marks_total;
            const unsigned mark_col=(bc>>8)&0xffu;
            if(mark_col<20u) nt_mark_cols_frame |= (uint32_t)1u<<mark_col;
        }

        if(have_symfull && pc==symfull_addr && ((af>>8)&0xffu)==col) {
            ++target_calls;
            target_call_active=true;
            target_return_pc=stack0;
            const int16_t top_l=have_ctx?(int16_t)rd16(mem,(u16)(raster_addr+2u)):0;
            const int16_t top_r=have_ctx?(int16_t)rd16(mem,(u16)(raster_addr+4u)):0;
            const int16_t bot_l=have_ctx?(int16_t)rd16(mem,(u16)(raster_addr+6u)):0;
            const int16_t bot_r=have_ctx?(int16_t)rd16(mem,(u16)(raster_addr+8u)):0;
            const u16 clip_top_p=have_ctx?rd16(mem,(u16)(raster_addr+11u)):0;
            const u16 clip_bot_p=have_ctx?rd16(mem,(u16)(raster_addr+13u)):0;
            const unsigned clip_top=have_ctx?mem->DebugRetrieve(clip_top_p):0;
            const unsigned clip_bot=have_ctx?mem->DebugRetrieve(clip_bot_p):0;
            std::printf(
                "SYM_ENTRY %03u loopBoundary=%u phase=%u stage=%u col=%u PC=%04X RET=%04X "
                "profile=%u shade=%u top=%d/%d bot=%d/%d border=%u clip=%u..%u retain_ok=%u "
                "AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X",
                target_calls,phase1_seen,phase_before,stage_before,col,pc,stack0,
                have_ctx?mem->DebugRetrieve(raster_addr):0,
                have_ctx?mem->DebugRetrieve((u16)(raster_addr+1u)):0,
                (int)top_l,(int)top_r,(int)bot_l,(int)bot_r,
                have_ctx?mem->DebugRetrieve((u16)(raster_addr+10u)):0,
                clip_top,clip_bot,
                have_ctx?mem->DebugRetrieve((u16)(name_addr+14u)):0,
                af,bc,de,hl,ix,iy,sp);
            if(have_ret)
                std::printf(" ret=%u/%u/%u span=%u/%u",
                    mem->DebugRetrieve(ret_total_addr),mem->DebugRetrieve(ret_skip_addr),
                    mem->DebugRetrieve(ret_edge_addr),mem->DebugRetrieve(span_total_addr),
                    mem->DebugRetrieve(span_skip_addr));
            std::printf("\n");
        } else if(target_call_active && pc==target_return_pc) {
            std::printf(
                "SYM_RETURN %03u loopBoundary=%u phase=%u stage=%u A=%02X target=%04X "
                "AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X",
                target_calls,phase1_seen,phase_before,stage_before,(unsigned)((af>>8)&0xffu),
                before,af,bc,de,hl,ix,iy,sp);
            if(have_ret)
                std::printf(" ret=%u/%u/%u span=%u/%u",
                    mem->DebugRetrieve(ret_total_addr),mem->DebugRetrieve(ret_skip_addr),
                    mem->DebugRetrieve(ret_edge_addr),mem->DebugRetrieve(span_total_addr),
                    mem->DebugRetrieve(span_skip_addr));
            std::printf("\n");
            target_call_active=false;
        }

        samples=0;
        core.RunToVBlank(fb.data(),audio.data(),&samples,&dbg,false);
        ++instructions;

        const uint16_t after=rd16(mem,target);
        if(after!=before && writes<max_writes) {
            ++writes;
            std::printf(
                "WRITE %03u loopBoundary=%u phase=%u stage=%u PC=%04X %-42s OP=%02X %02X %02X %02X "
                "word=%04X->%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X SP=%04X RET=%04X\n",
                writes,phase1_seen,phase_before,stage_before,pc,near_symbol(syms,pc).c_str(),
                op0,op1,op2,op3,before,after,af,bc,de,hl,ix,iy,sp,stack0);
        }

        const uint8_t p=mem->DebugRetrieve(phase_addr);
        if(p!=last_phase) {
            if(p==1u) {
                ++phase1_seen;
                std::printf("BOUNDARY phase1_seen=%u target=%04X cycles=%llu",
                            phase1_seen,rd16(mem,target),(unsigned long long)core.GetMasterClockCycles());
                if(have_ret)
                    std::printf(" ret=%u/%u/%u span=%u/%u",
                        mem->DebugRetrieve(ret_total_addr),mem->DebugRetrieve(ret_skip_addr),
                        mem->DebugRetrieve(ret_edge_addr),mem->DebugRetrieve(span_total_addr),
                        mem->DebugRetrieve(span_skip_addr));
                if(have_nt_mark) {
                    unsigned unique=0,mask=nt_mark_cols_frame;
                    while(mask){ unique += mask&1u; mask >>= 1u; }
                    std::printf(" ntmarks=%u cols=%u mask=%05X",nt_marks_frame,unique,(unsigned)nt_mark_cols_frame);
                    nt_marks_frame=0;
                    nt_mark_cols_frame=0;
                }
                std::printf("\n");
            }
            last_phase=p;
        }
    }

    auto *st=cpu->GetState();
    std::printf("TRACE_DONE phase1_seen=%u writes=%u ntmarks_total=%u instructions=%llu final=%04X PC=%04X SP=%04X\n",
                phase1_seen,writes,nt_marks_total,(unsigned long long)instructions,rd16(mem,target),
                st->PC->GetValue(),st->SP->GetValue());
    if(instructions>=instruction_limit) return 5;
    return 0;
}
