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
        const char *dot = strrchr(filename, '.');
        if (dot && strlen(dot + 1) < 4) strcpy(extension, dot + 1);
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

int main(int argc, char **argv)
{
    const char *rom = NULL, *out = "smsref", *dumplist = "";
    int frames = 2000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumplist = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] != '-') rom = argv[i];
    }
    if (!rom) { fprintf(stderr, "usage: smsref <rom> --frames N --dump F1,F2 --out PREFIX\n"); return 2; }

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
    fprintf(stderr, "smsref: loaded %s, system_hw=0x%02X, running %d frames\n",
            rom, system_hw, frames);

    /* parse dump-frame list into a sorted-enough lookup */
    int want[64], nwant = 0;
    { char buf[256]; strncpy(buf, dumplist, sizeof buf - 1); buf[sizeof buf - 1] = 0;
      for (char *t = strtok(buf, ","); t && nwant < 64; t = strtok(NULL, ",")) want[nwant++] = atoi(t); }

    for (int fr = 1; fr <= frames; fr++) {
        system_frame_sms(0);
        for (int k = 0; k < nwant; k++) if (want[k] == fr) dump_frame(out, fr);
    }
    fprintf(stderr, "smsref: done (%d frames)\n", frames);
    return 0;
}
