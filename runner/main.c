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
#include "services.h"
#include "coverage_route.h"
#ifdef SMSGG_HAVE_GAME_LAYOUT
#include "game_spec.h"
#endif
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <stdbool.h>
#include <stdlib.h>
static bool g_automated_input;
static uint64_t g_route_release;
static bool g_service_window,g_use_route;
static int runtime_boundary(uint64_t frame){
    if(g_use_route&&coverage_route_frame(frame))return 1;
    if(!g_service_window){sms_services_pad(glue_get_pad1());return sms_services_frame();}
    return 0;
}
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static bool g_benchmark;
static int g_press_n;
static uint64_t g_audio_frames;
static uint64_t g_benchmark_start;
static double g_route_cpu=-1;
static double cpu_seconds(void){
#ifdef _WIN32
    FILETIME a,b,k,u; ULARGE_INTEGER x,y;
    GetProcessTimes(GetCurrentProcess(), &a,&b,&k,&u);
    x.LowPart=k.dwLowDateTime; x.HighPart=k.dwHighDateTime;
    y.LowPart=u.dwLowDateTime; y.HighPart=u.dwHighDateTime;
    return (x.QuadPart+y.QuadPart)*1e-7;
#else
    return (double)clock()/CLOCKS_PER_SEC;
#endif
}
static int benchmark_frame(const uint32_t *fb, int w, int h){
    if(g_route_cpu<0 && glue_frame_count()>=g_benchmark_start)g_route_cpu=cpu_seconds();
    (void)fb; (void)w; (void)h; return 0;
}

#ifdef SMS_HAVE_SDL
#include "host_sdl.h"
#include "video/sms_vdp.h"
/* Bridge the runner's per-frame callback to the SDL host. Returns nonzero to
 * request shutdown (glue.c then unwinds the run loop). */
static int sdl_frame_cb(const uint32_t *fb, int w, int h){
    if(g_benchmark)benchmark_frame(fb,w,h);
    if(!g_benchmark)host_set_prepare_pacing(sms_services_fast_prepare());
    bool keep = host_present(fb, w, h);
    if(g_route_release&&glue_frame_count()>=g_route_release){g_automated_input=false;g_route_release=0;}
    if (!g_automated_input&&!sms_services_input_override()) {glue_set_pad1(host_get_pad1());sms_services_pad(host_get_pad1());}
    else sms_services_pad(glue_get_pad1());
    return keep&&!sms_services_frame() ? 0 : 1;
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

static uint64_t g_cap_wav_from, g_cap_wav_to = ~0ull;   /* capture mode: WAV spans frames [from,to] */
static bool g_cap_wav_mode;
static void audio_sink(const int16_t *frames, size_t n){
    g_audio_frames += n;
    if (g_cap_wav_mode){   /* sink runs after the frame counter ticks: count-1 is the frame just ended */
        uint64_t idx = glue_frame_count() - 1;
        if (idx < g_cap_wav_from || idx > g_cap_wav_to) return;
    }
    if (g_wav){
        fwrite(frames, 2*sizeof(int16_t), n, g_wav);
        g_wav_data_bytes += (uint32_t)(n * 2 * sizeof(int16_t));
    }
#ifdef SMS_HAVE_SDL
    if (g_audio_live&&!sms_services_fast_prepare()) host_audio_submit(frames, n);
#endif
}

/* ---- headless scripted input (--press FRAME:KEYS, repeatable) ----
 * KEYS letters: U D L R (d-pad), A=button1, B=button2, S=start; empty = release.
 * Each directive sets player 1's held buttons from its frame until a later one.
 * Deterministic input for automated tests — no window needed. */
static struct { uint64_t frame; uint8_t mask; } g_press[32];

static uint8_t press_keys_to_mask(const char *s){
    uint8_t m = 0;
    for (; *s; s++){
        switch (*s){
            case 'U': case 'u': m|=SMS_PAD_UP;    break;
            case 'D': case 'd': m|=SMS_PAD_DOWN;  break;
            case 'L': case 'l': m|=SMS_PAD_LEFT;  break;
            case 'R': case 'r': m|=SMS_PAD_RIGHT; break;
            case 'A': case 'a': m|=SMS_PAD_B1;    break;
            case 'B': case 'b': m|=SMS_PAD_B2;    break;
            case 'S': case 's': m|=SMS_PAD_START; break;
            default: break;   /* ignore separators */
        }
    }
    return m;
}
static void press_add(const char *tok){
    if (g_press_n >= (int)(sizeof g_press / sizeof g_press[0])) return;
    const char *colon = strchr(tok, ':');
    g_press[g_press_n].frame = strtoull(tok, NULL, 0);
    g_press[g_press_n].mask  = press_keys_to_mask(colon ? colon+1 : "");
    g_press_n++;
}
static uint8_t press_provider(uint64_t frame){
    uint8_t m = 0; uint64_t best = 0; int found = 0;
    for (int i = 0; i < g_press_n; i++)
        if (g_press[i].frame <= frame && (!found || g_press[i].frame >= best)){
            best = g_press[i].frame; m = g_press[i].mask; found = 1;
        }
    return m;   /* the latest directive at/under the current frame wins */
}


/* ---- frame-indexed input script (--input-script FILE) ----
 * "<frame> [P1:]BUTTONS" per line ("-" = release all), BUTTONS joined by '+':
 * UP DOWN LEFT RIGHT A B C START. Same format as nesrecomp --input; each line
 * REPLACES player 1's held buttons from that frame. Lines for P2+ are ignored
 * (SMS/GG runner models one pad). '#' starts a comment. */
static struct { uint64_t frame; uint8_t mask; } *g_script; static int g_script_n, g_script_cap;
static uint8_t script_name_mask(const char *t){
    if(!strcmp(t,"UP"))return SMS_PAD_UP; if(!strcmp(t,"DOWN"))return SMS_PAD_DOWN;
    if(!strcmp(t,"LEFT"))return SMS_PAD_LEFT; if(!strcmp(t,"RIGHT"))return SMS_PAD_RIGHT;
    if(!strcmp(t,"A")||!strcmp(t,"B1"))return SMS_PAD_B1;
    if(!strcmp(t,"B")||!strcmp(t,"C")||!strcmp(t,"B2"))return SMS_PAD_B2;
    if(!strcmp(t,"START"))return SMS_PAD_START;
    return 0;
}
static bool script_load(const char *path){
    FILE *f=fopen(path,"r"); if(!f){fprintf(stderr,"[runner] cannot open input script %s\n",path);return false;}
    char line[256]; int ln=0;
    while(fgets(line,sizeof line,f)){
        ln++; char *c=strchr(line,'#'); if(c)*c=0;
        char *p=line; while(*p==' '||*p=='\t')p++;
        if(!*p||*p=='\n'||*p=='\r')continue;
        char *q; uint64_t fr=strtoull(p,&q,10); if(q==p){fprintf(stderr,"[runner] %s:%d: bad frame\n",path,ln);fclose(f);return false;}
        while(*q==' '||*q=='\t')q++;
        int player=1;
        if((q[0]=='P'||q[0]=='p')&&q[1]>='1'&&q[1]<='9'&&q[2]==':'){player=q[1]-'0';q+=3;}
        q[strcspn(q,"\r\n")]=0;
        uint8_t m=0; char buf[160]; snprintf(buf,sizeof buf,"%s",q);
        if(strcmp(buf,"-")&&*buf)for(char *t=strtok(buf,"+ ");t;t=strtok(NULL,"+ ")){
            for(char *u=t;*u;u++)if(*u>='a'&&*u<='z')*u=(char)(*u-32);
            uint8_t b=script_name_mask(t); if(!b){fprintf(stderr,"[runner] %s:%d: bad button %s\n",path,ln,t);fclose(f);return false;} m|=b;
        }
        if(player!=1)continue;
        if(g_script_n==g_script_cap){g_script_cap=g_script_cap?g_script_cap*2:1024;g_script=realloc(g_script,(size_t)g_script_cap*sizeof *g_script);if(!g_script){fclose(f);return false;}}
        g_script[g_script_n].frame=fr;g_script[g_script_n].mask=m;g_script_n++;
    }
    fclose(f);
    fprintf(stderr,"[runner] input script: %d directive(s) from %s\n",g_script_n,path);
    return true;
}
static uint8_t script_provider(uint64_t frame){
    uint8_t m=0; uint64_t best=0; int found=0;
    for(int i=0;i<g_script_n;i++)
        if(g_script[i].frame<=frame&&(!found||g_script[i].frame>=best)){best=g_script[i].frame;m=g_script[i].mask;found=1;}
    return m;
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
    sms_settings_load();
    int launcher_mode=argc==1?1:0;
    char picked_rom[1024];
    unsigned debug_port=65536;uint64_t pause_at=0;bool show_at_ready=false;const char *status_path=NULL;
    bool launcher_used=false;
    const char *rom = NULL;
    uint64_t frames = 600;
    int force_gg = -1;
    uint64_t dump_frame = 0;
    const char *dump_out = NULL;
    const char *vdp_trace = NULL;   /* per-frame VDP-state CSV (oracle diff) */
    const char *psg_log = NULL;     /* chip_ring: (cyc,val) PSG-write log */
    bool want_window = false;
    int  win_scale = 3;
    bool interp = false;       /* oracle: run the reference superzazu interpreter */
    const char *audio_wav = NULL;   /* dump PSG output to this WAV */
    bool mute = false;              /* suppress live SDL audio (window build) */
    const char *route = NULL, *report = NULL;
    const char *benchmark_out = NULL;
    const char *gpu_report = NULL, *gpu_dump = NULL;
    int internal_scale=0; /* ordinary native SDL until internal resolution is requested */
    bool hidden=false;
    /* Headless capture contract (see README "Headless capture"). */
    const char *capture_out=NULL, *capture_wav=NULL, *input_script=NULL;
    int capture_w=0, capture_h=0; uint64_t capture_from=0, capture_to=~0ull, capture_every=1, exit_at=0;
    bool capture_mode=false;
    double start_cpu;
#ifdef SMS_CYC_WATCH
    const char *cyc_watch_file = NULL; uint16_t cyc_watch_addr = 0;
#endif

    bool frames_set = false;
    for (int i=1;i<argc;i++){
        if (strcmp(argv[i],"--frames")==0 && i+1<argc){ frames = strtoull(argv[++i],NULL,0); frames_set = true; }
        else if (strcmp(argv[i],"--launcher")==0) launcher_mode=1;
        else if (strcmp(argv[i],"--no-launcher")==0) launcher_mode=-1;
        else if (strcmp(argv[i],"--debug-port")==0&&i+1<argc){char *end;unsigned long value=strtoul(argv[++i],&end,10);if(*end||value>65535)return 2;debug_port=(unsigned)value;}
        else if (strcmp(argv[i],"--pause-at")==0&&i+1<argc) pause_at=strtoull(argv[++i],NULL,10);
        else if (strcmp(argv[i],"--show-at-ready")==0) show_at_ready=true;
        else if (strcmp(argv[i],"--status-file")==0&&i+1<argc) status_path=argv[++i];
        else if (strcmp(argv[i],"--gg")==0) force_gg = 1;
        else if (strcmp(argv[i],"--sms")==0) force_gg = 0;
        else if (strcmp(argv[i],"--dump-frame")==0 && i+1<argc) dump_frame = strtoull(argv[++i],NULL,0);
        else if (strcmp(argv[i],"--dump-out")==0 && i+1<argc) dump_out = argv[++i];
        else if (strcmp(argv[i],"--vdp-trace")==0 && i+1<argc) vdp_trace = argv[++i];
        else if (strcmp(argv[i],"--psg-log")==0 && i+1<argc) psg_log = argv[++i];
        else if (strcmp(argv[i],"--force-display")==0){ extern int g_vdp_force_display; g_vdp_force_display=1; }
        else if (strcmp(argv[i],"--interp")==0) interp = true;
        else if (strcmp(argv[i],"--audio-wav")==0 && i+1<argc) audio_wav = argv[++i];
        else if (strcmp(argv[i],"--mute")==0) mute = true;
        else if (strcmp(argv[i],"--benchmark")==0) g_benchmark = true;
        else if (strcmp(argv[i],"--benchmark-start")==0 && i+1<argc) g_benchmark_start=strtoull(argv[++i],NULL,10);
        else if (strcmp(argv[i],"--benchmark-output")==0 && i+1<argc) benchmark_out=argv[++i];
        else if (strcmp(argv[i],"--internal-scale")==0 && i+1<argc){
            internal_scale=(int)strtol(argv[++i],NULL,10);
            if(internal_scale!=1&&internal_scale!=2&&internal_scale!=4&&internal_scale!=8&&internal_scale!=16){
                fprintf(stderr,"internal scale must be 1, 2, 4, 8 or 16\n");return 2;
            }
        }
        else if (strcmp(argv[i],"--hidden")==0) hidden=true;
        else if (strcmp(argv[i],"--gpu-report")==0 && i+1<argc) gpu_report=argv[++i];
        else if (strcmp(argv[i],"--gpu-dump")==0 && i+1<argc) gpu_dump=argv[++i];
#ifdef SMS_CYC_WATCH
        else if (strcmp(argv[i],"--cyc-watch")==0 && i+2<argc){
            cyc_watch_addr = (uint16_t)strtoul(argv[++i],NULL,0); cyc_watch_file = argv[++i]; }
#endif
        else if (strcmp(argv[i],"--press")==0 && i+1<argc) press_add(argv[++i]);
        else if (strcmp(argv[i],"--route")==0 && i+1<argc) route=argv[++i];
        else if (strcmp(argv[i],"--report")==0 && i+1<argc) report=argv[++i];
        else if (strcmp(argv[i],"--capture-out")==0 && i+1<argc){ capture_out=argv[++i]; capture_mode=true; }
        else if (strcmp(argv[i],"--capture-size")==0 && i+1<argc){
            if (sscanf(argv[++i],"%dx%d",&capture_w,&capture_h)!=2 || capture_w<16 || capture_h<16 || capture_w>16384 || capture_h>16384){
                fprintf(stderr,"[runner] --capture-size requires WxH\n");return 2; }
            capture_mode=true; }
        else if (strcmp(argv[i],"--capture-from")==0 && i+1<argc) capture_from=strtoull(argv[++i],NULL,0);
        else if (strcmp(argv[i],"--capture-to")==0 && i+1<argc) capture_to=strtoull(argv[++i],NULL,0);
        else if (strcmp(argv[i],"--capture-every")==0 && i+1<argc){ capture_every=strtoull(argv[++i],NULL,0); if(!capture_every)capture_every=1; }
        else if (strcmp(argv[i],"--capture-wav")==0 && i+1<argc){ capture_wav=argv[++i]; capture_mode=true; }
        else if (strcmp(argv[i],"--exit-at-frame")==0 && i+1<argc){ exit_at=strtoull(argv[++i],NULL,0); }
        else if (strcmp(argv[i],"--input-script")==0 && i+1<argc) input_script=argv[++i];
        else if (strcmp(argv[i],"--window")==0){
            want_window = true;
            if (i+1<argc && argv[i+1][0] != '-') win_scale = (int)strtol(argv[++i],NULL,0);
        }
        else if (argv[i][0] != '-') rom = argv[i];
        else {fprintf(stderr,"[runner] unknown or incomplete arg: %s\n", argv[i]);return 2;}
    }
    if(interp){fprintf(stderr,"This host-native build requires generated coverage; interpreter play is unavailable.\n");return 2;}
    if(launcher_mode>=0&&(launcher_mode==1||(want_window&&!hidden&&!capture_mode&&!route&&!g_press_n&&!input_script))){
        int result=sms_launcher(rom,picked_rom,sizeof picked_rom);
        if(result!=0)return result==1?0:1;
        rom=picked_rom;want_window=true;win_scale=sms_settings_window_scale();launcher_used=true;
    }
    if (!rom){
        fprintf(stderr,"usage: %s <rom.sms|rom.gg> [--frames N] [--gg|--sms] "
                       "[--window [scale]] [--audio-wav out.wav] [--mute] "
                       "[--press FRAME:KEYS] [--input-script FILE] [--capture-out DIR --capture-size WxH --capture-from N --capture-to M --capture-every K --capture-wav FILE --exit-at-frame N] [--route route.txt] [--report out.json] [--interp]\n", argv[0]);
        return 2;
    }

    bool is_gg = (force_gg >= 0) ? (force_gg==1) : ends_with(rom, ".gg");
#ifdef SMSGG_HAVE_GAME_LAYOUT
    is_gg=g_game_spec.platform==GAME_PLATFORM_GG;
    if (force_gg>=0 && (force_gg==1)!=is_gg){
        fprintf(stderr,"[runner] platform override does not match this generated build\n");return 2;
    }
#endif

    if (capture_mode){
        /* Hidden window, no audio device, unpaced, deterministic stepping. */
        want_window = true; hidden = true; mute = true;
        if (exit_at) { frames = exit_at; frames_set = true; }
        else if (!frames_set && capture_to != ~0ull) { frames = capture_to + 1; frames_set = true; }
        else if (!frames_set) { fprintf(stderr,"[runner] capture mode needs --exit-at-frame, --capture-to or --frames\n"); return 2; }
        if (capture_wav) { audio_wav = capture_wav; g_cap_wav_mode = true; g_cap_wav_from = capture_from; g_cap_wav_to = capture_to; }
        if (capture_out) {
            char mk[700];
#ifdef _WIN32
            snprintf(mk, sizeof mk, "mkdir \"%s\" 2>nul", capture_out);
#else
            snprintf(mk, sizeof mk, "mkdir -p \"%s\" 2>/dev/null", capture_out);
#endif
            (void)system(mk);
        }
    }
    /* In windowed mode, default to run-until-closed unless --frames was given. */
    if (want_window && !frames_set) frames = 0;
    if(show_at_ready&&pause_at)hidden=true;

    fprintf(stderr,"[runner] SMS/GG recompiled runner (%s)\n",
            want_window ? "windowed" : "headless");
    if (!glue_load_rom(rom)) return 1;
    if (route){
        if (g_press_n || !coverage_route_load(route,glue_rom_crc32())) return 2;
        if (frames_set && frames && coverage_route_last_frame()>frames){coverage_route_close();return 2;}
        if (!frames_set&&!want_window)frames=coverage_route_last_frame()+1;
    }
    fprintf(stderr,"[runner] loaded %s (%llu bytes), platform=%s, running %s\n",
            rom, (unsigned long long)glue_rom_size(), is_gg?"GG":"SMS",
            frames ? "to frame limit" : "until window close");

    glue_init(is_gg, frames);
    if (route){
        g_automated_input=true;g_route_release=coverage_route_last_frame();
        g_use_route=true;
        if (coverage_route_frame(0)){coverage_route_close();return 1;}
    }
    glue_set_boundary_callback(runtime_boundary);
    if (g_benchmark) glue_set_frame_callback(benchmark_frame);
    if (dump_out) glue_set_dump(dump_frame ? dump_frame : frames, dump_out);
    if (vdp_trace) glue_set_vdp_trace(vdp_trace);
    if (psg_log) glue_set_psg_log(psg_log);
#ifdef SMS_CYC_WATCH
    if (cyc_watch_file){
        glue_set_cyc_watch(cyc_watch_addr, cyc_watch_file);
        fprintf(stderr,"[runner] cyc-watch anchor=0x%04X -> %s\n", cyc_watch_addr, cyc_watch_file);
    }
#endif
    if (input_script){
        if (!script_load(input_script)) return 2;
        g_automated_input=true;
        glue_set_input_cb(script_provider);
    } else if (g_press_n){
        g_automated_input=true;
        glue_set_input_cb(press_provider);
        fprintf(stderr,"[runner] scripted input: %d directive(s)\n", g_press_n);
    }

    if (want_window){
        (void)win_scale;   /* used only in the SDL block below */
#ifdef SMS_HAVE_SDL
        /* SMS shows the full 256x192; GG shows a 160x144 centred crop. */
        host_set_internal_scale(internal_scale);
        host_set_hidden(hidden);
        host_set_fullscreen(launcher_used?sms_settings_fullscreen():0);
        int cx = is_gg ? (SMS_SCREEN_W-GG_SCREEN_W)/2 : 0;
        int cy = is_gg ? (SMS_SCREEN_H-GG_SCREEN_H)/2 : 0;
        int cw = is_gg ? GG_SCREEN_W : SMS_SCREEN_W;
        int ch = is_gg ? GG_SCREEN_H : SMS_SCREEN_H;
        if (host_init(SMS_SCREEN_W, SMS_SCREEN_H, cx, cy, cw, ch, win_scale,
                      g_game_spec.display_name)){
            g_service_window=true;
            glue_set_frame_callback(sdl_frame_cb);
            glue_set_native_output(!host_uses_gpu());
            host_set_frame_cap((g_benchmark || capture_mode || pause_at) ? 0 : glue_frame_rate());
            if (capture_out) host_set_capture(capture_out, capture_w, capture_h, capture_from, capture_to, capture_every);
        } else
            {fprintf(stderr,"[runner] SDL window init failed\n");return 1;}
#else
        fprintf(stderr,"[runner] --window requested but built without SDL "
                       "(-DSMS_HAVE_SDL); running headless\n");
#endif
    }

    /* Audio outputs (independent; either/both may be active). */
    bool audio_on = false;
#ifdef SMS_HAVE_SDL
    if (want_window && !mute && sms_settings_audio_enabled()){
        if (host_audio_init(glue_audio_sample_rate())){ g_audio_live = true; audio_on = true; }
    }
#else
    (void)mute;
#endif
    if (audio_wav){
        if (wav_open(audio_wav, glue_audio_sample_rate())) audio_on = true;
    }
    if (audio_on || g_benchmark) glue_set_audio_sink(audio_sink);

    if(!sms_services_init(rom,debug_port,pause_at,show_at_ready,want_window,status_path))return 1;
    start_cpu = cpu_seconds();
    if (interp){
        fprintf(stderr,"[runner] ORACLE reference mode (superzazu interpreter)\n");
        glue_run_interp();
    } else {
        glue_run();
    }

    if (benchmark_out){
        FILE *f=fopen(benchmark_out,"w");
        if (!f){ perror(benchmark_out); return 1; }
        fprintf(f,"{\"frames\":%llu,\"cpu_ms\":%.6f,\"audio_frames\":%llu,\"dispatch_misses\":%d,\"route_frames\":%llu,\"route_cpu_ms\":%.6f}\n",
            (unsigned long long)glue_frame_count(),(cpu_seconds()-start_cpu)*1000,
            (unsigned long long)g_audio_frames,glue_dispatch_miss_count(),
            (unsigned long long)(glue_frame_count()>g_benchmark_start?glue_frame_count()-g_benchmark_start:0),
            g_route_cpu>=0?(cpu_seconds()-g_route_cpu)*1000:0);
        fclose(f);
    }
#ifdef SMS_HAVE_SDL
    if(gpu_report){FILE *f=fopen(gpu_report,"w");if(!f)return 1;host_report(f);fclose(f);}
    if(gpu_dump&&!host_dump_gpu(gpu_dump)){fprintf(stderr,"GPU dump failed\n");return 1;}
    if (g_audio_live) host_audio_shutdown();
    if (want_window) host_shutdown();
#endif
    wav_close();
    if (psg_log) glue_psg_log_close();
#ifdef SMS_CYC_WATCH
    if (cyc_watch_file) glue_cyc_watch_close();
#endif

    if (capture_mode)
        fprintf(stderr,"[capture] frames_written=%u present=%dx%d audio_samples=%llu wav=%s last_frame=%llu\n",
#ifdef SMS_HAVE_SDL
                (unsigned)host_capture_written(),
#else
                0u,
#endif
                capture_w, capture_h, (unsigned long long)g_audio_frames, capture_wav?capture_wav:"none",
                (unsigned long long)glue_frame_count());
    fprintf(stderr,"[runner] stopped after %llu frames; dispatch misses: %d\n",
            (unsigned long long)glue_frame_count(), glue_dispatch_miss_count());
    bool services_ok=sms_services_close();
    bool ok=services_ok&&!coverage_route_failed()&&glue_dispatch_miss_count()==0;
    if (report && !glue_write_report(report))ok=false;
    coverage_route_close();
    return ok?0:1;
}
