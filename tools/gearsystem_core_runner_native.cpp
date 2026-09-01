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
    /* Gearsystem's rendered output buffer is tightly packed at the active
     * runtime width. The allocation is max-overscan sized, but using that max
     * width as the row pitch shears Game Gear screenshots after row zero. */
    for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
        const u8* p=&fb[((size_t)y*(size_t)w + (size_t)x)*4u];
        // GS_PIXEL_RGBA8888 buffer is byte R,G,B,A on this build.
        fwrite(p,1,3,f);
    }
    fclose(f);
}

int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: %s rom.gg frames [out.ppm] [dump_addr_hex|-] [dump_len] [vram_addr_hex] [vram_len]\n",argv[0]);return 2;}
    const char* rom=argv[1]; int frames=atoi(argv[2]);
    GearsystemCore core; core.Init(GS_PIXEL_RGBA8888);

    /* Match the libretro cartridge-loading path exactly. The filename loader
     * fails to execute this project's 4 MiB generated cartridge correctly,
     * while LoadROMFromBuffer() is the path libretro uses successfully. */
    std::ifstream rom_file(rom,std::ios::binary);
    if(!rom_file){perror("rom");return 3;}
    std::vector<u8> rom_bytes((std::istreambuf_iterator<char>(rom_file)),{});
    if(rom_bytes.empty() ||
       !core.LoadROMFromBuffer(rom_bytes.data(),(int)rom_bytes.size(),nullptr,rom)){
        fprintf(stderr,"LoadROMFromBuffer failed\n");return 3;
    }
    printf("load_mode=buffer rom_bytes=%zu\n",rom_bytes.size());
    GS_RuntimeInfo ri{}; core.GetRuntimeInfo(ri);
    std::vector<u8> fb(GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN*GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN*4);
    std::vector<s16> audio(16384); int samples=0;
    Memory* mem=core.GetMemory();

    /* Optional exact logical-frame capture. The ROM may spend more than one
     * display VBlank applying one baked packet; waiting on a live Z80 marker
     * keeps framebuffer captures aligned with host-bake frame numbers.
     * Dormant unless both environment variables are supplied. */
    const char *wait_addr_env=getenv("GS_WAIT_ADDR");
    const char *wait_u16_env=getenv("GS_WAIT_U16_GE");
    bool wait_marker=(wait_addr_env&&wait_u16_env);
    u16 wait_addr=wait_marker?(u16)strtoul(wait_addr_env,nullptr,16):0u;
    unsigned wait_target=wait_marker?(unsigned)strtoul(wait_u16_env,nullptr,0):0u;
    int ran=0;
    for(;ran<frames;ran++) {
        samples=0;
        core.RunToVBlank(fb.data(), audio.data(), &samples, nullptr, true);
        if(wait_marker){
            unsigned v=(unsigned)mem->DebugRetrieve(wait_addr) |
                       ((unsigned)mem->DebugRetrieve((u16)(wait_addr+1u))<<8);
            if(v>=wait_target){++ran;break;}
        }
    }
    auto *st=core.GetProcessor()->GetState();
    printf("frames=%d ran=%d screen=%dx%d cycles=%llu PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X RAM[C000..C007]=",
        frames,ran,ri.screen_width,ri.screen_height,(unsigned long long)core.GetMasterClockCycles(),
        st->PC->GetValue(),st->SP->GetValue(),st->AF->GetValue(),st->BC->GetValue(),st->DE->GetValue(),st->HL->GetValue());
    for(int i=0;i<8;i++) printf("%02X",mem->DebugRetrieve((u16)(0xC000+i))); printf("\n");
    if(argc>=4 && argv[3][0] != '-') {save_ppm(argv[3],fb,ri.screen_width,ri.screen_height);printf("saved=%s\n",argv[3]);}
    if(argc>=5 && argv[4][0] != '-') {
        unsigned addr = (unsigned)strtoul(argv[4], nullptr, 16);
        unsigned len = (argc>=6) ? (unsigned)strtoul(argv[5], nullptr, 0) : 32u;
        if(len > 2048u) len = 2048u;
        printf("DUMP[%04X..%04X]=", addr & 0xFFFFu, (addr + (len ? len - 1u : 0u)) & 0xFFFFu);
        for(unsigned i=0;i<len;i++) printf("%02X", mem->DebugRetrieve((u16)(addr+i)));
        printf("\n");
    }
    if(argc>=7 && argv[6][0] != '-') {
        unsigned addr = (unsigned)strtoul(argv[6], nullptr, 16) & 0x3FFFu;
        unsigned len = (argc>=8) ? (unsigned)strtoul(argv[7], nullptr, 0) : 32u;
        if(len > 2048u) len = 2048u;
        u8 *vram=core.GetVideo()->GetVRAM();
        printf("VRAM[%04X..%04X]=", addr, (addr + (len ? len - 1u : 0u)) & 0x3FFFu);
        for(unsigned i=0;i<len;i++) printf("%02X",vram[(addr+i)&0x3FFFu]);
        printf("\n");
    }
    return 0;
}
