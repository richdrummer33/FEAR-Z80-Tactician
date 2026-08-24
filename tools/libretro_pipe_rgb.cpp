#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <iterator>
#include <vector>
#include "libretro.h"

static enum retro_pixel_format g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned g_frames=0, g_w=0, g_h=0;

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_pixfmt=*(enum retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data=true; return true;
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool*)data=false; return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: { retro_variable *v=(retro_variable*)data; if(v) v->value=nullptr; return true; }
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY: *(const char**)data="."; return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE: *(unsigned*)data=RETRO_LANGUAGE_ENGLISH; return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: return false;
        default: return false;
    }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data) return;
    g_w=width; g_h=height; ++g_frames;
    static std::vector<uint8_t> rgb;
    rgb.resize((size_t)width*height*3u);
    size_t o=0;
    for(unsigned y=0;y<height;y++) {
        const uint8_t *row=(const uint8_t*)data+(size_t)y*pitch;
        for(unsigned x=0;x<width;x++) {
            uint8_t r,g,b;
            if(g_pixfmt==RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p=((const uint32_t*)row)[x]; r=(p>>16)&255; g=(p>>8)&255; b=p&255;
            } else if(g_pixfmt==RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t p=((const uint16_t*)row)[x]; r=(uint8_t)(((p>>11)&31)*255/31); g=(uint8_t)(((p>>5)&63)*255/63); b=(uint8_t)((p&31)*255/31);
            } else {
                uint16_t p=((const uint16_t*)row)[x]; r=(uint8_t)(((p>>10)&31)*255/31); g=(uint8_t)(((p>>5)&31)*255/31); b=(uint8_t)((p&31)*255/31);
            }
            rgb[o++]=r; rgb[o++]=g; rgb[o++]=b;
        }
    }
    fwrite(rgb.data(),1,rgb.size(),stdout);
}
static void audio_cb(int16_t,int16_t) {}
static size_t audio_batch_cb(const int16_t*, size_t frames) { return frames; }
static void input_poll_cb() {}
static int16_t input_state_cb(unsigned,unsigned,unsigned,unsigned) { return 0; }

template<typename T> static T sym(void *h,const char*name){dlerror();void*p=dlsym(h,name);const char*e=dlerror();if(e){fprintf(stderr,"missing %s: %s\n",name,e);exit(3);}return reinterpret_cast<T>(p);} 

int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: %s core.so rom.gg frames\n",argv[0]);return 2;}
    const char*core_path=argv[1],*rom_path=argv[2]; unsigned frames=(unsigned)strtoul(argv[3],nullptr,10);
    std::ifstream f(rom_path,std::ios::binary); if(!f){perror("rom");return 2;} std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)),{});
    void*h=dlopen(core_path,RTLD_NOW|RTLD_LOCAL); if(!h){fprintf(stderr,"dlopen: %s\n",dlerror());return 2;}
    auto set_env=sym<void(*)(retro_environment_t)>(h,"retro_set_environment");
    auto set_video=sym<void(*)(retro_video_refresh_t)>(h,"retro_set_video_refresh");
    auto set_as=sym<void(*)(retro_audio_sample_t)>(h,"retro_set_audio_sample");
    auto set_ab=sym<void(*)(retro_audio_sample_batch_t)>(h,"retro_set_audio_sample_batch");
    auto set_poll=sym<void(*)(retro_input_poll_t)>(h,"retro_set_input_poll");
    auto set_state=sym<void(*)(retro_input_state_t)>(h,"retro_set_input_state");
    auto init=sym<void(*)()>(h,"retro_init"); auto deinit=sym<void(*)()>(h,"retro_deinit");
    auto load=sym<bool(*)(const retro_game_info*)>(h,"retro_load_game"); auto unload=sym<void(*)()>(h,"retro_unload_game"); auto run=sym<void(*)()>(h,"retro_run");
    set_env(env_cb); set_video(video_cb); set_as(audio_cb); set_ab(audio_batch_cb); set_poll(input_poll_cb); set_state(input_state_cb); init();
    retro_game_info gi{};gi.path=rom_path;gi.data=rom.data();gi.size=rom.size();if(!load(&gi)){fprintf(stderr,"load failed\n");return 4;}
    for(unsigned i=0;i<frames;i++) run();
    fflush(stdout); fprintf(stderr,"video_frames=%u size=%ux%u pixfmt=%d\n",g_frames,g_w,g_h,(int)g_pixfmt);
    unload(); deinit(); dlclose(h); return 0;
}
