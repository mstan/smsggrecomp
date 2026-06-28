/* smsref — headless Genesis Plus GX SMS/GG reference oracle (Phase A: dump).
 *
 * Loads an SMS/GG ROM, runs to a list of frames, and dumps VRAM/CRAM/Z80 state
 * in the SAME labeled/raw format the recomp emits, so tools/oracle/{vdp_diff,
 * state_diff}.py diff it unchanged. TCP server + chip_ring come next.
 *
 *   smsref <rom> --frames N --dump "450,1200,1600" --out PREFIX
 *
 * Per dumped frame F: PREFIX_F.vram (16KB), PREFIX_F.cram (raw), PREFIX_F.cpu
 * (labeled Z80 regs).  GPGX is an OFFLINE oracle only (never shipped).
 */
#include "shared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

/* ---- frontend contract the core expects (osd.h) ---- */
t_config config;
char GG_ROM[256], AR_ROM[256], SK_ROM[256], SK_UPMEM[256], GG_BIOS[256];
char MD_BIOS[256], CD_BIOS_EU[256], CD_BIOS_US[256], CD_BIOS_JP[256];
char MS_BIOS_US[256], MS_BIOS_EU[256], MS_BIOS_JP[256];

/* NTSC filter contexts (frontend-owned; unused since config.ntsc=0) */
#include "md_ntsc.h"
#include "sms_ntsc.h"
md_ntsc_t  *md_ntsc  = NULL;
sms_ntsc_t *sms_ntsc = NULL;

void osd_input_update(void) {}
void ROMCheatUpdate(void) {}

int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension)
{
    FILE *f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "smsref: cannot open %s\n", filename); return 0; }
    int n = (int)fread(buffer, 1, maxsize, f);
    if (extension) {
        /* GPGX detects by the LAST 3 filename chars: SMS at [0..2]=="SMS",
         * GG at [1..2]=="GG" (loadrom.c). ".gg" -> ".GG" matches GG. */
        size_t L = strlen(filename);
        if (L >= 3) { memcpy(extension, filename + L - 3, 3); extension[3] = 0; }
        else extension[0] = 0;
    }
    fclose(f);
    return n;
}

static void set_config_defaults(void)
{
    memset(&config, 0, sizeof config);
    config.system        = 0;   /* auto-detect from ROM */
    config.region_detect = 0;   /* auto */
    config.vdp_mode      = 0;   /* auto */
    config.master_clock  = 0;   /* auto */
    config.ym2413        = 2;   /* AUTO (enable SMS FM only if the ROM uses it) */
    config.ym2612        = 0;
    config.hq_psg        = 0;
    config.psg_preamp    = 150;
    config.fm_preamp     = 100;
    config.filter        = 0;
    config.lp_range      = 0x9999;
    config.render        = 0;
    config.overscan      = 0;
    config.ntsc          = 0;
    config.no_sprite_limit = 1;
    config.gg_extra      = 0;
    config.left_border   = 0;
    for (int i = 0; i < MAX_INPUTS; i++) { config.input[i].device = -1; config.input[i].port = i; }
    config.input[0].device = 0;   /* gamepad on port 0 */
    config.input[1].device = 0;
}

static uint16_t g_fb[1024 * 512];   /* RGB565 scratch framebuffer */

static void dump_frame(const char *prefix, int frame)
{
    char p[1024]; FILE *f;
    snprintf(p, sizeof p, "%s_%d.vram", prefix, frame);
    f = fopen(p, "wb"); if (f) { fwrite(vram, 1, 0x4000, f); fclose(f); }   /* SMS uses 16KB */
    /* GPGX stores SMS CRAM at a 2-byte stride (6-bit value in even byte, odd=0);
     * de-stride to the 32-byte raw SMS CRAM the recomp/Mesen emit. (GG's 12-bit
     * 2-byte/entry CRAM would be dumped raw — handle when GG is added.) */
    snprintf(p, sizeof p, "%s_%d.cram", prefix, frame);
    f = fopen(p, "wb");
    if (f) { uint8_t sc[32]; for (int i = 0; i < 32; i++) sc[i] = cram[i * 2]; fwrite(sc, 1, 32, f); fclose(f); }
    snprintf(p, sizeof p, "%s_%d.cpu", prefix, frame);
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "a=%u\nf=%u\nb=%u\nc=%u\nd=%u\ne=%u\nh=%u\nl=%u\n",
            Z80.af.b.h, Z80.af.b.l, Z80.bc.b.h, Z80.bc.b.l,
            Z80.de.b.h, Z80.de.b.l, Z80.hl.b.h, Z80.hl.b.l);
        fprintf(f, "a_=%u\nf_=%u\nb_=%u\nc_=%u\nd_=%u\ne_=%u\nh_=%u\nl_=%u\n",
            Z80.af2.b.h, Z80.af2.b.l, Z80.bc2.b.h, Z80.bc2.b.l,
            Z80.de2.b.h, Z80.de2.b.l, Z80.hl2.b.h, Z80.hl2.b.l);
        fprintf(f, "ix=%u\niy=%u\nsp=%u\npc=%u\nwz=%u\ni=%u\nr=%u\n"
                   "iff1=%u\niff2=%u\nim=%u\nhalted=%u\n",
            Z80.ix.w.l, Z80.iy.w.l, Z80.sp.w.l, Z80.pc.w.l, Z80.wz.w.l,
            Z80.i, (Z80.r & 0x7f) | (Z80.r2 & 0x80),
            Z80.iff1 ? 1u : 0u, Z80.iff2 ? 1u : 0u, Z80.im, Z80.halt ? 1u : 0u);
        fclose(f);
    }
    fprintf(stderr, "smsref: dumped frame %d\n", frame);
}

/* WAV header (S16 stereo); patched with frame count at the end. */
static void wav_hdr(FILE *f, uint32_t rate, uint32_t nframes){
    uint32_t bytes=nframes*4, byterate=rate*4, riff=36+bytes, fmtlen=16; uint16_t pcm=1,ch=2,ba=4,bits=16;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVE",1,4,f); fwrite("fmt ",1,4,f);
    fwrite(&fmtlen,4,1,f); fwrite(&pcm,2,1,f); fwrite(&ch,2,1,f); fwrite(&rate,4,1,f);
    fwrite(&byterate,4,1,f); fwrite(&ba,2,1,f); fwrite(&bits,2,1,f); fwrite("data",1,4,f); fwrite(&bytes,4,1,f);
}

/* ------------------------------------------------------------------ server */
static long g_frame_no = 0;   /* frames advanced since reset */

static uint64_t fnv1a(const void *d, size_t n){
    const uint8_t *p = d; uint64_t h = 1469598103934665603ULL;
    for (size_t i=0;i<n;i++){ h ^= p[i]; h *= 1099511628211ULL; } return h;
}
static void hexenc(const uint8_t *d, size_t n, char *out){
    static const char *H="0123456789abcdef";
    for (size_t i=0;i<n;i++){ out[2*i]=H[d[i]>>4]; out[2*i+1]=H[d[i]&15]; } out[2*n]=0;
}
static void sline(SOCKET c, const char *s){ send(c,s,(int)strlen(s),0); send(c,"\n",1,0); }

/* tiny field extractors (no JSON lib): find "key": then read int */
static long jint(const char *s, const char *key, long dflt){
    const char *p = strstr(s, key); if (!p) return dflt; p += strlen(key);
    while (*p && (*p==' '||*p==':'||*p=='"')) p++;
    return (*p=='-'||(*p>='0'&&*p<='9')) ? strtol(p,NULL,0) : dflt;
}

static void cpu_json(char *o){
    sprintf(o,"{\"frame\":%ld,\"a\":%u,\"f\":%u,\"b\":%u,\"c\":%u,\"d\":%u,\"e\":%u,"
        "\"h\":%u,\"l\":%u,\"ix\":%u,\"iy\":%u,\"sp\":%u,\"pc\":%u,\"wz\":%u,"
        "\"i\":%u,\"r\":%u,\"iff1\":%u,\"iff2\":%u,\"im\":%u,\"halted\":%u}",
        g_frame_no, Z80.af.b.h,Z80.af.b.l,Z80.bc.b.h,Z80.bc.b.l,Z80.de.b.h,Z80.de.b.l,
        Z80.hl.b.h,Z80.hl.b.l,Z80.ix.w.l,Z80.iy.w.l,Z80.sp.w.l,Z80.pc.w.l,Z80.wz.w.l,
        Z80.i,(Z80.r&0x7f)|(Z80.r2&0x80),Z80.iff1?1:0,Z80.iff2?1:0,Z80.im,Z80.halt?1:0);
}

static int run_server(int port){
    WSADATA w; if (WSAStartup(MAKEWORD(2,2),&w)) return 1;
    SOCKET ls = socket(AF_INET,SOCK_STREAM,0);
    int yes=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,(char*)&yes,sizeof yes);
    struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET;
    a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons((u_short)port);
    if (bind(ls,(struct sockaddr*)&a,sizeof a) || listen(ls,1)){ fprintf(stderr,"smsref: bind/listen %d failed\n",port); return 1; }
    fprintf(stderr,"smsref: server on 127.0.0.1:%d\n",port);
    static char vhex[0x8000+1]; static char obuf[0x9000];
    for (;;){
        SOCKET c = accept(ls,NULL,NULL); if (c==INVALID_SOCKET) break;
        char line[512]; int li=0; int done=0;
        while (!done){
            char ch; int r=recv(c,&ch,1,0); if (r<=0) break;
            if (ch=='\n'){
                line[li]=0; li=0;
                if      (strstr(line,"\"ping\"")) sline(c,"{\"ok\":true,\"sys\":\"smsref\"}");
                else if (strstr(line,"\"quit\"")){ sline(c,"{\"ok\":true}"); done=1; }
                else if (strstr(line,"\"reset\"")){ system_reset(); g_frame_no=0; sline(c,"{\"ok\":true}"); }
                else if (strstr(line,"\"frame\"") && !strstr(line,"run")){ char o[64]; sprintf(o,"{\"frame\":%ld}",g_frame_no); sline(c,o); }
                else if (strstr(line,"\"run_to\"")){ long tgt=jint(line,"\"frame\"",g_frame_no);
                        while (g_frame_no<tgt){ system_frame_sms(0); g_frame_no++; } char o[64]; sprintf(o,"{\"frame\":%ld}",g_frame_no); sline(c,o); }
                else if (strstr(line,"\"run\"")){ long k=jint(line,"\"frames\"",1);
                        for (long i=0;i<k;i++){ system_frame_sms(0); g_frame_no++; } char o[64]; sprintf(o,"{\"frame\":%ld}",g_frame_no); sline(c,o); }
                else if (strstr(line,"\"regs\"")){ cpu_json(obuf); sline(c,obuf); }
                else if (strstr(line,"\"state\"")){
                        sprintf(obuf,"{\"frame\":%ld,\"vram_h\":\"%016llx\",\"cram_h\":\"%016llx\",\"pc\":%u}",
                            g_frame_no,(unsigned long long)fnv1a(vram,0x4000),
                            (unsigned long long)fnv1a(cram,0x40),Z80.pc.w.l); sline(c,obuf); }
                else if (strstr(line,"\"read_vram\"")){ hexenc(vram,0x4000,vhex);
                        sprintf(obuf,"{\"vram\":\"%s\"}",vhex); sline(c,obuf); }
                else if (strstr(line,"\"read_cram\"")){ uint8_t sc[32]; for(int i=0;i<32;i++) sc[i]=cram[i*2];
                        char h[65]; hexenc(sc,32,h); sprintf(obuf,"{\"cram\":\"%s\"}",h); sline(c,obuf); }
                else if (strstr(line,"\"read_ram\"")){ long ad=jint(line,"\"addr\"",0),len=jint(line,"\"len\"",16);
                        if(len>0x2000)len=0x2000; static char rh[0x4001]; hexenc(work_ram+(ad&0x1fff),(size_t)len,rh);
                        sprintf(obuf,"{\"ram\":\"%s\"}",rh); sline(c,obuf); }
                else sline(c,"{\"error\":\"unknown cmd\"}");
            } else if (li<(int)sizeof line-1) line[li++]=ch;
        }
        closesocket(c);
    }
    closesocket(ls); WSACleanup(); return 0;
}

int main(int argc, char **argv)
{
    const char *rom = NULL, *out = "smsref", *dumplist = "", *wavpath = NULL;
    int frames = 2000, server_port = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumplist = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wavpath = argv[++i];
        else if (!strcmp(argv[i], "--server") && i + 1 < argc) server_port = atoi(argv[++i]);
        else if (argv[i][0] != '-') rom = argv[i];
    }
    if (!rom) { fprintf(stderr, "usage: smsref <rom> [--server PORT | --frames N --dump F1,F2 --out PREFIX]\n"); return 2; }

    set_config_defaults();
    memset(g_fb, 0, sizeof g_fb);
    bitmap.width  = 1024;
    bitmap.height = 512;
    bitmap.pitch  = 1024 * 2;       /* RGB565 */
    bitmap.data   = (uint8_t *)g_fb;

    if (!load_rom((char *)rom)) { fprintf(stderr, "smsref: load_rom failed\n"); return 1; }
    audio_init(44100, 0);
    system_init();
    system_reset();
    fprintf(stderr, "smsref: loaded %s, system_hw=0x%02X\n", rom, system_hw);

    if (server_port) return run_server(server_port);

    /* parse dump-frame list into a sorted-enough lookup */
    int want[64], nwant = 0;
    { char buf[256]; strncpy(buf, dumplist, sizeof buf - 1); buf[sizeof buf - 1] = 0;
      for (char *t = strtok(buf, ","); t && nwant < 64; t = strtok(NULL, ",")) want[nwant++] = atoi(t); }

    FILE *wav = NULL; uint64_t wav_total = 0; const uint32_t WRATE = 44100;
    static int16_t abuf[8192];
    if (wavpath) { wav = fopen(wavpath, "wb"); if (wav) wav_hdr(wav, WRATE, 0); }

    for (int fr = 1; fr <= frames; fr++) {
        system_frame_sms(0);
        if (wav) { int n = audio_update(abuf); if (n > 0) { fwrite(abuf, 4, n, wav); wav_total += n; } }
        for (int k = 0; k < nwant; k++) if (want[k] == fr) dump_frame(out, fr);
    }
    if (wav) { fseek(wav, 0, SEEK_SET); wav_hdr(wav, WRATE, (uint32_t)wav_total); fclose(wav);
        fprintf(stderr, "smsref: wrote %llu audio frames @ %u Hz -> %s\n",
            (unsigned long long)wav_total, WRATE, wavpath); }
    fprintf(stderr, "smsref: done (%d frames)\n", frames);
    return 0;
}
