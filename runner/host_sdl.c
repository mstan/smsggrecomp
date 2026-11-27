/*
 * host_sdl.c - SDL2 live display host (built only with -DSMS_HAVE_SDL).
 *
 * A streaming ARGB8888 texture is updated from the runner's framebuffer each
 * frame and presented. The loop is paced by a precise wall-clock frame limiter
 * (host_set_frame_cap) locked to the emulated frame rate, NOT by vsync — vsync
 * paces to the monitor's refresh, which runs the game too fast on >60 Hz
 * displays. We take the Z80/VDP framebuffer as-is (full 256x192) and crop to the
 * platform viewport (SMS = full, GG = 160x144 centred) via the RenderCopy rect.
 *
 * SDL_MAIN_HANDLED keeps our own main() (no SDL2main / WinMain shim needed).
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "host_sdl.h"
#include "services.h"
#include "recomp_input.h"
#include "recomp_audio.h"
#include "retro_presentation/pacing.h"
#include "glue.h"       /* SMS_PAD_* bit constants (the controller contract) */
#ifdef SMS_GPU
#include "host_gl.h"
#endif

#include "png_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static SDL_Rect      g_src;     /* crop region of the framebuffer */
static int g_internal_scale;
static bool g_hidden,g_gpu;
static int g_fullscreen;
void host_set_fullscreen(int value){g_fullscreen=value;}
bool host_uses_gpu(void){return g_gpu;}
static double g_work_ms[8192];
static unsigned g_work_count;
static uint64_t g_work_start;
static int compare_ms(const void *a,const void *b){double x=*(const double*)a,y=*(const double*)b;return(x>y)-(x<y);}
void host_set_internal_scale(int s){g_internal_scale=s;}
void host_set_hidden(bool h){g_hidden=h;}
void host_report(FILE *f){
    qsort(g_work_ms,g_work_count,sizeof g_work_ms[0],compare_ms);
    fprintf(f,"{\"work_samples\":%u,\"frame_work_p95_ms\":%.6f,\"frame_work_max_ms\":%.6f,\"renderer\":",
        g_work_count,g_work_count?g_work_ms[(g_work_count-1)*95/100]:0,g_work_count?g_work_ms[g_work_count-1]:0);
#ifdef SMS_GPU
    if(g_gpu)host_gl_report(f);else
#endif
    fputs("{\"gpu\":false,\"effective_scale\":1}",f);
    fputs("}\n",f);
}
bool host_dump_gpu(const char *p){
#ifdef SMS_GPU
    return g_gpu&&host_gl_dump(p);
#else
    (void)p;return false;
#endif
}

bool host_init(int fb_w, int fb_h,
               int crop_x, int crop_y, int crop_w, int crop_h,
               int scale, const char *title){
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0){
        fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    if (scale < 1) scale = 1;
    g_work_count=0;g_work_start=0;
    g_src.x = crop_x; g_src.y = crop_y; g_src.w = crop_w; g_src.h = crop_h;

    Uint32 flags=g_hidden?SDL_WINDOW_HIDDEN:SDL_WINDOW_SHOWN;
    if(g_fullscreen==1)flags|=SDL_WINDOW_FULLSCREEN_DESKTOP;
    else if(g_fullscreen==2)flags|=SDL_WINDOW_FULLSCREEN;
#if defined(SMS_GPU) && !defined(SMS_VDP_REFERENCE)
    if(g_internal_scale>0){
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    g_win=SDL_CreateWindow(title?title:"SMS/GG",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        crop_w*scale,crop_h*scale,flags|SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    if(g_win){
        g_gpu=host_gl_init(g_win,crop_x,crop_y,crop_w,crop_h,g_internal_scale);
        if(g_gpu)return true;
        SDL_DestroyWindow(g_win);g_win=NULL;
    }
    fprintf(stderr,"[host] GPU unavailable; portable native presentation: %s\n",SDL_GetError());
    }
#endif
    g_win = SDL_CreateWindow(title ? title : "SMS/GG recompiled",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             crop_w * scale, crop_h * scale, flags|SDL_WINDOW_RESIZABLE);
    if (!g_win){ fprintf(stderr, "[host] CreateWindow: %s\n", SDL_GetError()); return false; }

    /* No PRESENTVSYNC: the wall-clock limiter (host_set_frame_cap) owns pacing,
     * so the game speed is independent of the monitor's refresh rate. */
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren)  /* fall back to software if no accelerated renderer */
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren){ fprintf(stderr, "[host] CreateRenderer: %s\n", SDL_GetError()); return false; }

    /* integer-scale crisply; letterbox to preserve aspect */
    SDL_RenderSetLogicalSize(g_ren, crop_w, crop_h);

    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, fb_w, fb_h);
    if (!g_tex){ fprintf(stderr, "[host] CreateTexture: %s\n", SDL_GetError()); return false; }
    return true;
}

/* Keyboard -> SMS controller. Arrows = D-pad, Z/X = buttons 1/2 (Sonic: jump),
 * Enter = Start (Game Gear). Esc quits. */
static uint8_t g_pad;     /* live P1 mask (SMS_PAD_*) */


static ri_context *g_input;
static SDL_GameController *g_controllers[RI_MAX_DEVICES];
static ri_device g_devices[RI_MAX_DEVICES];
static unsigned g_device_count;
static void input_update(void){
    /* SDL handle lifetime and event pumping stay in the host. The shared core
     * owns device matching, saved assignment, bindings and held-state policy. */
    SDL_GameControllerUpdate();
    for(unsigned i=0;i<RI_MAX_DEVICES;i++)if(g_controllers[i]&&!SDL_GameControllerGetAttached(g_controllers[i])){
        SDL_GameControllerClose(g_controllers[i]);g_controllers[i]=NULL;
    }
    int total=SDL_NumJoysticks();
    for(int j=0;j<total;j++)if(SDL_IsGameController(j)){
        SDL_JoystickID id=SDL_JoystickGetDeviceInstanceID(j);bool open=false;
        for(unsigned i=0;i<RI_MAX_DEVICES;i++)if(g_controllers[i]&&SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controllers[i]))==id)open=true;
        if(!open)for(unsigned i=0;i<RI_MAX_DEVICES;i++)if(!g_controllers[i]){g_controllers[i]=SDL_GameControllerOpen(j);break;}
    }
    g_device_count=0;
    for(unsigned i=0;i<RI_MAX_DEVICES;i++)if(g_controllers[i]){
        ri_device *d=&g_devices[g_device_count++];memset(d,0,sizeof *d);
        SDL_Joystick *joy=SDL_GameControllerGetJoystick(g_controllers[i]);
        d->id=(uint32_t)SDL_JoystickInstanceID(joy);
        snprintf(d->name,sizeof d->name,"%s",SDL_GameControllerName(g_controllers[i]));
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy),d->guid,sizeof d->guid);
        const char *serial=SDL_GameControllerGetSerial(g_controllers[i]);
        if(serial&&*serial)snprintf(d->physical_key,sizeof d->physical_key,"%s:%s",d->guid,serial);
        for(int b=0;b<SDL_CONTROLLER_BUTTON_MAX;b++)if(SDL_GameControllerGetButton(g_controllers[i],b))d->buttons|=1u<<b;
        for(int a=0;a<RI_MAX_AXES;a++)d->axes[a]=SDL_GameControllerGetAxis(g_controllers[i],a);
    }
    if(!g_input){
        g_input=ri_create(1,7,NULL,NULL);
        ri_player player;sms_settings_player(&player);
        ri_restore(g_input,&player,1);
    }
    int count=0;const uint8_t *keys=SDL_GetKeyboardState(&count);
    ri_update(g_input,g_devices,g_device_count,keys,(size_t)count);
    g_pad=(uint8_t)ri_sample(g_input,0);
}
const char *host_controller_label(void){return g_input?ri_player_label(g_input,0):"Keyboard";}
bool host_poll(void){
    bool quit=false;SDL_Event e;
    while(SDL_PollEvent(&e))if(e.type==SDL_QUIT||(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE))quit=true;
    input_update();return !quit;
}
void host_show(void){if(g_win&&g_hidden){SDL_ShowWindow(g_win);g_hidden=false;}}

/* ---- headless presented-frame capture (--capture-out) ----
 * Dumps exactly what the window would show: the GPU target when the GPU path
 * is active, otherwise the cropped native framebuffer, aspect-fitted (nearest)
 * and centred on a --capture-size canvas. The window is hidden by the caller. */
static char     g_cap_dir[600];
static int      g_cap_w, g_cap_h;
static uint64_t g_cap_from, g_cap_to = ~0ull, g_cap_every = 1;
static uint32_t g_cap_written;
void host_set_capture(const char *dir, int w, int h, uint64_t from, uint64_t to, uint64_t every){
    snprintf(g_cap_dir, sizeof g_cap_dir, "%s", dir ? dir : "");
    g_cap_w = w; g_cap_h = h; g_cap_from = from; g_cap_to = to; g_cap_every = every ? every : 1;
}
uint32_t host_capture_written(void){ return g_cap_written; }
static void cap_fit(const uint32_t *src, int sw, int sh, int sstride, uint32_t *dst, int dw, int dh){
    memset(dst, 0, (size_t)dw * dh * 4);
    for (int i = 0; i < dw * dh; i++) dst[i] = 0xFF000000u;
    int scale_n = dw * sh < dh * sw ? dw : dh, scale_d = dw * sh < dh * sw ? sw : sh;
    int ow = (int)((int64_t)sw * scale_n / scale_d), oh = (int)((int64_t)sh * scale_n / scale_d);
    if (ow > dw) ow = dw;
    if (oh > dh) oh = dh;
    int ox = (dw - ow) / 2, oy = (dh - oh) / 2;
    for (int y = 0; y < oh; y++){
        int sy = (int)((int64_t)y * sh / oh);
        for (int x = 0; x < ow; x++){
            int sx = (int)((int64_t)x * sw / ow);
            dst[(size_t)(oy + y) * dw + ox + x] = src[(size_t)sy * sstride + sx] | 0xFF000000u;
        }
    }
}
static void host_capture_frame(const uint32_t *fb, int fb_w){
    uint64_t idx = glue_frame_count() - 1;   /* 0-based frame just presented */
    if (idx < g_cap_from || idx > g_cap_to || ((idx - g_cap_from) % g_cap_every)) return;
    uint32_t *pix = NULL; int w = 0, h = 0, stride = 0;
    const uint32_t *src = NULL; uint32_t *own = NULL;
#ifdef SMS_GPU
    if (g_gpu){
        size_t cap = (size_t)g_src.w * g_src.h * 256;   /* pixels, up to 16x */
        own = (uint32_t *)malloc(cap * 4);
        if (!own || !host_gl_readback(own, cap, &w, &h)){ free(own); fputs("[capture] GPU readback failed\n", stderr); return; }
        src = own; stride = w;
    } else
#endif
    {
        if (!fb) return;
        src = fb + (size_t)g_src.y * fb_w + g_src.x; w = g_src.w; h = g_src.h; stride = fb_w;
    }
    char path[700]; snprintf(path, sizeof path, "%s/frame_%06u.png", g_cap_dir, (unsigned)g_cap_written);
    int ow = g_cap_w > 0 ? g_cap_w : w, oh = g_cap_h > 0 ? g_cap_h : h;
    if (ow == w && oh == h && !own){
        pix = (uint32_t *)malloc((size_t)w * h * 4);
        if (pix) for (int y = 0; y < h; y++) memcpy(pix + (size_t)y * w, src + (size_t)y * stride, (size_t)w * 4);
    } else {
        pix = (uint32_t *)malloc((size_t)ow * oh * 4);
        if (pix) cap_fit(src, w, h, stride, pix, ow, oh);
    }
    if (pix && png_write_argb(path, pix, ow, oh, ow) == 0) g_cap_written++;
    free(pix); free(own);
}

uint8_t host_get_pad1(void){ return g_pad; }

/* ---- wall-clock frame limiter ----
 * Deadline-advance model: the deadline moves forward by exactly one frame period
 * each present, so the long-term rate is drift-free. We coarse-sleep (SDL_Delay)
 * to ~1 ms before the deadline, then spin the remainder for sub-ms precision. */
static RetroFramePacer g_pacer;
static bool g_prepare_pacing;
void host_set_prepare_pacing(bool fast){if(fast!=g_prepare_pacing){g_prepare_pacing=fast;host_set_frame_cap(fast?0:glue_frame_rate());}}
void host_set_frame_cap(double fps){retro_frame_pacer_init(&g_pacer,SDL_GetPerformanceFrequency(),fps);}
static void host_pace(void){
    uint64_t deadline=retro_frame_pacer_deadline(&g_pacer,SDL_GetPerformanceCounter());
    for(;;){uint64_t now=SDL_GetPerformanceCounter();if(now>=deadline)break;
        double ms=(deadline-now)*1000.0/SDL_GetPerformanceFrequency();
        if(ms>2)SDL_Delay((uint32_t)(ms-1));}
}

bool host_present(const uint32_t *fb, int fb_w, int fb_h){
#ifdef SMS_GPU
    if(g_gpu)host_gl_present(vdp_frame_jobs());else
#endif
    {
    if (!g_tex) return false;
    SDL_UpdateTexture(g_tex, NULL, fb, fb_w * (int)sizeof(uint32_t));
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, &g_src, NULL);
    SDL_RenderPresent(g_ren);
    }
    (void)fb_h;
    if (g_cap_dir[0]) host_capture_frame(fb, fb_w);

    bool quit = !host_poll();

    uint64_t end=SDL_GetPerformanceCounter();
    if(g_work_start && g_work_count<8192)g_work_ms[g_work_count++]=(end-g_work_start)*1000.0/SDL_GetPerformanceFrequency();
    host_pace();      /* hold the loop to realtime (display-refresh-independent) */
    g_work_start=SDL_GetPerformanceCounter();
    return !quit;
}

void host_shutdown(void){
#ifdef SMS_GPU
    host_gl_shutdown();
#endif
    g_gpu=false;
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    g_tex = NULL; g_ren = NULL; g_win = NULL;
    for(unsigned i=0;i<RI_MAX_DEVICES;i++)if(g_controllers[i]){SDL_GameControllerClose(g_controllers[i]);g_controllers[i]=NULL;}
    ri_destroy(g_input);g_input=NULL;
    SDL_Quit();
}

/* ---- audio ---- */
static SDL_AudioDeviceID g_adev;
static recomp_audio *g_bridge;
static int g_audio_channels=2;
static int g_volume=100;
static void audio_callback(void *unused,Uint8 *stream,int bytes){
    (void)unused;
    int frames=bytes/(int)(sizeof(int16_t)*g_audio_channels);
    if(g_bridge)recomp_audio_pull(g_bridge,(int16_t*)stream,frames);else memset(stream,0,(size_t)bytes);
    if(g_volume!=100){int16_t *p=(int16_t*)stream;for(int i=0;i<bytes/2;i++)p[i]=(int16_t)((int32_t)p[i]*g_volume/100);}
}
bool host_audio_init(uint32_t src_rate){
    if(SDL_InitSubSystem(SDL_INIT_AUDIO)!=0)return false;
    SDL_AudioSpec want,have;SDL_zero(want);
    want.freq=48000;want.format=AUDIO_S16SYS;want.channels=2;want.samples=1024;want.callback=audio_callback;
    const char *device=sms_settings_audio_device();
    g_adev=SDL_OpenAudioDevice(device&&*device?device:NULL,0,&want,&have,SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if(!g_adev){fprintf(stderr,"[audio] %s\n",SDL_GetError());return false;}
    rab_config config;rab_config_defaults(&config);
    /* Generic stereo clock bridge. Chip synthesis/profile is owned by the
     * SMS/GG PSG provider; this policy does not alter guest clocks or PCM. */
    config.channels=2;config.source_rate=src_rate;config.host_rate=have.freq;
    g_bridge=recomp_audio_create(&config);g_volume=sms_settings_volume();
    if(!g_bridge){SDL_CloseAudioDevice(g_adev);g_adev=0;return false;}
    SDL_PauseAudioDevice(g_adev,0);
    fprintf(stderr,"[audio] native Windows SDL driver=%s; shared stereo bridge %u -> %d Hz\n",SDL_GetCurrentAudioDriver(),src_rate,have.freq);
    return true;
}
void host_audio_submit(const int16_t *frames,size_t n){
    if(!g_bridge||!g_adev||!n)return;
    SDL_LockAudioDevice(g_adev);recomp_audio_push(g_bridge,frames,(int)n);SDL_UnlockAudioDevice(g_adev);
}
void host_audio_pause(bool pause){
    if(!g_adev)return;
    SDL_PauseAudioDevice(g_adev,pause?1:0);
    if(pause){SDL_LockAudioDevice(g_adev);recomp_audio_reset(g_bridge);SDL_UnlockAudioDevice(g_adev);}
}
void host_audio_shutdown(void){
    if(g_adev){SDL_CloseAudioDevice(g_adev);g_adev=0;}
    recomp_audio_destroy(g_bridge);g_bridge=NULL;
}
