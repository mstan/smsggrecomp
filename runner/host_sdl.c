/*
 * host_sdl.c - SDL2 live display host (built only with -DSMS_HAVE_SDL).
 *
 * A streaming ARGB8888 texture is updated from the runner's framebuffer each
 * frame and presented with vsync, which also paces the game loop. We take the
 * Z80/VDP framebuffer as-is (full 256x192) and crop to the platform viewport
 * (SMS = full, GG = 160x144 centred) via the RenderCopy source rect.
 *
 * SDL_MAIN_HANDLED keeps our own main() (no SDL2main / WinMain shim needed).
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "host_sdl.h"

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

    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren)  /* fall back to software if no accelerated/vsync renderer */
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren){ fprintf(stderr, "[host] CreateRenderer: %s\n", SDL_GetError()); return false; }

    /* integer-scale crisply; letterbox to preserve aspect */
    SDL_RenderSetLogicalSize(g_ren, crop_w, crop_h);

    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, fb_w, fb_h);
    if (!g_tex){ fprintf(stderr, "[host] CreateTexture: %s\n", SDL_GetError()); return false; }
    return true;
}

bool host_present(const uint32_t *fb, int fb_w, int fb_h){
    if (!g_tex) return false;
    SDL_UpdateTexture(g_tex, NULL, fb, fb_w * (int)sizeof(uint32_t));
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, &g_src, NULL);
    SDL_RenderPresent(g_ren);   /* blocks to vsync, pacing the loop */
    (void)fb_h;

    bool quit = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)){
        if (e.type == SDL_QUIT) quit = true;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = true;
    }
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
