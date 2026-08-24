#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>

#include "libretro.h"

static enum retro_pixel_format g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static std::vector<uint8_t> g_frame;
static unsigned g_w=0,g_h=0; static size_t g_pitch=0; static unsigned g_video_frames=0;

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            g_pixfmt = *(enum retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data = true; return true;
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool*)data = false; return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            retro_variable *v = (retro_variable*)data; if (v) v->value = nullptr; return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *(unsigned*)data = RETRO_LANGUAGE_ENGLISH; return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            return false;
        default:
            return false;
    }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data) return;
    g_w=width; g_h=height; g_pitch=pitch; ++g_video_frames;
    size_t bytes = pitch * height;
    g_frame.assign((const uint8_t*)data, (const uint8_t*)data + bytes);
}
static void audio_cb(int16_t,int16_t) {}
static size_t audio_batch_cb(const int16_t*, size_t frames) { return frames; }
static void input_poll_cb() {}
static int16_t input_state_cb(unsigned,unsigned,unsigned,unsigned) { return 0; }

static bool save_ppm(const char *path) {
    if (g_frame.empty() || !g_w || !g_h) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "P6\n" << g_w << " " << g_h << "\n255\n";
    for (unsigned y=0;y<g_h;y++) {
        const uint8_t *row=&g_frame[(size_t)y*g_pitch];
        for (unsigned x=0;x<g_w;x++) {
            uint8_t rgb[3];
            if (g_pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p=((const uint32_t*)row)[x];
                rgb[0]=(p>>16)&255; rgb[1]=(p>>8)&255; rgb[2]=p&255;
            } else if (g_pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t p=((const uint16_t*)row)[x];
                rgb[0]=(uint8_t)(((p>>11)&31)*255/31); rgb[1]=(uint8_t)(((p>>5)&63)*255/63); rgb[2]=(uint8_t)((p&31)*255/31);
            } else {
                uint16_t p=((const uint16_t*)row)[x];
                rgb[0]=(uint8_t)(((p>>10)&31)*255/31); rgb[1]=(uint8_t)(((p>>5)&31)*255/31); rgb[2]=(uint8_t)((p&31)*255/31);
            }
            out.write((char*)rgb,3);
        }
    }
    return true;
}

template<typename T> static T sym(void *h, const char *name) {
    dlerror(); void *p=dlsym(h,name); const char *e=dlerror();
    if(e){ fprintf(stderr,"missing symbol %s: %s\n",name,e); exit(3);} return reinterpret_cast<T>(p);
}

int main(int argc,char **argv){
    if(argc<4){fprintf(stderr,"usage: %s <core.so> <rom.gg> <frames> [frame.ppm] [ram_off_hex] [ram_len]\n",argv[0]);return 2;}
    const char *core_path=argv[1], *rom_path=argv[2]; unsigned frames=(unsigned)strtoul(argv[3],nullptr,10);
    const char *ppm=(argc>=5)?argv[4]:nullptr;
    std::ifstream f(rom_path,std::ios::binary); if(!f){perror("rom");return 2;}
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)),{});
    void *h=dlopen(core_path,RTLD_NOW|RTLD_LOCAL); if(!h){fprintf(stderr,"dlopen: %s\n",dlerror());return 2;}
    auto retro_set_environment=sym<void(*)(retro_environment_t)>(h,"retro_set_environment");
    auto retro_set_video_refresh=sym<void(*)(retro_video_refresh_t)>(h,"retro_set_video_refresh");
    auto retro_set_audio_sample=sym<void(*)(retro_audio_sample_t)>(h,"retro_set_audio_sample");
    auto retro_set_audio_sample_batch=sym<void(*)(retro_audio_sample_batch_t)>(h,"retro_set_audio_sample_batch");
    auto retro_set_input_poll=sym<void(*)(retro_input_poll_t)>(h,"retro_set_input_poll");
    auto retro_set_input_state=sym<void(*)(retro_input_state_t)>(h,"retro_set_input_state");
    auto retro_init=sym<void(*)()>(h,"retro_init"); auto retro_deinit=sym<void(*)()>(h,"retro_deinit");
    auto retro_load_game=sym<bool(*)(const retro_game_info*)>(h,"retro_load_game"); auto retro_unload_game=sym<void(*)()>(h,"retro_unload_game");
    auto retro_run=sym<void(*)()>(h,"retro_run");
    auto retro_get_memory_data=sym<void*(*)(unsigned)>(h,"retro_get_memory_data");
    auto retro_get_memory_size=sym<size_t(*)(unsigned)>(h,"retro_get_memory_size");
    auto retro_get_system_av_info=sym<void(*)(retro_system_av_info*)>(h,"retro_get_system_av_info");
    retro_set_environment(env_cb); retro_set_video_refresh(video_cb); retro_set_audio_sample(audio_cb); retro_set_audio_sample_batch(audio_batch_cb); retro_set_input_poll(input_poll_cb); retro_set_input_state(input_state_cb);
    retro_init();
    retro_game_info gi{}; gi.path=rom_path; gi.data=rom.data(); gi.size=rom.size();
    if(!retro_load_game(&gi)){fprintf(stderr,"retro_load_game failed\n");retro_deinit();return 4;}
    retro_system_av_info av{}; retro_get_system_av_info(&av);
    for(unsigned i=0;i<frames;i++) retro_run();
    printf("ran=%u video_frames=%u geometry=%ux%u fps=%.6f pixfmt=%d last=%ux%u pitch=%zu rom=%zu\n",frames,g_video_frames,av.geometry.base_width,av.geometry.base_height,av.timing.fps,(int)g_pixfmt,g_w,g_h,g_pitch,rom.size());
    if(ppm){ if(!save_ppm(ppm)){fprintf(stderr,"failed to save ppm\n");return 5;} printf("saved=%s\n",ppm); }
    if(argc>=6){
        unsigned off=(unsigned)strtoul(argv[5],nullptr,0); unsigned len=(argc>=7)?(unsigned)strtoul(argv[6],nullptr,0):64u;
        uint8_t *ram=(uint8_t*)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM); size_t rsz=retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
        printf("ram_size=%zu off=0x%04X len=%u\n",rsz,off,len);
        if(ram && off<rsz){ if(off+len>rsz)len=(unsigned)(rsz-off); for(unsigned i=0;i<len;i++){ if(i%16==0)printf("%04X:",off+i); printf("%02X",ram[off+i]); if(i%16==15||i+1==len)printf("\n"); } }
    }
    retro_unload_game(); retro_deinit(); dlclose(h); return 0;
}
