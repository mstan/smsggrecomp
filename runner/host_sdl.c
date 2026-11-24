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
    if (SDL_Init(SDL_INIT_VIDEO) != 0){
        fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    if (scale < 1) scale = 1;
    g_work_count=0;g_work_start=0;
    g_src.x = crop_x; g_src.y = crop_y; g_src.w = crop_w; g_src.h = crop_h;

    Uint32 flags=g_hidden?SDL_WINDOW_HIDDEN:SDL_WINDOW_SHOWN;
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

static uint8_t key_to_pad(SDL_Keycode k){
    switch (k){
        case SDLK_UP:     return SMS_PAD_UP;
        case SDLK_DOWN:   return SMS_PAD_DOWN;
        case SDLK_LEFT:   return SMS_PAD_LEFT;
        case SDLK_RIGHT:  return SMS_PAD_RIGHT;
        case SDLK_z:      return SMS_PAD_B1;
        case SDLK_x:      return SMS_PAD_B2;
        case SDLK_RETURN: return SMS_PAD_START;
        default:          return 0;
    }
}


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
static double   g_perf_freq;
static double   g_period_counts;   /* frame period in perf-counter units; 0 = uncapped */
static uint64_t g_deadline;        /* perf-counter time this frame should end */

void host_set_frame_cap(double fps){
    g_perf_freq     = (double)SDL_GetPerformanceFrequency();
    g_period_counts = (fps > 0.0) ? g_perf_freq / fps : 0.0;
    g_deadline      = 0;           /* re-anchored on the next present */
}

static void host_pace(void){
    if (g_period_counts <= 0.0) return;
    uint64_t now = SDL_GetPerformanceCounter();
    if (g_deadline == 0) g_deadline = now;                 /* first frame: anchor */
    g_deadline += (uint64_t)g_period_counts;
    /* If we fell far behind (debugger pause, hitch), resync instead of bursting
     * to catch up. */
    if (now > g_deadline + (uint64_t)(g_period_counts * 4.0))
        g_deadline = now + (uint64_t)g_period_counts;
    for (;;){
        now = SDL_GetPerformanceCounter();
        if (now >= g_deadline) break;
        double remain_ms = (double)(g_deadline - now) * 1000.0 / g_perf_freq;
        if (remain_ms > 2.0) SDL_Delay((uint32_t)(remain_ms - 1.0));   /* else spin */
    }
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

    bool quit = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)){
        if (e.type == SDL_QUIT) quit = true;
        else if (e.type == SDL_KEYDOWN){
            if (e.key.keysym.sym == SDLK_ESCAPE) quit = true;
            else g_pad |= key_to_pad(e.key.keysym.sym);
        } else if (e.type == SDL_KEYUP){
            g_pad &= (uint8_t)~key_to_pad(e.key.keysym.sym);
        }
    }

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
    SDL_Quit();
}

/* ---- audio ---- */
static SDL_AudioDeviceID g_adev;
static SDL_AudioStream  *g_astream;   /* PSG src_rate stereo -> device rate */

bool host_audio_init(uint32_t src_rate){
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0){
        fprintf(stderr, "[host] SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = 48000;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = NULL;                  /* we push via SDL_QueueAudio */
    g_adev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                 SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_adev){
        fprintf(stderr, "[host] SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return false;
    }
    g_astream = SDL_NewAudioStream(AUDIO_S16SYS, 2, (int)src_rate,
                                   have.format, have.channels, have.freq);
    if (!g_astream){
        fprintf(stderr, "[host] SDL_NewAudioStream: %s\n", SDL_GetError());
        SDL_CloseAudioDevice(g_adev); g_adev = 0;
        return false;
    }
    SDL_PauseAudioDevice(g_adev, 0);       /* start playback */
    fprintf(stderr, "[host] audio: PSG %u Hz -> device %d Hz stereo\n",
            src_rate, have.freq);
    return true;
}

void host_audio_submit(const int16_t *stereo_frames, size_t frame_count){
    if (!g_astream || !g_adev || frame_count == 0) return;
    SDL_AudioStreamPut(g_astream, stereo_frames,
                       (int)(frame_count * 2 * sizeof(int16_t)));
    int avail;
    while ((avail = SDL_AudioStreamAvailable(g_astream)) > 0){
        uint8_t tmp[8192];
        int want = avail > (int)sizeof(tmp) ? (int)sizeof(tmp) : avail;
        int got = SDL_AudioStreamGet(g_astream, tmp, want);
        if (got <= 0) break;
        SDL_QueueAudio(g_adev, tmp, (uint32_t)got);
    }
}

void host_audio_shutdown(void){
    if (g_astream){ SDL_FreeAudioStream(g_astream); g_astream = NULL; }
    if (g_adev){ SDL_CloseAudioDevice(g_adev); g_adev = 0; }
}
