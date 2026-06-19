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
    bool want_window = false;
    int  win_scale = 3;
    bool interp = false;       /* oracle: run the reference superzazu interpreter */

    bool frames_set = false;
    for (int i=1;i<argc;i++){
        if (strcmp(argv[i],"--frames")==0 && i+1<argc){ frames = strtoull(argv[++i],NULL,0); frames_set = true; }
        else if (strcmp(argv[i],"--gg")==0) force_gg = 1;
        else if (strcmp(argv[i],"--sms")==0) force_gg = 0;
        else if (strcmp(argv[i],"--dump-frame")==0 && i+1<argc) dump_frame = strtoull(argv[++i],NULL,0);
        else if (strcmp(argv[i],"--dump-out")==0 && i+1<argc) dump_out = argv[++i];
        else if (strcmp(argv[i],"--force-display")==0){ extern int g_vdp_force_display; g_vdp_force_display=1; }
        else if (strcmp(argv[i],"--interp")==0) interp = true;
        else if (strcmp(argv[i],"--window")==0){
            want_window = true;
            if (i+1<argc && argv[i+1][0] != '-') win_scale = (int)strtol(argv[++i],NULL,0);
        }
        else if (argv[i][0] != '-') rom = argv[i];
        else fprintf(stderr,"[runner] unknown arg: %s\n", argv[i]);
    }
    if (!rom){
        fprintf(stderr,"usage: %s <rom.sms|rom.gg> [--frames N] [--gg|--sms]\n", argv[0]);
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

    if (interp){
        fprintf(stderr,"[runner] ORACLE reference mode (superzazu interpreter)\n");
        glue_run_interp();
    } else {
        glue_run();
    }

#ifdef SMS_HAVE_SDL
    if (want_window) host_shutdown();
#endif

    fprintf(stderr,"[runner] stopped after %llu frames; dispatch misses: %d\n",
            (unsigned long long)glue_frame_count(), glue_dispatch_miss_count());
    return 0;
}
