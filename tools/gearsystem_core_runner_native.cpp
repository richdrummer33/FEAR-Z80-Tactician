#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>
#include "GearsystemCore.h"
#include "Processor.h"
#include "Memory.h"
bool g_mcp_stdio_mode = false;

static void save_ppm(const char* path, const std::vector<u8>& fb, int w, int h) {
    FILE* f=fopen(path,"wb"); if(!f){perror("fopen"); return;}
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
        const u8* p=&fb[(y*GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN + x)*4];
        // GS_PIXEL_RGBA8888 buffer is byte R,G,B,A on this build.
        fwrite(p,1,3,f);
    }
    fclose(f);
}

int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: %s rom.gg frames [out.ppm] [dump_addr_hex] [dump_len]\n",argv[0]);return 2;}
    const char* rom=argv[1]; int frames=atoi(argv[2]);
    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);
    if(!core.LoadROM(rom)){fprintf(stderr,"LoadROM failed\n");return 3;}
    GS_RuntimeInfo ri{}; core.GetRuntimeInfo(ri);
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384); int samples=0;
    for(int i=0;i<frames;i++) { samples=0; core.RunToVBlank(fb.data(), audio.data(), &samples, nullptr, true); }
    auto *st=core.GetProcessor()->GetState();
    printf("frames=%d screen=%dx%d cycles=%llu PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X RAM[C000..C007]=",
        frames,ri.screen_width,ri.screen_height,(unsigned long long)core.GetMasterClockCycles(),
        st->PC->GetValue(),st->SP->GetValue(),st->AF->GetValue(),st->BC->GetValue(),st->DE->GetValue(),st->HL->GetValue());
    Memory* mem=core.GetMemory(); for(int i=0;i<8;i++) printf("%02X",mem->DebugRetrieve((u16)(0xC000+i))); printf("\n");
    if(argc>=4 && argv[3][0] != '-') {save_ppm(argv[3],fb,ri.screen_width,ri.screen_height);printf("saved=%s\n",argv[3]);}
    if(argc>=5) {
        unsigned addr = (unsigned)strtoul(argv[4], nullptr, 16);
        unsigned len = (argc>=6) ? (unsigned)strtoul(argv[5], nullptr, 0) : 32u;
        if(len > 2048u) len = 2048u;
        printf("DUMP[%04X..%04X]=", addr & 0xFFFFu, (addr + (len ? len - 1u : 0u)) & 0xFFFFu);
        for(unsigned i=0;i<len;i++) printf("%02X", mem->DebugRetrieve((u16)(addr+i)));
        printf("\n");
    }
    return 0;
}
