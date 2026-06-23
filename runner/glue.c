/*
 * glue.c - SMS/GG runner core: CPU state, bus, I/O, VDP timing + interrupt
 * delivery, and the dispatch-miss surface.
 *
 * Execution model (mirrors the genesis/NES siblings): the recompiled reset
 * routine never returns. Generated code calls sms_tick() after every
 * instruction; when the cycle counter reaches the next scheduled VDP event,
 * sms_sync() advances the VDP line-by-line, latches line/frame interrupts, and
 * - when IFF1 is set - takes the IM1 interrupt by calling the handler at
 * 0x0038 (its RET unwinds back here). The frame limit longjmps out of the
 * running game for a clean headless shutdown.
 *
 * call_by_address() is GENERATED (<prefix>_dispatch.c); this file provides the
 * sms_dispatch_miss() it falls back to, plus sms_sync/sms_halt/the bus.
 */
#include "glue.h"
#include "include/sms_runtime.h"
#include "include/sms_clocks.h"
#include "video/sms_vdp.h"
#include "external/superzazu/z80.h"
#include "png_write.h"
#include "audio/sn76489.h"
#include "jit/shard_jit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/* ---- CPU + memory ---- */
Z80State  g_z80;
uint64_t  g_sync_deadline;
uint64_t  g_frame_ic;   /* per-frame instruction count probe */

static uint8_t  *g_rom;
static size_t    g_rom_size;
static uint8_t   g_ram[0x2000];     /* 8 KB system RAM ($C000-$DFFF) */
static int       g_bank[3] = {0,1,2};
static bool      g_mapper_cm;        /* Codemasters mapper */

/* ---- timing ---- */
static uint64_t  g_next_line_cyc;
static uint64_t  g_frame;
static uint64_t  g_frame_limit;
static jmp_buf   g_quit_env;
static bool      g_running;

/* ---- always-on timing instrumentation (PRINCIPLES #17: query, don't probe) --
 * g_frame counts VDP frame boundaries (== real VBlanks). g_irq_taken counts how
 * many times the IM1 handler actually ran; if it exceeds g_frame the ISR is
 * re-entering within a single real frame (the suspected ~3x over-count source).
 * g_sync_maxdepth tracks the deepest re-entrant sms_sync nesting. */
static uint64_t  g_irq_taken;
static uint64_t  g_irq_reentrant;   /* take_irq calls that fired inside the ISR */
static int       g_sync_depth;
static int       g_sync_maxdepth;

/* ---- recomp-vs-hybrid execution split (always-on) ----
 * Every dispatch miss runs the routine under the superzazu interpreter. We
 * accumulate the Z80 cycles each such call consumes; comparing that to total
 * Z80 cycles (g_z80.cyc) gives the fraction of emulated time spent in the
 * interpreter fallback vs the statically-recompiled code. */
static uint64_t  g_hybrid_cyc;      /* Z80 cycles executed inside hybrid_interpret */
static uint64_t  g_hybrid_calls;    /* number of hybrid invocations */

/* ---- frame dump ---- */
static uint64_t  g_dump_frame = (uint64_t)-1;
static const char *g_dump_path;
static uint32_t  g_fb[SMS_SCREEN_W * SMS_SCREEN_H];

/* ---- live per-frame callback (SDL host) ---- */
static int (*g_frame_cb)(const uint32_t *fb, int w, int h);

/* ---- platform + audio ----
 * g_is_gg gates the GG-only stereo register ($06). The PSG is advanced from
 * advance_vdp() (the single VDP-timing choke point shared by the recomp,
 * hybrid, and interp paths) so audio stays in lockstep with video on every
 * path; g_psg_cyc tracks the absolute Z80 cycle already fed to the PSG.
 * Synthesis only runs when an audio sink is attached, so headless oracle/diff
 * runs pay nothing. The sink is handed interleaved-stereo frames each video
 * frame (the SDL audio host or a WAV writer). */
static bool      g_is_gg;
static uint64_t  g_psg_cyc;
static void    (*g_audio_sink)(const int16_t *stereo_frames, size_t frame_count);

/* ---- input ---- live controller masks (SMS_PAD_* bits, 1 = pressed). Driven
 * by the host each frame; read (active-low) by sms_io_in. Both run paths read
 * through the same sms_io_in, so input reaches recomp and hybrid/interp alike. */
static uint8_t   g_pad1, g_pad2;
static uint8_t (*g_input_cb)(uint64_t frame);   /* headless scripted input */

/* ---- per-frame VDP-state trace (oracle: query, don't probe) ----
 * When enabled, every frame appends a hash of the FULL VDP state (VRAM, CRAM,
 * registers) so a recomp run and an --interp reference run can be diffed
 * frame-by-frame to find the FIRST frame whose VDP STATE (not pixels) diverges.
 * That discriminates a renderer/shared bug (identical state, different pixels)
 * from a CPU/recomp bug (state itself diverges) and is immune to the hybrid-
 * overlap confound, since it compares actual machine state rather than output. */
static FILE *g_vdp_trace;
static uint64_t fnv1a64(const void *buf, size_t n){
    const uint8_t *p = (const uint8_t*)buf;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i=0;i<n;i++){ h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* ---- always-on VDP register/CRAM write ring (raster-effect probe) ----
 * Every VDP register or CRAM write is recorded with the scanline it occurred
 * on (PRINCIPLES #17: query the ring, never arm-then-probe). At the dump frame
 * we replay the ring for the just-finished frame to see whether scroll (r8/r9)
 * or palette (CRAM) change DURING active display (lines 0..191) - the signature
 * of a per-scanline raster effect a full-frame-snapshot renderer cannot show.
 * VRAM writes are deliberately not recorded (too high-volume; not needed). */
#define SMS_VDPW_RING 8192
static struct { uint32_t frame; int16_t line; uint8_t kind; uint16_t addr; uint8_t val; }
                 g_vdpw[SMS_VDPW_RING];
static uint32_t  g_vdpw_pos;
static void vdp_write_obs(int kind, uint16_t addr, uint8_t value){
    if (kind == VDPW_VRAM){
        /* env-armed VRAM write watch: SMS_VRAM_WATCH=<hexaddr> [SMS_VRAM_WATCH_FRAME=N].
         * Logs the writing PC + recent function-entry chain so a single divergent
         * VRAM byte can be attributed to the function that produced it. */
        static int armed=-1; static uint16_t waddr; static long wframe;
        if (armed<0){
            const char *e=getenv("SMS_VRAM_WATCH");
            armed = e ? 1 : 0;
            if (armed){ waddr=(uint16_t)strtoul(e,NULL,16);
                const char *f=getenv("SMS_VRAM_WATCH_FRAME"); wframe=f?strtol(f,NULL,10):-1; }
        }
        if (armed && addr==waddr && (wframe<0 || (long)g_frame==wframe)){
#ifdef SMS_TRACE_PC
            fprintf(stderr,"[vramwatch] frame=%llu addr=%04X val=%02X pc=%04X chain:",
                    (unsigned long long)g_frame, addr, value, g_dbg_pc);
#else
            fprintf(stderr,"[vramwatch] frame=%llu addr=%04X val=%02X chain:",
                    (unsigned long long)g_frame, addr, value);
#endif
            for (int j=1;j<=8 && g_enter_pos>=(uint32_t)j;j++)
                fprintf(stderr," %04X", g_enter_ring[(g_enter_pos-j)&(SMS_ENTER_RING_SIZE-1)]);
            fprintf(stderr,"\n");
        }
        return;   /* VRAM not recorded in the (reg/CRAM) ring */
    }
    uint32_t i = g_vdpw_pos++ & (SMS_VDPW_RING - 1);
    g_vdpw[i].frame = (uint32_t)g_frame;
    g_vdpw[i].line  = (int16_t)g_vdp.line;
    g_vdpw[i].kind  = (uint8_t)kind;
    g_vdpw[i].addr  = addr;
    g_vdpw[i].val   = value;
}

/* ---- dispatch misses ---- */
static uint8_t  *g_miss_seen;        /* 64K dedup bitmap */
static int       g_miss_count;
static const char *g_miss_path = "dispatch_misses.log";

/* ---- always-on function-entry ring ---- */
uint16_t g_enter_ring[SMS_ENTER_RING_SIZE];
uint32_t g_enter_pos;
uint16_t g_dbg_pc;             /* address of the instruction now executing; pushed by take_irq */
#ifdef SMS_TRACE_PC
uint16_t g_dbg_irq_from;   /* main-loop PC interrupted by the last IRQ */
#endif

/* ============================ bus ============================ */
static inline uint8_t rom_byte(int bank, uint32_t off){
    size_t idx = (size_t)(unsigned)bank * 0x4000u + off;
    if (g_rom_size) idx %= g_rom_size;
    return g_rom ? g_rom[idx] : 0xFF;
}

uint8_t sms_read8(uint16_t a){
    if (a < 0x0400) return rom_byte(0, a);              /* fixed first 1 KB */
    if (a < 0x4000) return rom_byte(g_bank[0], a);
    if (a < 0x8000) return rom_byte(g_bank[1], (uint32_t)a - 0x4000);
    if (a < 0xC000) return rom_byte(g_bank[2], (uint32_t)a - 0x8000);
    return g_ram[(a - 0xC000) & 0x1FFF];
}

void sms_write8(uint16_t a, uint8_t v){
#ifdef SMS_TRACE_PC
    {   /* env-armed RAM write watch: SMS_RAM_WATCH=<hexaddr> [SMS_RAM_WATCH_FRAME=N] */
        static int armed=-1; static uint16_t waddr; static long wframe; static int wall;
        if (armed<0){ const char *e=getenv("SMS_RAM_WATCH"); armed=e?1:0;
            wall = (e && (e[0]=='A'||e[0]=='a'));   /* SMS_RAM_WATCH=ALL -> log every write */
            if (armed){ waddr=wall?0:(uint16_t)strtoul(e,NULL,16);
                const char *f=getenv("SMS_RAM_WATCH_FRAME"); wframe=f?strtol(f,NULL,10):-1; } }
        if (armed && wall && (wframe<0 || (long)g_frame==wframe)){
            fprintf(stderr,"W %llu %04X %02X\n", (unsigned long long)g_frame, a, v);
        } else
        if (armed && !wall && a==waddr && (wframe<0 || (long)g_frame==wframe)){
            fprintf(stderr,"[ramwatch] frame=%llu addr=%04X val=%02X pc=%04X chain:",
                    (unsigned long long)g_frame, a, v, g_dbg_pc);
            for (int j=1;j<=8 && g_enter_pos>=(uint32_t)j;j++)
                fprintf(stderr," %04X", g_enter_ring[(g_enter_pos-j)&(SMS_ENTER_RING_SIZE-1)]);
            fprintf(stderr,"\n");
        }
    }
#endif
    if (a >= 0xC000){
        g_ram[(a - 0xC000) & 0x1FFF] = v;
        if (a >= 0xFFFC){                                /* Sega frame regs */
            switch (a){
                case 0xFFFC: break;                      /* RAM/mapper control */
                case 0xFFFD: g_bank[0] = v; break;
                case 0xFFFE: g_bank[1] = v; break;
                case 0xFFFF: g_bank[2] = v; break;
            }
        }
        return;
    }
    if (g_mapper_cm){                                    /* Codemasters frame regs */
        if      (a == 0x0000) g_bank[0] = v;
        else if (a == 0x4000) g_bank[1] = v;
        else if (a == 0x8000) g_bank[2] = v;
    }
    /* Sega mapper: ROM region is read-only. */
}

int sms_slot_bank(uint16_t addr){
    return (addr < 0xC000) ? g_bank[addr >> 14] : -1;
}

uint8_t sms_io_in(uint8_t p){
    if (p < 0x40){                                       /* control / GG system ports */
        if (g_is_gg && p == 0x00)                        /* GG START on bit7 (active low) */
            return (uint8_t)(0xFF ^ ((g_pad1 & SMS_PAD_START) ? 0x80 : 0x00));
        return 0xFF;
    }
    if (p < 0x80) return (p & 1) ? vdp_hcounter() : vdp_vcounter();
    if (p < 0xC0) return (p & 1) ? vdp_status_read() : vdp_data_read();
    /* $C0-$FF controller ports, active low (0 = pressed). Even ($DC) = P1 D-pad
     * + buttons + P2 up/down; odd ($DD) = P2 left/right/buttons + reset/TH
     * (idle high). */
    if (p & 1){
        uint8_t b = 0xFF;                                /* $DD */
        if (g_pad2 & SMS_PAD_LEFT)  b &= (uint8_t)~0x01;
        if (g_pad2 & SMS_PAD_RIGHT) b &= (uint8_t)~0x02;
        if (g_pad2 & SMS_PAD_B1)    b &= (uint8_t)~0x04;
        if (g_pad2 & SMS_PAD_B2)    b &= (uint8_t)~0x08;
        return b;                                        /* bit4 reset, bit6/7 TH: not pressed */
    } else {
        uint8_t b = 0xFF;                                /* $DC */
        if (g_pad1 & SMS_PAD_UP)    b &= (uint8_t)~0x01;
        if (g_pad1 & SMS_PAD_DOWN)  b &= (uint8_t)~0x02;
        if (g_pad1 & SMS_PAD_LEFT)  b &= (uint8_t)~0x04;
        if (g_pad1 & SMS_PAD_RIGHT) b &= (uint8_t)~0x08;
        if (g_pad1 & SMS_PAD_B1)    b &= (uint8_t)~0x10;
        if (g_pad1 & SMS_PAD_B2)    b &= (uint8_t)~0x20;
        if (g_pad2 & SMS_PAD_UP)    b &= (uint8_t)~0x40;
        if (g_pad2 & SMS_PAD_DOWN)  b &= (uint8_t)~0x80;
        return b;
    }
}

void sms_io_out(uint8_t p, uint8_t v){
    if (p < 0x40){                                       /* memory / GG system / IO control */
        if (g_is_gg && p == 0x06) psg_write_stereo(v);   /* GG stereo routing register */
        return;                                          /* $3E mem-control etc.: not modelled */
    }
    if (p < 0x80){ psg_write(v); return; }               /* PSG data ($40-$7F, canonically $7F) */
    if (p < 0xC0){ if (p & 1) vdp_control_write(v); else vdp_data_write(v); return; }
    (void)v;                                             /* $C0-$FF controllers: no output */
}

/* ====================== timing + interrupts ====================== */
static void frame_completed(void){
    g_frame++;
    { static int armed=-1; if(armed<0){armed=getenv("SMS_IC_TRACE")?1:0;}
      if(armed) fprintf(stderr,"IC %llu %llu\n",(unsigned long long)g_frame,(unsigned long long)g_frame_ic); g_frame_ic=0; }
#ifdef SMS_HAVE_JIT
    /* live Tier-2 coverage: shards published (monotonic up = coverage growing) and
     * the interp fraction over the last window (drops as shards take over). */
    { static uint64_t lf, lh, lt;
      if (g_frame - lf >= 120){
          uint64_t dh = g_hybrid_cyc - lh, dt = g_z80.cyc - lt;
          fprintf(stderr, "[jit-cov] frame=%llu shards=%llu req=%llu declined=%llu | "
                  "interp last~2s=%.1f%% total=%.1f%%\n",
                  (unsigned long long)g_frame, (unsigned long long)sms_jit_published(),
                  (unsigned long long)sms_jit_requested(), (unsigned long long)sms_jit_declined(),
                  dt ? 100.0*(double)dh/(double)dt : 0.0,
                  g_z80.cyc ? 100.0*(double)g_hybrid_cyc/(double)g_z80.cyc : 0.0);
          fflush(stderr);
          lf=g_frame; lh=g_hybrid_cyc; lt=g_z80.cyc;
      } }
#endif
    if (g_input_cb) g_pad1 = g_input_cb(g_frame);   /* scripted input for the upcoming frame */
    if (g_audio_sink){
        /* Drain this frame's PSG output to the sink (interleaved stereo). The
         * PSG was advanced up to the current absolute cycle in advance_vdp(). */
        int16_t buf[2048 * 2];
        size_t n;
        while ((n = psg_render(buf, 2048)) > 0) g_audio_sink(buf, n);
    }
    if (g_vdp_trace){
        /* End-of-frame VDP state == what the renderer would consume for this
         * frame. Hash each region separately so the diff says WHICH diverged. */
        fprintf(g_vdp_trace, "%llu,%016llx,%016llx,%016llx,%016llx,%02X,%02X,%02X,%02X,%04X\n",
                (unsigned long long)g_frame,
                (unsigned long long)fnv1a64(g_vdp.vram, sizeof g_vdp.vram),
                (unsigned long long)fnv1a64(g_vdp.cram, sizeof g_vdp.cram),
                (unsigned long long)fnv1a64(g_vdp.reg,  sizeof g_vdp.reg),
                (unsigned long long)fnv1a64(g_ram, sizeof g_ram),
                g_vdp.reg[8], g_vdp.reg[9], g_vdp.reg[0], g_vdp.reg[1], g_z80.sp);
        fflush(g_vdp_trace);
    }
    {   /* env-armed: dump the function-entry ring at a target frame to expose a
         * spin loop (which functions are repeating when the game is stuck). */
        static long rf=-2;
        if (rf==-2){ const char *e=getenv("SMS_RING_DUMP_FRAME"); rf=e?strtol(e,NULL,10):-1; }
        if (rf>=0 && (long)g_frame==rf){
            fprintf(stderr,"[ring] frame %ld last 40 entries (newest first):", rf);
            for (int j=1;j<=40 && g_enter_pos>=(uint32_t)j;j++)
                fprintf(stderr," %04X", g_enter_ring[(g_enter_pos-j)&(SMS_ENTER_RING_SIZE-1)]);
            fprintf(stderr,"\n");
        }
    }
    if (g_dump_path && g_frame == g_dump_frame){
        int nz_vram=0, nz_cram=0;
        for (int i=0;i<0x4000;i++) if (g_vdp.vram[i]) nz_vram++;
        for (int i=0;i<0x40;i++)   if (g_vdp.cram[i]) nz_cram++;
        {   /* raw VRAM/RAM dumps (<png>.vram/.ram) for recomp-vs-interp byte diffs */
            char vp[512]; snprintf(vp,sizeof vp,"%s.vram",g_dump_path);
            FILE *vf=fopen(vp,"wb"); if (vf){ fwrite(g_vdp.vram,1,0x4000,vf); fclose(vf); }
            snprintf(vp,sizeof vp,"%s.ram",g_dump_path);
            FILE *rf=fopen(vp,"wb"); if (rf){ fwrite(g_ram,1,sizeof g_ram,rf); fclose(rf); }
        }
        fprintf(stderr,
            "[vdp] dump@%llu disp=%d r0=%02X r1=%02X r2=%02X r5=%02X r6=%02X "
            "r7=%02X r8=%02X r9=%02X r10=%02X | vram_nz=%d cram_nz=%d line=%d\n",
            (unsigned long long)g_frame, (g_vdp.reg[1]>>6)&1,
            g_vdp.reg[0],g_vdp.reg[1],g_vdp.reg[2],g_vdp.reg[5],g_vdp.reg[6],
            g_vdp.reg[7],g_vdp.reg[8],g_vdp.reg[9],g_vdp.reg[10],
            nz_vram, nz_cram, g_vdp.line);
        {   /* one-shot renderer diagnostic (not a hot path): the BG nametable
             * tile-index grid + the y->row mapping the renderer computes, so we
             * can see exactly what rows the "garbage band" reads. */
            int nt_base = (g_vdp.reg[2] & 0x0E) << 10;
            int vs = g_vdp.reg[9];
            fprintf(stderr, "[nt] base=%04X vscroll=%d  y->row map (192-line, %%224):\n", nt_base, vs);
            for (int y=0; y<SMS_SCREEN_H; y+=8){
                int by = (y + vs) % 224;
                fprintf(stderr, "  y=%3d by=%3d row=%2d\n", y, by, by>>3);
            }
            fprintf(stderr, "[nt] horizon rows 0..9 FULL entries (T=tile P=pal bit11 R=prio bit12):\n");
            for (int row=0; row<10; row++){
                fprintf(stderr, "  r%02d:", row);
                for (int col=0; col<16; col++){
                    int ea = (nt_base + (row*32 + col)*2) & 0x3FFF;
                    int entry = g_vdp.vram[ea] | (g_vdp.vram[ea+1] << 8);
                    fprintf(stderr, " %03X%c%c", entry & 0x1FF,
                            (entry & 0x800)?'P':'.', (entry & 0x1000)?'R':'.');
                }
                fprintf(stderr, "\n");
            }
            /* Decode a few suspect horizon tiles as 8x8 palette-index art so we
             * can see whether the pattern is a coherent shape and which colour
             * indices it uses (high indices -> upper half of the sub-palette). */
            fprintf(stderr, "[cram] 32 entries (idx: raw -> R,G,B 0..3):\n");
            for (int e=0;e<32;e++){
                uint8_t c = g_vdp.cram[e & 0x1F];
                fprintf(stderr, "  %2d: %02X -> %d,%d,%d%s", e, c,
                        c&3,(c>>2)&3,(c>>4)&3,
                        (e==15)?"\n":(e%4==3?"\n":"   "));
            }
            int suspects[] = {0x0FC,0x0FD,0x0FE,0x0FF,0x07B,0x08C};
            for (unsigned s=0; s<sizeof suspects/sizeof suspects[0]; s++){
                int t = suspects[s];
                fprintf(stderr, "[tile %03X] pattern (palette index 0..F per pixel):\n", t);
                for (int py=0; py<8; py++){
                    const uint8_t *p = &g_vdp.vram[((t*32)+py*4) & 0x3FFF];
                    fprintf(stderr, "   ");
                    for (int px=0; px<8; px++){
                        int bit=7-px;
                        int c=((p[0]>>bit)&1)|(((p[1]>>bit)&1)<<1)|(((p[2]>>bit)&1)<<2)|(((p[3]>>bit)&1)<<3);
                        fprintf(stderr, "%X", c);
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
        {   /* Replay the VDP-write ring for the just-finished frame: do scroll
             * (r8/r9) or palette (CRAM) change DURING active display (lines
             * 0..191)? That is the signature of a raster effect this snapshot
             * renderer cannot reproduce. Writes are tagged with g_frame as it
             * was DURING the frame, which is g_frame-1 now (incremented above). */
            uint32_t target = (uint32_t)(g_frame - 1);
            int a_r8=0, a_r9=0, a_cram=0, a_other=0, vbl=0, total=0, printed=0;
            uint32_t span = g_vdpw_pos < SMS_VDPW_RING ? g_vdpw_pos : SMS_VDPW_RING;
            fprintf(stderr, "[vdpw] active-display reg/CRAM writes in frame %u "
                    "(line 0..191):\n", target);
            for (uint32_t i=0;i<span;i++){
                uint32_t idx = (g_vdpw_pos - span + i) & (SMS_VDPW_RING - 1);
                if (g_vdpw[idx].frame != target) continue;
                total++;
                int ln = g_vdpw[idx].line;
                int active = (ln >= 0 && ln <= 191);
                if (!active){ vbl++; continue; }
                if (g_vdpw[idx].kind == VDPW_REG){
                    int rg = g_vdpw[idx].addr;
                    if (rg==8) a_r8++; else if (rg==9) a_r9++; else a_other++;
                } else a_cram++;
                if (printed < 200){
                    printed++;
                    fprintf(stderr, "   line %3d %s %02X = %02X\n", ln,
                            g_vdpw[idx].kind==VDPW_REG?"REG ":"CRAM",
                            g_vdpw[idx].addr, g_vdpw[idx].val);
                }
            }
            fprintf(stderr, "[vdpw] frame %u: total reg/cram writes=%d (vblank=%d) | "
                    "ACTIVE r8=%d r9=%d cram=%d otherReg=%d\n",
                    target, total, vbl, a_r8, a_r9, a_cram, a_other);
        }
#ifdef SMS_TRACE_PC
        fprintf(stderr, "[vdp] lastPC=%04X  IRQ-interrupted main PC=%04X  "
                "IY=%04X IX=%04X SP=%04X iff1=%d\n",
                g_dbg_pc, g_dbg_irq_from, g_z80.iy, g_z80.ix, g_z80.sp, g_z80.iff1);
#endif
        {   /* chronological ring (oldest -> newest) */
            uint32_t span = g_enter_pos < SMS_ENTER_RING_SIZE ? g_enter_pos : SMS_ENTER_RING_SIZE;
            fprintf(stderr, "[vdp] entry order (%u):", span);
            for (uint32_t i=0;i<span;i++)
                fprintf(stderr, " %04X",
                    g_enter_ring[(g_enter_pos-span+i) & (SMS_ENTER_RING_SIZE-1)]);
            fprintf(stderr, "\n");
        }
        vdp_render_frame(g_fb);
        if (png_write_argb(g_dump_path, g_fb, SMS_SCREEN_W, SMS_SCREEN_H, SMS_SCREEN_W) == 0)
            fprintf(stderr, "[runner] dumped frame %llu -> %s\n",
                    (unsigned long long)g_frame, g_dump_path);
    }
    if (g_frame_cb){                     /* live host: render + present every frame */
        vdp_render_frame(g_fb);
        if (g_frame_cb(g_fb, SMS_SCREEN_W, SMS_SCREEN_H) && g_running)
            longjmp(g_quit_env, 1);      /* user closed the window */
    }
    if (g_frame_limit && g_frame >= g_frame_limit && g_running)
        longjmp(g_quit_env, 1);
}

static void take_irq(void){
#ifdef SMS_TRACE_PC
    g_dbg_irq_from = g_dbg_pc;
#endif
    g_irq_taken++;
    if (g_sync_depth > 1) g_irq_reentrant++;
    g_z80.cyc += 13;                     /* IM1 interrupt acceptance latency (Z80: 13 T-states,
                                          * matches superzazu's z80_gen_int). Without this the
                                          * recompiled path runs 13 cycles/frame ahead of the
                                          * interpreter, drifting by a whole main-loop iteration
                                          * over hundreds of frames. */
    g_z80.iff1 = g_z80.iff2 = false;     /* Z80 disables interrupts on accept */
    g_z80.halted = false;
    /* Push the REAL interrupted PC (g_dbg_pc = address of the instruction this IRQ
     * preempted), exactly as hardware does. Control still unwinds via the C call
     * stack (the handler's RET pops these two bytes and returns here), but the
     * pushed VALUE is now correct for handlers that READ the stacked return
     * address - which Sonic Blast's title path does. A 0x0000 placeholder sent it
     * down the wrong branch and the title never uploaded. */
    g_z80.sp = (uint16_t)(g_z80.sp - 2);
    sms_write16(g_z80.sp, g_dbg_pc);
    {   /* env-gated IRQ-event trace: SMS_IRQ_TRACE=1 [SMS_IRQ_LO/HI=frame] */
        static int armed=-1; static long lo,hi;
        if (armed<0){ const char *e=getenv("SMS_IRQ_TRACE"); armed=e?1:0;
            const char *l=getenv("SMS_IRQ_LO"), *h=getenv("SMS_IRQ_HI");
            lo=l?strtol(l,0,10):-1; hi=h?strtol(h,0,10):(1L<<30); }
        if (armed && (long)g_frame>=lo && (long)g_frame<=hi){
            int fi=g_vdp.frame_irq, li=g_vdp.line_irq;
            uint16_t from=g_dbg_pc, sp0=g_z80.sp;
            call_by_address(0x0038);
            fprintf(stderr,"[irq] f=%llu from=%04X src=%s iff1_after=%d sp:%04X->%04X line=%d\n",
                (unsigned long long)g_frame, from, fi?"VBL":(li?"LINE":"?"),
                g_z80.iff1, sp0, g_z80.sp, g_vdp.line);
            return;
        }
    }
    call_by_address(0x0038);             /* IM1 vector; RET unwinds back here */
}

/* Advance the VDP up to absolute cycle `cyc`, stepping scanlines and firing
 * frame_completed() at each frame boundary. Shared by sms_sync (recompiled
 * path) and the hybrid interpreter so both drive video timing identically. */
extern int g_diff_freeze;
static void advance_vdp(uint64_t cyc){
    if (g_diff_freeze) return;     /* differential harness: VDP frozen during a test run */
    /* Keep the PSG in lockstep with the VDP on the same absolute timeline.
     * Only synthesise when a sink is listening (oracle/diff runs pay nothing).
     * Done before stepping lines so the just-finished frame's samples are
     * available when frame_completed() drains. */
    if (g_audio_sink && cyc > g_psg_cyc){
        psg_advance((uint32_t)(cyc - g_psg_cyc));
        g_psg_cyc = cyc;
    }
    while (cyc >= g_next_line_cyc){
        vdp_step_line();
        g_next_line_cyc += SMS_CYC_PER_LINE;
        if (g_vdp.line == 0) frame_completed();
    }
}

/* Set the CPU's next sync deadline. Normally the next scanline event, BUT while
 * the VDP /INT line is asserted (frame/line IRQ pending and not yet cleared) we
 * deadline at the very next instruction so the recompiled path SAMPLES the IRQ
 * at every instruction boundary — exactly like the interpreter/hybrid do — until
 * it is accepted (iff1) or the source is cleared (status read). This is the one
 * fix that unifies the interrupt-sampling contract across all three CPU modes;
 * the hot path stays a single compare while no IRQ is pending. */
void sms_set_sync_deadline(void){
    g_sync_deadline = vdp_irq_asserted() ? g_z80.cyc + 1 : g_next_line_cyc;
}

void sms_sync(void){
    if (++g_sync_depth > g_sync_maxdepth) g_sync_maxdepth = g_sync_depth;
    advance_vdp(g_z80.cyc);
    if (vdp_irq_asserted() && g_z80.iff1 && !g_z80.ei_block)
        take_irq();
    sms_set_sync_deadline();
    g_sync_depth--;
}

void sms_halt(void){
    g_z80.halted = true;
    long guard = 0;
    while (g_z80.halted){
        g_z80.cyc = (g_sync_deadline > g_z80.cyc) ? g_sync_deadline
                                                  : g_z80.cyc + SMS_CYC_PER_LINE;
        sms_sync();                      /* may take_irq -> clears halted */
        if (++guard > 4 * SMS_LINES_PER_FRAME * 1000){  /* ~1000 frames: bail */
            g_z80.halted = false;
            break;
        }
    }
}

/* ====================== hybrid interpreter fallback ======================
 *
 * call_by_address() resolves computed targets to recompiled functions. The
 * static finder cannot resolve every computed jump (multi-bank script tables
 * like $2AF6, RAM-driven dispatch, etc.), so those targets land in the
 * dispatch switch's `default:` and reach sms_dispatch_miss(). Rather than
 * no-op the call (which leaks the Z80 stack and skips real work), we interpret
 * the routine with the vendored superzazu Z80 over the SAME bus/IO hooks - so
 * it sees the live g_bank mapping - until the routine returns to its caller,
 * then sync back and let the recompiled caller continue.
 *
 * Stop condition: a recompiled function call C-returns exactly when its Z80
 * RET pops the entry frame. We reproduce that: interpret until the stack
 * pointer rises above the value it had on entry (sp > entry_sp). Internal
 * CALLs and the `push ret; jp (hl)` computed-call idiom keep sp <= entry_sp
 * until the routine's own final RET, so they don't trip an early stop (see the
 * computed-call-via-jp-hl note). This holds for both a computed CALL (caller
 * pushed the continuation: entry frame == that continuation) and a tail jump-
 * table dispatch (entry frame == the parent's return) - in both cases the
 * routine's RET unwinds exactly one C level, matching call_by_address. */
static z80  g_hz;
static bool g_hz_init;

static uint8_t hyb_read (void *u, uint16_t a){ (void)u; return sms_read8(a); }
static void    hyb_write(void *u, uint16_t a, uint8_t v){ (void)u; sms_write8(a, v); }
static uint8_t hyb_in   (z80 *z, uint8_t p){ (void)z; return sms_io_in(p); }
static void    hyb_out  (z80 *z, uint8_t p, uint8_t v){ (void)z; sms_io_out(p, v); }

static inline uint8_t pack_f(const z80 *z){
    return (uint8_t)((z->sf<<7)|(z->zf<<6)|(z->yf<<5)|(z->hf<<4)|
                     (z->xf<<3)|(z->pf<<2)|(z->nf<<1)|(z->cf));
}
static inline void unpack_f(z80 *z, uint8_t f){
    z->sf=(f>>7)&1; z->zf=(f>>6)&1; z->yf=(f>>5)&1; z->hf=(f>>4)&1;
    z->xf=(f>>3)&1; z->pf=(f>>2)&1; z->nf=(f>>1)&1; z->cf=f&1;
}

/* g_z80 (the recompiled register file) -> g_hz (superzazu). cyc is rebased to
 * 0 by the caller to keep superzazu's 32-bit `cyc` from overflowing on long
 * runs; absolute time stays in g_z80.cyc. */
static void state_to_hz(void){
    g_hz.a=g_z80.a; g_hz.b=g_z80.b; g_hz.c=g_z80.c; g_hz.d=g_z80.d;
    g_hz.e=g_z80.e; g_hz.h=g_z80.h; g_hz.l=g_z80.l;
    unpack_f(&g_hz, g_z80.f);
    g_hz.a_=g_z80.a_; g_hz.b_=g_z80.b_; g_hz.c_=g_z80.c_; g_hz.d_=g_z80.d_;
    g_hz.e_=g_z80.e_; g_hz.h_=g_z80.h_; g_hz.l_=g_z80.l_; g_hz.f_=g_z80.f_;
    g_hz.ix=g_z80.ix; g_hz.iy=g_z80.iy; g_hz.sp=g_z80.sp;
    g_hz.mem_ptr=g_z80.wz; g_hz.i=g_z80.i; g_hz.r=g_z80.r;
    g_hz.iff1=g_z80.iff1; g_hz.iff2=g_z80.iff2;
    g_hz.interrupt_mode=g_z80.im; g_hz.halted=g_z80.halted;
    g_hz.int_pending=0; g_hz.nmi_pending=0;
    g_hz.iff_delay = g_z80.ei_block;     /* carry the EI-delay slot into the interpreter */
}
/* g_hz (superzazu) -> g_z80. cyc is reapplied by the caller (rebased). */
static void state_from_hz(void){
    g_z80.a=g_hz.a; g_z80.b=g_hz.b; g_z80.c=g_hz.c; g_z80.d=g_hz.d;
    g_z80.e=g_hz.e; g_z80.h=g_hz.h; g_z80.l=g_hz.l;
    g_z80.f=pack_f(&g_hz);
    g_z80.a_=g_hz.a_; g_z80.b_=g_hz.b_; g_z80.c_=g_hz.c_; g_z80.d_=g_hz.d_;
    g_z80.e_=g_hz.e_; g_z80.h_=g_hz.h_; g_z80.l_=g_hz.l_; g_z80.f_=g_hz.f_;
    g_z80.ix=g_hz.ix; g_z80.iy=g_hz.iy; g_z80.sp=g_hz.sp;
    g_z80.wz=g_hz.mem_ptr; g_z80.i=g_hz.i; g_z80.r=g_hz.r;
    g_z80.iff1=g_hz.iff1; g_z80.iff2=g_hz.iff2;
    g_z80.im=g_hz.interrupt_mode; g_z80.halted=g_hz.halted;
    g_z80.ei_block = g_hz.iff_delay ? 1 : 0;
}

/* Classify the instruction at `pc` for hybrid call/ret-depth tracking:
 * +1 = pushes a return (CALL / CALL cc / RST), -1 = pops a return (RET / RET cc /
 * RETI / RETN), 0 = neither. PUSH/POP move SP but are NOT control transfers, so
 * they classify as 0 - which is the whole point: a routine entered via a tail
 * JP(HL) (e.g. an IRQ-handler continuation like Sonic Blast's line-raster $1AAF)
 * pops the caller's saved registers before its own RET. A raw `sp > entry_sp`
 * stop test trips on that first POP and bails mid-routine (leaving iff1=0);
 * depth-tracking exits only on the routine's actual returning RET. */
static int hyb_cf_class(uint16_t pc){
    uint8_t b = sms_read8(pc);
    if (b == 0xED) return ((sms_read8((uint16_t)(pc+1)) & 0xC7) == 0x45) ? -1 : 0; /* RETI/RETN */
    if (b == 0xC9)          return -1;   /* RET        */
    if ((b & 0xC7) == 0xC0) return -1;   /* RET cc     */
    if (b == 0xCD)          return +1;   /* CALL nn    */
    if ((b & 0xC7) == 0xC4) return +1;   /* CALL cc,nn */
    if ((b & 0xC7) == 0xC7) return +1;   /* RST        */
    return 0;
}

static void hybrid_interpret(uint16_t addr){
    if (!g_hz_init){
        z80_init(&g_hz);
        g_hz.read_byte=hyb_read; g_hz.write_byte=hyb_write;
        g_hz.port_in=hyb_in;     g_hz.port_out=hyb_out;
        g_hz_init=true;
    }
    state_to_hz();
    g_hz.pc = addr;
    const uint64_t base = g_z80.cyc;     /* absolute time at entry */
    g_hz.cyc = 0;                        /* rebase: g_hz.cyc counts this call only */
    const uint16_t entry_sp = g_hz.sp;
    sms_enter(addr);                     /* record the hybrid target in the ring */

    /* ~16M-instruction backstop: the routine should return long before this;
     * tripping it means a real stall, not normal work. */
    const long guard_max = 16L*1000L*1000L;
    long guard = 0;
    /* Stop when the routine RETs out of its entry frame. We track explicit
     * CALL/RST/interrupt depth and only test at a taken RET:
     *   depth>0                         -> internal return, depth--
     *   depth==0 && sp>entry_sp         -> returned to the entry caller: STOP
     *   depth==0 && sp<=entry_sp        -> the `push <cont>; jp(hl)` computed-call
     *                                      idiom returning to its continuation
     *                                      (the push isn't a CALL opcode, so depth
     *                                      stayed 0) -> keep going
     * Testing only at a RET (not on every sp>entry_sp) is what lets a tail-jumped
     * IRQ-handler continuation (Sonic Blast's line-raster $1AAF) POP the caller's
     * saved registers - lifting sp above entry_sp - without a premature stop; its
     * own final RET is the real exit. The old sp-only test bailed at that POP and
     * left iff1=0, freezing the title sequence. */
    int depth = 0;
    for (;;){
        bool took_int = false;
        if (!g_diff_freeze && vdp_irq_asserted() && g_hz.iff1 && !g_hz.int_pending){
            z80_gen_int(&g_hz, 0xFF);    /* IM1: vector 0x0038 (data ignored) */
            took_int = true;             /* the next step accepts it, pushing a return */
        }
        int cls = took_int ? 0 : hyb_cf_class(g_hz.pc);
        uint16_t sp0 = g_hz.sp;
        z80_step(&g_hz); g_frame_ic++;
        advance_vdp(base + g_hz.cyc);
        g_sync_deadline = g_next_line_cyc;
        int dsp = (int16_t)(g_hz.sp - sp0);
        if (took_int){ if (dsp == -2) depth++; }          /* interrupt pushed PC      */
        else if (cls > 0 && dsp == -2) depth++;           /* taken CALL/RST           */
        else if (cls < 0 && dsp ==  2){                   /* taken RET/RETI/RETN      */
            if (depth > 0) depth--;
            else if (g_hz.sp > entry_sp) break;           /* returned to entry caller */
            /* else: push;jp(hl) continuation return -> keep interpreting */
        }
        if (++guard >= guard_max){
            fprintf(stderr, "[hybrid] guard tripped: addr=%04X pc=%04X sp=%04X "
                    "(entry_sp=%04X depth=%d)\n", addr, g_hz.pc, g_hz.sp, entry_sp, depth);
            break;
        }
    }
    state_from_hz();
    g_z80.cyc = base + g_hz.cyc;         /* reapply rebased absolute time */
    /* If the routine left an IRQ asserted (e.g. VBlank latched while iff1 was
     * clear), deadline at the next instruction so native accepts it the instant
     * it becomes eligible — not many instructions later at the next line. This
     * is the hybrid->native handoff half of the interrupt-sampling fix. */
    sms_set_sync_deadline();
    g_hybrid_cyc += g_hz.cyc;            /* cycles this routine spent in the interpreter */
    g_hybrid_calls++;
}

/* ============ differential function harness (invariant, timing-free) ============ *
 * Invariant: a recompiled function F started from state S must produce the same
 * register+RAM exit deltas as superzazu running F from S. We run BOTH from one
 * captured snapshot with IRQ/VDP FROZEN (so neither side is interrupted — pure
 * computation, no timeline), then diff. A nonzero delta is a pure translation
 * bug, independent of any emulator timeline. Env: SMS_DIFF_ADDR=<hex> picks the
 * function to test; SMS_DIFF_LO/HI bound the frame window. */
int  g_diff_freeze = 0;
int  g_diff_active = 0;
static int      g_in_diff = 0;
static uint16_t g_diff_targets[256]; static int g_diff_ntargets=0;
static long     g_diff_lo=-1, g_diff_hi=-1, g_diff_max=400;
static int diff_is_target(uint16_t a){
    if (g_diff_ntargets==0) return 0;
    for (int i=0;i<g_diff_ntargets;i++) if (g_diff_targets[i]==a) return 1;
    return 0;
}

uint64_t g_diff_icount = 0;        /* instructions in the current controlled run */
static jmp_buf g_diff_jmp;
void sms_diff_abort(void){ longjmp(g_diff_jmp, 1); }   /* budget exceeded -> bail the test */

/* ---- instruction-level trace (pinpoint the divergent instruction) ---- */
int g_diff_trace = 0;
#define DTRACE_MAX 300000
typedef struct { uint16_t pc, af, bc, de, hl, ix, iy, sp; } DTrace;
static DTrace g_rtrace[DTRACE_MAX]; static int g_rtrace_n;
static DTrace g_strace[DTRACE_MAX]; static int g_strace_n;
#ifdef SMS_TRACE_PC
void sms_diff_logpc(void){      /* called from SMS_PC during the controlled recomp run */
    if (g_rtrace_n>=DTRACE_MAX) return;
    DTrace *t=&g_rtrace[g_rtrace_n++];
    t->pc=g_dbg_pc; t->af=(uint16_t)((g_z80.a<<8)|g_z80.f);
    t->bc=(uint16_t)((g_z80.b<<8)|g_z80.c); t->de=(uint16_t)((g_z80.d<<8)|g_z80.e);
    t->hl=(uint16_t)((g_z80.h<<8)|g_z80.l); t->ix=g_z80.ix; t->iy=g_z80.iy; t->sp=g_z80.sp;
}
#endif

typedef struct { Z80State z; uint8_t ram[0x2000]; int bank[3]; SmsVdp vdp;
                 uint64_t next_line, deadline, psg; } DiffSnap;
static void diff_save(DiffSnap *s){
    s->z=g_z80; memcpy(s->ram,g_ram,sizeof g_ram);
    s->bank[0]=g_bank[0]; s->bank[1]=g_bank[1]; s->bank[2]=g_bank[2];
    s->vdp=g_vdp; s->next_line=g_next_line_cyc; s->deadline=g_sync_deadline; s->psg=g_psg_cyc;
}
static void diff_restore(const DiffSnap *s){
    g_z80=s->z; memcpy(g_ram,s->ram,sizeof g_ram);
    g_bank[0]=s->bank[0]; g_bank[1]=s->bank[1]; g_bank[2]=s->bank[2];
    g_vdp=s->vdp; g_next_line_cyc=s->next_line; g_sync_deadline=s->deadline; g_psg_cyc=s->psg;
}
/* run F purely in superzazu from the current g_z80, until it RETs (sp rises above
 * entry); IRQ/VDP are frozen by g_diff_freeze. */
static void diff_run_super(uint16_t addr){
    if (!g_hz_init){ z80_init(&g_hz); g_hz.read_byte=hyb_read; g_hz.write_byte=hyb_write;
                     g_hz.port_in=hyb_in; g_hz.port_out=hyb_out; g_hz_init=true; }
    state_to_hz(); g_hz.pc=addr;
    uint64_t base=g_z80.cyc; g_hz.cyc=0; uint16_t entry_sp=g_hz.sp;
    long guard=0;
    for(;;){
        if (g_diff_trace && g_strace_n<DTRACE_MAX){ DTrace *t=&g_strace[g_strace_n++];
            t->pc=g_hz.pc; t->af=(uint16_t)((g_hz.a<<8)|pack_f(&g_hz));
            t->bc=(uint16_t)((g_hz.b<<8)|g_hz.c); t->de=(uint16_t)((g_hz.d<<8)|g_hz.e);
            t->hl=(uint16_t)((g_hz.h<<8)|g_hz.l); t->ix=g_hz.ix; t->iy=g_hz.iy; t->sp=g_hz.sp; }
        uint8_t op0=sms_read8(g_hz.pc);          /* is this a RET-class instruction? */
        int is_ret = (op0==0xC9)||(op0==0xC0)||(op0==0xC8)||(op0==0xD0)||(op0==0xD8)||
                     (op0==0xE0)||(op0==0xE8)||(op0==0xF0)||(op0==0xF8)||
                     (op0==0xED && (sms_read8((uint16_t)(g_hz.pc+1))==0x4D||sms_read8((uint16_t)(g_hz.pc+1))==0x45));
        uint16_t sp_before=g_hz.sp;
        z80_step(&g_hz);
        /* Treat it as the function returning only when an actual RET *fired* and
         * raised SP above entry. Requiring SP to have risen this step excludes
         * (a) a bare `pop` (not a ret) and (b) an UNTAKEN conditional ret (SP
         * unchanged) — both of which otherwise false-trip when an earlier pop
         * already lifted SP above entry. */
        if (!g_hz.halted && is_ret && g_hz.sp > sp_before && g_hz.sp > entry_sp) break;
        if (++guard >= 8L*1000*1000) break; }
    state_from_hz(); g_z80.cyc=base+g_hz.cyc;
}
static int diff_compare(const DiffSnap *r, const DiffSnap *i, uint16_t addr){
    int rd = r->z.a!=i->z.a||r->z.f!=i->z.f||r->z.b!=i->z.b||r->z.c!=i->z.c||
             r->z.d!=i->z.d||r->z.e!=i->z.e||r->z.h!=i->z.h||r->z.l!=i->z.l||
             r->z.ix!=i->z.ix||r->z.iy!=i->z.iy||r->z.sp!=i->z.sp;
    int rf=-1; for (int k=0;k<0x2000;k++) if (r->ram[k]!=i->ram[k]){ rf=k; break; }
    /* Both runs start at the same cyc with the VDP frozen, so a cycle delta
     * difference is a pure cycle-count translation bug (the accumulator that
     * drifts the recompiled timeline against the hardware/interpreter). */
    int rc = (r->z.cyc != i->z.cyc);
    if (!rd && rf<0 && !rc){ fprintf(stderr,"[diff] %04X ok (frame %llu)\n",addr,(unsigned long long)g_frame); return 0; }
    fprintf(stderr,"[diff] %04X DIVERGES (frame %llu):",addr,(unsigned long long)g_frame);
    if (rc) fprintf(stderr," cyc{%llu/%llu d=%+lld}",
                    (unsigned long long)r->z.cyc,(unsigned long long)i->z.cyc,
                    (long long)(r->z.cyc - i->z.cyc));
    if (rd){ fprintf(stderr," regs{");
        #define RB(N,X) if(r->z.X!=i->z.X)fprintf(stderr," " N ":%02X/%02X",r->z.X,i->z.X)
        RB("A",a);RB("F",f);RB("B",b);RB("C",c);RB("D",d);RB("E",e);RB("H",h);RB("L",l);
        if(r->z.ix!=i->z.ix)fprintf(stderr," IX:%04X/%04X",r->z.ix,i->z.ix);
        if(r->z.iy!=i->z.iy)fprintf(stderr," IY:%04X/%04X",r->z.iy,i->z.iy);
        if(r->z.sp!=i->z.sp)fprintf(stderr," SP:%04X/%04X",r->z.sp,i->z.sp);
        fprintf(stderr," }"); }
    if (rf>=0){ int n=0; fprintf(stderr," ram{");
        for (int k=rf;k<0x2000&&n<8;k++) if(r->ram[k]!=i->ram[k]){ fprintf(stderr," %04X:%02X/%02X",0xC000+k,r->ram[k],i->ram[k]); n++; }
        fprintf(stderr," }"); }
    fprintf(stderr,"  (recomp/interp)\n");
    return 1;
}
void sms_diff_enter(uint16_t addr){
    if (g_in_diff || !diff_is_target(addr)) return;
    if (g_diff_lo>=0 && ((long)g_frame<g_diff_lo || (long)g_frame>g_diff_hi)) return;
    if (g_diff_max<=0) return;
    g_diff_max--;
    g_in_diff=1; g_diff_freeze=1;
    static DiffSnap S, R, I;               /* static: survive setjmp/longjmp cleanly */
    diff_save(&S);
    g_rtrace_n=g_strace_n=0; g_diff_trace=getenv("SMS_DIFF_TRACE")?1:0;
    g_sync_deadline=(uint64_t)-1; g_diff_icount=0;
    if (setjmp(g_diff_jmp)==0){
        call_by_address(addr);             /* controlled recompiled run of F (longjmps if over budget) */
        diff_save(&R);
        diff_restore(&S); g_sync_deadline=(uint64_t)-1;
        diff_run_super(addr);              /* superzazu run of F from the same S */
        diff_save(&I);
        diff_restore(&S);
        g_in_diff=0; g_diff_freeze=0;
        int d = diff_compare(&R,&I,addr);
        if (d && g_diff_trace){            /* pinpoint the first divergent instruction */
            int n = g_rtrace_n<g_strace_n?g_rtrace_n:g_strace_n, k;
            for (k=0;k<n;k++){ DTrace*a=&g_rtrace[k],*b=&g_strace[k];
                if (a->pc!=b->pc||a->af!=b->af||a->bc!=b->bc||a->de!=b->de||a->hl!=b->hl||a->ix!=b->ix||a->iy!=b->iy||a->sp!=b->sp) break; }
            if (k>0){ DTrace*p=&g_rtrace[k-1];
                fprintf(stderr,"[diff]   first divergent instruction @ PC %04X (entered insn %d). State AFTER it (recomp/interp at next-insn %04X):\n",p->pc,k-1,g_rtrace[k].pc);
                DTrace*a=&g_rtrace[k],*b=&g_strace[k];
                fprintf(stderr,"[diff]     AF %04X/%04X BC %04X/%04X DE %04X/%04X HL %04X/%04X IX %04X/%04X IY %04X/%04X SP %04X/%04X\n",
                        a->af,b->af,a->bc,b->bc,a->de,b->de,a->hl,b->hl,a->ix,b->ix,a->iy,b->iy,a->sp,b->sp);
                int lo = k-6<0?0:k-6;          /* dump the tails of both streams */
                fprintf(stderr,"[diff]   RECOMP tail (n=%d):",g_rtrace_n);
                for(int j=lo;j<g_rtrace_n && j<k+4;j++) fprintf(stderr," %04X(sp%04X)",g_rtrace[j].pc,g_rtrace[j].sp);
                fprintf(stderr,"\n[diff]   INTERP tail (n=%d):",g_strace_n);
                for(int j=lo;j<g_strace_n && j<k+4;j++) fprintf(stderr," %04X(sp%04X)",g_strace[j].pc,g_strace[j].sp);
                fprintf(stderr,"\n");
            }
            g_diff_max=0;                  /* one shot */
        }
        g_diff_trace=0;
    } else {                               /* controlled run never returned -> skip */
        g_diff_trace=0;
        diff_restore(&S);
        g_in_diff=0; g_diff_freeze=0;
        fprintf(stderr,"[diff] %04X skipped (non-returning, frame %llu)\n",addr,(unsigned long long)g_frame);
    }
}
void glue_diff_init(void){
    const char *a=getenv("SMS_DIFF_ADDR");
    if (!a) return;
    g_diff_active=1; g_diff_ntargets=0;
    char buf[512]; snprintf(buf,sizeof buf,"%s",a);
    for (char *t=strtok(buf,",; "); t && g_diff_ntargets<256; t=strtok(NULL,",; "))
        g_diff_targets[g_diff_ntargets++]=(uint16_t)strtoul(t,NULL,16);
    const char *lo=getenv("SMS_DIFF_LO"), *hi=getenv("SMS_DIFF_HI");
    if (lo) g_diff_lo=strtol(lo,NULL,10);
    if (hi) g_diff_hi=strtol(hi,NULL,10);
}

/* ====================== dispatch miss ====================== */
#ifdef SMS_HAVE_JIT
/* Live bus the shard ABI runs against on the game thread (the worker validates
 * shards against a sandbox bus over a snapshot instead). Thin wrappers over the
 * runner's global bus; ctx unused. */
static uint8_t live_r8 (void *c, uint16_t a){ (void)c; return sms_read8(a); }
static void    live_w8 (void *c, uint16_t a, uint8_t v){ (void)c; sms_write8(a, v); }
static uint8_t live_in (void *c, uint8_t p){ (void)c; return sms_io_in(p); }
static void    live_out(void *c, uint8_t p, uint8_t v){ (void)c; sms_io_out(p, v); }
/* shard CALL on the game thread: the live shard runs on &g_z80, so the callee runs
 * on the live dispatcher (Tier 1/2/3) over the same global state. */
static void    live_call(void *c, Z80State *s, uint16_t t){ (void)c; (void)s; call_by_address(t); }
/* shard per-instruction sync on the game thread: advance the VDP + accept a pending
 * IRQ exactly as the static path's sms_tick does (s == &g_z80). */
static void    live_sync(Z80State *s){ (void)s; sms_sync(); }
static const Bus g_live_bus = { live_r8, live_w8, live_in, live_out, live_call,
                                &g_sync_deadline, live_sync, NULL };
#endif

void sms_dispatch_miss(uint16_t addr){
#ifdef SMS_HAVE_JIT
    /* Tier 2: if a trusted shard exists, run it natively and return. Otherwise
     * enqueue an async compile request (non-blocking, deduped — the worker thread
     * does all compilation/validation) and fall through to the interpreter this
     * time. NEVER blocks the game thread (SLJIT.md §2). */
    {
        ShardFn _sh = sms_jit_lookup(addr);
        if (_sh){ _sh(&g_z80, &g_live_bus); return; }
        sms_jit_request(addr);
    }
#endif
    /* Log each newly-seen miss once - this is the static-analysis worklist
     * (PRINCIPLES #16): every address here is a computed target the finder
     * should ideally resolve statically. The hybrid below is the robust
     * fallback so the game keeps running while that work is outstanding. */
    if (!g_miss_seen || !g_miss_seen[addr]){
        if (g_miss_seen) g_miss_seen[addr] = 1;
        g_miss_count++;
        fprintf(stderr, "[dispatch] miss 0x%04X (frame %llu) -> hybrid\n",
                addr, (unsigned long long)g_frame);

        /* Query the always-on entry ring backward for the call chain. The last
         * entry is the function that jumped/called to the bad target. */
        fprintf(stderr, "  entry trail (newest->oldest):");
        for (int i = 1; i <= 12; i++){
            if (g_enter_pos < (uint32_t)i) break;
            uint32_t pos = g_enter_pos - (uint32_t)i;
            fprintf(stderr, " %04X", g_enter_ring[pos & (SMS_ENTER_RING_SIZE - 1)]);
        }
        fprintf(stderr, "\n");

        FILE *f = fopen(g_miss_path, "a");
        if (f){
            fprintf(f, "0x%04X  ; from", addr);
            for (int i = 1; i <= 4; i++){
                if (g_enter_pos < (uint32_t)i) break;
                uint32_t pos = g_enter_pos - (uint32_t)i;
                fprintf(f, " %04X", g_enter_ring[pos & (SMS_ENTER_RING_SIZE - 1)]);
            }
            fprintf(f, "\n");
            fclose(f);
        }
    }

    /* Robust fallback: interpret the routine to completion with live banking. */
    hybrid_interpret(addr);
}

/* ====================== lifecycle ====================== */
bool glue_load_rom(const char *path){
    FILE *f = fopen(path, "rb");
    if (!f){ fprintf(stderr, "[runner] cannot open ROM %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0){ fclose(f); fprintf(stderr, "[runner] empty ROM\n"); return false; }
    g_rom = (uint8_t*)malloc((size_t)sz);
    if (!g_rom){ fclose(f); return false; }
    size_t got = fread(g_rom, 1, (size_t)sz, f);
    fclose(f);
    g_rom_size = got;
    return got == (size_t)sz;
}

void glue_init(bool is_gg, uint64_t frame_limit){
    memset(&g_z80, 0, sizeof(g_z80));
    memset(g_ram, 0, sizeof(g_ram));
    g_bank[0]=0; g_bank[1]=1; g_bank[2]=2;
    g_z80.sp = 0xDFF0;
    g_z80.cyc = 0;
    g_next_line_cyc = SMS_CYC_PER_LINE;
    g_sync_deadline = SMS_CYC_PER_LINE;
    g_frame = 0;
    g_frame_limit = frame_limit;
    g_miss_count = 0;
    free(g_miss_seen);
    g_miss_seen = (uint8_t*)calloc(0x10000, 1);
    remove(g_miss_path);                 /* fresh log per run */
    g_is_gg = is_gg;
    vdp_reset(is_gg);
    g_vdpw_pos = 0;
    vdp_set_write_observer(vdp_write_obs);   /* always-on raster probe */
    psg_init();                          /* fresh PSG state */
    g_psg_cyc = 0;
    g_pad1 = g_pad2 = 0;                  /* controllers idle */
    g_hybrid_cyc = g_hybrid_calls = 0;
    glue_diff_init();                    /* differential harness (env-gated, no-op unless SMS_DIFF_ADDR) */
}

void glue_run_interp(void){
    g_running = true;
    /* Reference CPU = superzazu over the shared glue bus/VDP/IO. */
    z80_init(&g_hz);
    g_hz.read_byte = hyb_read; g_hz.write_byte = hyb_write;
    g_hz.port_in   = hyb_in;   g_hz.port_out   = hyb_out;
    g_hz_init = true;
    g_hz.pc = 0x0000;            /* reset vector; the game sets SP/IM1 itself */

    if (setjmp(g_quit_env) == 0){
        for (;;){
            if (vdp_irq_asserted() && g_hz.iff1 && !g_hz.int_pending)
                z80_gen_int(&g_hz, 0xFF);          /* IM1 vector 0x0038 */
            z80_step(&g_hz); g_frame_ic++;
            advance_vdp(g_hz.cyc);                  /* frame dump/limit/cb here */
            g_sync_deadline = g_next_line_cyc;
        }
    }
    g_running = false;
    fprintf(stderr, "[interp] reference run stopped after %llu frames\n",
            (unsigned long long)g_frame);
}

void glue_run(void){
    g_running = true;
    sms_jit_init();                      /* start the Tier-2 shard worker (no-op unless -DSMS_HAVE_JIT) */
    if (setjmp(g_quit_env) == 0){
        call_by_address(0x0000);         /* reset entry; runs the game */
        /* If we get here the reset routine RETurned (unusual) - just stop. */
    }
    sms_jit_shutdown();
    g_running = false;
    fprintf(stderr, "[timing] frames=%llu irq_taken=%llu (reentrant=%llu) "
            "irq/frame=%.2f sync_maxdepth=%d\n",
            (unsigned long long)g_frame, (unsigned long long)g_irq_taken,
            (unsigned long long)g_irq_reentrant,
            g_frame ? (double)g_irq_taken / (double)g_frame : 0.0,
            g_sync_maxdepth);
    fprintf(stderr, "[exec] hybrid(interp): %llu calls, %llu of %llu Z80 cyc = "
            "%.3f%% interp / %.3f%% static\n",
            (unsigned long long)g_hybrid_calls,
            (unsigned long long)g_hybrid_cyc, (unsigned long long)g_z80.cyc,
            g_z80.cyc ? 100.0 * (double)g_hybrid_cyc / (double)g_z80.cyc : 0.0,
            g_z80.cyc ? 100.0 * (double)(g_z80.cyc - g_hybrid_cyc) / (double)g_z80.cyc : 0.0);
}

void     glue_set_dump(uint64_t frame, const char *path){ g_dump_frame = frame; g_dump_path = path; }
void     glue_set_vdp_trace(const char *path){
    if (g_vdp_trace){ fclose(g_vdp_trace); g_vdp_trace = NULL; }
    if (path){
        g_vdp_trace = fopen(path, "w");
        if (g_vdp_trace)
            fprintf(g_vdp_trace, "frame,vram_h,cram_h,reg_h,r8,r9,r0,r1\n");
        else
            fprintf(stderr, "[runner] cannot open vdp-trace %s\n", path);
    }
}
void     glue_set_frame_callback(int (*cb)(const uint32_t *fb, int w, int h)){ g_frame_cb = cb; }
void     glue_set_audio_sink(void (*sink)(const int16_t *stereo_frames, size_t frame_count)){
    g_audio_sink = sink;
    g_psg_cyc = g_z80.cyc;   /* start metering audio from "now" */
}
uint32_t glue_audio_sample_rate(void){ return psg_sample_rate(); }
double   glue_frame_rate(void){ return (double)SMS_Z80_HZ / (double)SMS_CYC_PER_FRAME; }
void     glue_set_pad1(uint8_t pressed){ g_pad1 = pressed; }
void     glue_set_pad2(uint8_t pressed){ g_pad2 = pressed; }
void     glue_set_input_cb(uint8_t (*cb)(uint64_t frame)){ g_input_cb = cb; }
uint64_t glue_frame_count(void){ return g_frame; }
int      glue_dispatch_miss_count(void){ return g_miss_count; }
size_t   glue_rom_size(void){ return g_rom_size; }
