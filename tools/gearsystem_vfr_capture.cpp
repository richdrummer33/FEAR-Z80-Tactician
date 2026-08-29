#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "libretro.h"

static enum retro_pixel_format g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static std::vector<uint8_t> g_frame;
static unsigned g_w=0,g_h=0;
static size_t g_pitch=0;
static unsigned g_video_frames=0;

static bool env_cb(unsigned cmd, void *data) {
    switch(cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            g_pixfmt=*(enum retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data=true; return true;
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool*)data=false; return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            retro_variable *v=(retro_variable*)data; if(v) v->value=nullptr; return true;
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
            *(const char**)data="."; return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *(unsigned*)data=RETRO_LANGUAGE_ENGLISH; return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            return false;
        default:
            return false;
    }
}

static void video_cb(const void *data,unsigned width,unsigned height,size_t pitch) {
    if(!data) return;
    g_w=width; g_h=height; g_pitch=pitch; ++g_video_frames;
    g_frame.assign((const uint8_t*)data,(const uint8_t*)data+pitch*height);
}
static void audio_cb(int16_t,int16_t) {}
static size_t audio_batch_cb(const int16_t*,size_t frames) { return frames; }
static void input_poll_cb() {}
static int16_t input_state_cb(unsigned,unsigned,unsigned,unsigned) { return 0; }

template<typename T> static T sym(void *h,const char *name) {
    dlerror(); void *p=dlsym(h,name); const char *e=dlerror();
    if(e){std::fprintf(stderr,"missing symbol %s: %s\n",name,e);std::exit(3);}
    return reinterpret_cast<T>(p);
}

static bool frame_has_visible_content() {
    if(g_frame.empty()||!g_w||!g_h) return false;
    for(unsigned y=0;y<g_h;++y) {
        const uint8_t *row=&g_frame[(size_t)y*g_pitch];
        if(g_pixfmt==RETRO_PIXEL_FORMAT_XRGB8888) {
            const uint32_t *p=(const uint32_t*)row;
            for(unsigned x=0;x<g_w;++x) if((p[x]&0x00ffffffu)!=0u) return true;
        } else {
            const uint16_t *p=(const uint16_t*)row;
            for(unsigned x=0;x<g_w;++x) if(p[x]!=0u) return true;
        }
    }
    return false;
}

static uint64_t fnv64(const std::vector<uint8_t>& v) {
    uint64_t h=1469598103934665603ull;
    for(uint8_t b:v){h^=b;h*=1099511628211ull;}
    return h;
}

static bool save_ppm(const std::filesystem::path& path) {
    if(g_frame.empty()||!g_w||!g_h) return false;
    std::ofstream out(path,std::ios::binary);
    if(!out) return false;
    out<<"P6\n"<<g_w<<" "<<g_h<<"\n255\n";
    for(unsigned y=0;y<g_h;++y) {
        const uint8_t *row=&g_frame[(size_t)y*g_pitch];
        for(unsigned x=0;x<g_w;++x) {
            uint8_t rgb[3];
            if(g_pixfmt==RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p=((const uint32_t*)row)[x];
                rgb[0]=(p>>16)&255; rgb[1]=(p>>8)&255; rgb[2]=p&255;
            } else if(g_pixfmt==RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t p=((const uint16_t*)row)[x];
                rgb[0]=(uint8_t)(((p>>11)&31)*255/31);
                rgb[1]=(uint8_t)(((p>>5)&63)*255/63);
                rgb[2]=(uint8_t)((p&31)*255/31);
            } else {
                uint16_t p=((const uint16_t*)row)[x];
                rgb[0]=(uint8_t)(((p>>10)&31)*255/31);
                rgb[1]=(uint8_t)(((p>>5)&31)*255/31);
                rgb[2]=(uint8_t)((p&31)*255/31);
            }
            out.write((char*)rgb,3);
        }
    }
    return true;
}

struct Entry {
    std::string file;
    unsigned repeats=0;
    uint64_t hash=0;
};

int main(int argc,char **argv) {
    if(argc<5) {
        std::fprintf(stderr,
            "usage: %s core.so rom.gg out_dir max_seconds [stable_stop_seconds=1.5]\n",
            argv[0]);
        return 2;
    }
    const char *core_path=argv[1],*rom_path=argv[2];
    std::filesystem::path out_dir=argv[3];
    const double max_seconds=std::strtod(argv[4],nullptr);
    const double stable_stop=(argc>=6)?std::strtod(argv[5],nullptr):1.5;
    if(max_seconds<=0.0||stable_stop<0.25) return 2;
    std::filesystem::create_directories(out_dir);

    std::ifstream rf(rom_path,std::ios::binary);
    if(!rf){std::perror("rom");return 2;}
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(rf)),{});

    void *h=dlopen(core_path,RTLD_NOW|RTLD_LOCAL);
    if(!h){std::fprintf(stderr,"dlopen: %s\n",dlerror());return 2;}
    auto retro_set_environment=sym<void(*)(retro_environment_t)>(h,"retro_set_environment");
    auto retro_set_video_refresh=sym<void(*)(retro_video_refresh_t)>(h,"retro_set_video_refresh");
    auto retro_set_audio_sample=sym<void(*)(retro_audio_sample_t)>(h,"retro_set_audio_sample");
    auto retro_set_audio_sample_batch=sym<void(*)(retro_audio_sample_batch_t)>(h,"retro_set_audio_sample_batch");
    auto retro_set_input_poll=sym<void(*)(retro_input_poll_t)>(h,"retro_set_input_poll");
    auto retro_set_input_state=sym<void(*)(retro_input_state_t)>(h,"retro_set_input_state");
    auto retro_init=sym<void(*)()>(h,"retro_init");
    auto retro_deinit=sym<void(*)()>(h,"retro_deinit");
    auto retro_load_game=sym<bool(*)(const retro_game_info*)>(h,"retro_load_game");
    auto retro_unload_game=sym<void(*)()>(h,"retro_unload_game");
    auto retro_run=sym<void(*)()>(h,"retro_run");
    auto retro_get_system_av_info=sym<void(*)(retro_system_av_info*)>(h,"retro_get_system_av_info");

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    retro_game_info gi{}; gi.path=rom_path; gi.data=rom.data(); gi.size=rom.size();
    if(!retro_load_game(&gi)){std::fprintf(stderr,"retro_load_game failed\n");return 4;}
    retro_system_av_info av{}; retro_get_system_av_info(&av);
    const double fps=av.timing.fps>1.0?av.timing.fps:59.922743;
    const unsigned max_frames=(unsigned)(max_seconds*fps+0.5);
    const unsigned stable_frames=(unsigned)(stable_stop*fps+0.5);
    const unsigned min_before_stable=(unsigned)(8.0*fps+0.5);

    std::vector<Entry> entries;
    uint64_t last_hash=0;
    unsigned same_run=0;
    bool started=false;
    unsigned skipped_startup=0u;
    for(unsigned i=0;i<max_frames;++i) {
        retro_run();
        if(g_frame.empty()) continue;

        /* DISPLAY_OFF tile construction can take many source VBlanks.  Do not
         * let that black startup become eight seconds of "video".  Begin on
         * the first genuinely visible GG framebuffer. */
        if(!started) {
            if(!frame_has_visible_content()) { ++skipped_startup; continue; }
            started=true;
        }

        uint64_t hash=fnv64(g_frame);
        bool same=!entries.empty() && hash==last_hash;
        if(same) {
            ++entries.back().repeats;
            ++same_run;
        } else {
            char name[64];
            std::snprintf(name,sizeof(name),"frame_%05u.ppm",(unsigned)entries.size());
            if(!save_ppm(out_dir/name)){std::fprintf(stderr,"failed to save %s\n",name);return 5;}
            entries.push_back({name,1u,hash});
            last_hash=hash;
            same_run=1u;
        }
        if(entries.size()>=3u && i>=min_before_stable && same_run>=stable_frames) break;
    }

    if(entries.empty()){std::fprintf(stderr,"no video frames captured\n");return 6;}

    /* Keep a short, natural final hold, not the whole detector tail. */
    const unsigned final_hold=(unsigned)(0.45*fps+0.5);
    if(entries.back().repeats>final_hold) entries.back().repeats=final_hold;

    std::ofstream concat(out_dir/"frames.ffconcat");
    concat<<"ffconcat version 1.0\n";
    std::ofstream csv(out_dir/"timing.csv");
    csv<<"unique_index,source_vblanks,start_seconds,duration_seconds,file,fnv64\n";
    double t=0.0;
    unsigned src=0;
    for(size_t i=0;i<entries.size();++i) {
        const Entry& e=entries[i];
        double dur=e.repeats/fps;
        concat<<"file '"<<e.file<<"'\n";
        concat<<std::setprecision(12)<<"duration "<<dur<<"\n";
        csv<<i<<","<<e.repeats<<","<<std::fixed<<std::setprecision(9)<<t<<","<<dur
           <<","<<e.file<<","<<std::hex<<std::uppercase<<e.hash<<std::dec<<"\n";
        t+=dur; src+=e.repeats;
    }
    /* concat demuxer needs the final image repeated so its duration is honored. */
    concat<<"file '"<<entries.back().file<<"'\n";

    std::cout<<std::fixed<<std::setprecision(6)
        <<"capture vblank_fps="<<fps
        <<" source_vblanks="<<src
        <<" unique_frames="<<entries.size()
        <<" duration="<<t
        <<" seconds geometry="<<g_w<<"x"<<g_h
        <<" pixfmt="<<(int)g_pixfmt
        <<" rom_bytes="<<rom.size()
        <<" skipped_startup_vblanks="<<skipped_startup<<"\n";
    std::cout<<"No interpolation: each unique emulator framebuffer is held for its exact count of source VBlanks.\n";

    retro_unload_game(); retro_deinit(); dlclose(h);
    return 0;
}
