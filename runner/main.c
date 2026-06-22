/*
 * main.c - headless SMS/GG runner entry point.
 *
 *   <exe> <rom.sms|rom.gg> [--frames N] [--gg]
 *
 * Loads the ROM, runs the recompiled game for N frames (default 600), and on
 * exit reports the frame count and any dispatch misses (also written to
 * dispatch_misses.log next to the working directory) so the dispatch-miss loop
 * (PRINCIPLES #13a) can resolve them. Video/audio host + always-on rings + TCP
 * (DEBUG.md) layer on top of this core later.
 */
#include "glue.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <stdbool.h>
#include <stdlib.h>

#ifdef SMS_HAVE_SDL
#include "host_sdl.h"
#include "video/sms_vdp.h"
/* Bridge the runner's per-frame callback to the SDL host. Returns nonzero to
 * request shutdown (glue.c then unwinds the run loop). */
static int sdl_frame_cb(const uint32_t *fb, int w, int h){
    return host_present(fb, w, h) ? 0 : 1;
}
#endif

/* ---- audio output sinks --------------------------------------------------
 * glue hands us interleaved-stereo S16 frames once per video frame. We fan
 * them out to any active output: a WAV file (headless/observable dump) and/or
 * the live SDL audio device. Both are optional and independent. */
static FILE    *g_wav;
static uint32_t g_wav_data_bytes;
#ifdef SMS_HAVE_SDL
static bool     g_audio_live;
#endif

static void wav_put32(FILE *f, uint32_t v){ fputc(v&0xFF,f); fputc((v>>8)&0xFF,f); fputc((v>>16)&0xFF,f); fputc((v>>24)&0xFF,f); }
static void wav_put16(FILE *f, uint16_t v){ fputc(v&0xFF,f); fputc((v>>8)&0xFF,f); }

static bool wav_open(const char *path, uint32_t rate){
    g_wav = fopen(path, "wb");
    if (!g_wav){ fprintf(stderr,"[runner] cannot open WAV %s\n", path); return false; }
    const uint16_t ch = 2, bits = 16;
    fwrite("RIFF",1,4,g_wav); wav_put32(g_wav, 0);            /* size patched on close */
    fwrite("WAVE",1,4,g_wav);
    fwrite("fmt ",1,4,g_wav); wav_put32(g_wav, 16);
    wav_put16(g_wav, 1);                                      /* PCM */
    wav_put16(g_wav, ch);
    wav_put32(g_wav, rate);
    wav_put32(g_wav, rate * ch * (bits/8));                   /* byte rate */
    wav_put16(g_wav, (uint16_t)(ch * (bits/8)));              /* block align */
    wav_put16(g_wav, bits);
    fwrite("data",1,4,g_wav); wav_put32(g_wav, 0);            /* size patched on close */
    g_wav_data_bytes = 0;
    fprintf(stderr,"[runner] writing audio to %s (%u Hz S16 stereo)\n", path, rate);
    return true;
}
static void wav_close(void){
    if (!g_wav) return;
    fflush(g_wav);
    fseek(g_wav, 4,  SEEK_SET); wav_put32(g_wav, 36 + g_wav_data_bytes);
    fseek(g_wav, 40, SEEK_SET); wav_put32(g_wav, g_wav_data_bytes);
    fclose(g_wav); g_wav = NULL;
}

static void audio_sink(const int16_t *frames, size_t n){
    if (g_wav){
        fwrite(frames, 2*sizeof(int16_t), n, g_wav);
        g_wav_data_bytes += (uint32_t)(n * 2 * sizeof(int16_t));
    }
#ifdef SMS_HAVE_SDL
    if (g_audio_live) host_audio_submit(frames, n);
#endif
}

static int ci_cmp(const char *a, const char *b){
    for (; *a && *b; a++, b++){
        int ca=*a, cb=*b;
        if (ca>='A'&&ca<='Z') ca+=32;
        if (cb>='A'&&cb<='Z') cb+=32;
        if (ca!=cb) return ca-cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
static bool ends_with(const char *s, const char *suf){
    size_t ls=strlen(s), lf=strlen(suf);
    return ls>=lf && ci_cmp(s+ls-lf, suf)==0;
}

int main(int argc, char **argv){
    const char *rom = NULL;
    uint64_t frames = 600;
    int force_gg = -1;
    uint64_t dump_frame = 0;
    const char *dump_out = NULL;
    const char *vdp_trace = NULL;   /* per-frame VDP-state CSV (oracle diff) */
    bool want_window = false;
    int  win_scale = 3;
    bool interp = false;       /* oracle: run the reference superzazu interpreter */
    const char *audio_wav = NULL;   /* dump PSG output to this WAV */
    bool mute = false;              /* suppress live SDL audio (window build) */

    bool frames_set = false;
    for (int i=1;i<argc;i++){
        if (strcmp(argv[i],"--frames")==0 && i+1<argc){ frames = strtoull(argv[++i],NULL,0); frames_set = true; }
        else if (strcmp(argv[i],"--gg")==0) force_gg = 1;
        else if (strcmp(argv[i],"--sms")==0) force_gg = 0;
        else if (strcmp(argv[i],"--dump-frame")==0 && i+1<argc) dump_frame = strtoull(argv[++i],NULL,0);
        else if (strcmp(argv[i],"--dump-out")==0 && i+1<argc) dump_out = argv[++i];
        else if (strcmp(argv[i],"--vdp-trace")==0 && i+1<argc) vdp_trace = argv[++i];
        else if (strcmp(argv[i],"--force-display")==0){ extern int g_vdp_force_display; g_vdp_force_display=1; }
        else if (strcmp(argv[i],"--interp")==0) interp = true;
        else if (strcmp(argv[i],"--audio-wav")==0 && i+1<argc) audio_wav = argv[++i];
        else if (strcmp(argv[i],"--mute")==0) mute = true;
        else if (strcmp(argv[i],"--window")==0){
            want_window = true;
            if (i+1<argc && argv[i+1][0] != '-') win_scale = (int)strtol(argv[++i],NULL,0);
        }
        else if (argv[i][0] != '-') rom = argv[i];
        else fprintf(stderr,"[runner] unknown arg: %s\n", argv[i]);
    }
    if (!rom){
        fprintf(stderr,"usage: %s <rom.sms|rom.gg> [--frames N] [--gg|--sms] "
                       "[--window [scale]] [--audio-wav out.wav] [--mute] [--interp]\n", argv[0]);
        return 2;
    }

    bool is_gg = (force_gg >= 0) ? (force_gg==1) : ends_with(rom, ".gg");

    /* In windowed mode, default to run-until-closed unless --frames was given. */
    if (want_window && !frames_set) frames = 0;

    fprintf(stderr,"[runner] SMS/GG recompiled runner (%s)\n",
            want_window ? "windowed" : "headless");
    if (!glue_load_rom(rom)) return 1;
    fprintf(stderr,"[runner] loaded %s (%llu bytes), platform=%s, running %s\n",
            rom, (unsigned long long)glue_rom_size(), is_gg?"GG":"SMS",
            frames ? "to frame limit" : "until window close");

    glue_init(is_gg, frames);
    if (dump_out) glue_set_dump(dump_frame ? dump_frame : frames, dump_out);
    if (vdp_trace) glue_set_vdp_trace(vdp_trace);

    if (want_window){
        (void)win_scale;   /* used only in the SDL block below */
#ifdef SMS_HAVE_SDL
        /* SMS shows the full 256x192; GG shows a 160x144 centred crop. */
        int cx = is_gg ? (SMS_SCREEN_W-GG_SCREEN_W)/2 : 0;
        int cy = is_gg ? (SMS_SCREEN_H-GG_SCREEN_H)/2 : 0;
        int cw = is_gg ? GG_SCREEN_W : SMS_SCREEN_W;
        int ch = is_gg ? GG_SCREEN_H : SMS_SCREEN_H;
        if (host_init(SMS_SCREEN_W, SMS_SCREEN_H, cx, cy, cw, ch, win_scale,
                      is_gg ? "Sonic (GG) - recompiled" : "Sonic 1 (SMS) - recompiled"))
            glue_set_frame_callback(sdl_frame_cb);
        else
            fprintf(stderr,"[runner] SDL window init failed; running headless\n");
#else
        fprintf(stderr,"[runner] --window requested but built without SDL "
                       "(-DSMS_HAVE_SDL); running headless\n");
#endif
    }

    /* Audio outputs (independent; either/both may be active). */
    bool audio_on = false;
#ifdef SMS_HAVE_SDL
    if (want_window && !mute){
        if (host_audio_init(glue_audio_sample_rate())){ g_audio_live = true; audio_on = true; }
    }
#else
    (void)mute;
#endif
    if (audio_wav){
        if (wav_open(audio_wav, glue_audio_sample_rate())) audio_on = true;
    }
    if (audio_on) glue_set_audio_sink(audio_sink);

    if (interp){
        fprintf(stderr,"[runner] ORACLE reference mode (superzazu interpreter)\n");
        glue_run_interp();
    } else {
        glue_run();
    }

#ifdef SMS_HAVE_SDL
    if (g_audio_live) host_audio_shutdown();
    if (want_window) host_shutdown();
#endif
    wav_close();

    fprintf(stderr,"[runner] stopped after %llu frames; dispatch misses: %d\n",
            (unsigned long long)glue_frame_count(), glue_dispatch_miss_count());
    return 0;
}
