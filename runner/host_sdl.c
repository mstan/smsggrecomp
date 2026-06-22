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

#include <stdio.h>

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static SDL_Rect      g_src;     /* crop region of the framebuffer */

bool host_init(int fb_w, int fb_h,
               int crop_x, int crop_y, int crop_w, int crop_h,
               int scale, const char *title){
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0){
        fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    if (scale < 1) scale = 1;
    g_src.x = crop_x; g_src.y = crop_y; g_src.w = crop_w; g_src.h = crop_h;

    g_win = SDL_CreateWindow(title ? title : "SMS/GG recompiled",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             crop_w * scale, crop_h * scale, SDL_WINDOW_SHOWN);
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
    if (!g_tex) return false;
    SDL_UpdateTexture(g_tex, NULL, fb, fb_w * (int)sizeof(uint32_t));
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, &g_src, NULL);
    SDL_RenderPresent(g_ren);
    (void)fb_h;

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

    host_pace();      /* hold the loop to realtime (display-refresh-independent) */
    return !quit;
}

void host_shutdown(void){
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
