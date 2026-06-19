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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/* ---- CPU + memory ---- */
Z80State  g_z80;
uint64_t  g_sync_deadline;

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

/* ---- frame dump ---- */
static uint64_t  g_dump_frame = (uint64_t)-1;
static const char *g_dump_path;
static uint32_t  g_fb[SMS_SCREEN_W * SMS_SCREEN_H];

/* ---- live per-frame callback (SDL host) ---- */
static int (*g_frame_cb)(const uint32_t *fb, int w, int h);

/* ---- dispatch misses ---- */
static uint8_t  *g_miss_seen;        /* 64K dedup bitmap */
static int       g_miss_count;
static const char *g_miss_path = "dispatch_misses.log";

/* ---- always-on function-entry ring ---- */
uint16_t g_enter_ring[SMS_ENTER_RING_SIZE];
uint32_t g_enter_pos;
#ifdef SMS_TRACE_PC
uint16_t g_dbg_pc;
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
    if (p < 0x40) return 0xFF;                           /* control / GG ports */
    if (p < 0x80) return (p & 1) ? vdp_hcounter() : vdp_vcounter();
    if (p < 0xC0) return (p & 1) ? vdp_status_read() : vdp_data_read();
    return 0xFF;                                         /* controllers idle */
}

void sms_io_out(uint8_t p, uint8_t v){
    if (p < 0x40) return;                                /* memory/IO control */
    if (p < 0x80) return;                                /* PSG ($7F) - TODO wire sn76489 */
    if (p < 0xC0){ if (p & 1) vdp_control_write(v); else vdp_data_write(v); return; }
    /* $C0-$FF: GG system / stereo ($06) - TODO */
    (void)v;
}

/* ====================== timing + interrupts ====================== */
static void frame_completed(void){
    g_frame++;
    if (g_dump_path && g_frame == g_dump_frame){
        int nz_vram=0, nz_cram=0;
        for (int i=0;i<0x4000;i++) if (g_vdp.vram[i]) nz_vram++;
        for (int i=0;i<0x40;i++)   if (g_vdp.cram[i]) nz_cram++;
        fprintf(stderr,
            "[vdp] dump@%llu disp=%d r0=%02X r1=%02X r2=%02X r5=%02X r6=%02X "
            "r7=%02X r8=%02X r9=%02X r10=%02X | vram_nz=%d cram_nz=%d line=%d\n",
            (unsigned long long)g_frame, (g_vdp.reg[1]>>6)&1,
            g_vdp.reg[0],g_vdp.reg[1],g_vdp.reg[2],g_vdp.reg[5],g_vdp.reg[6],
            g_vdp.reg[7],g_vdp.reg[8],g_vdp.reg[9],g_vdp.reg[10],
            nz_vram, nz_cram, g_vdp.line);
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
    g_z80.iff1 = g_z80.iff2 = false;     /* Z80 disables interrupts on accept */
    g_z80.halted = false;
    /* push a return address so the handler's RET stays SP-balanced. The real
     * interrupted PC is not tracked in the recompiled model; the C call stack
     * carries the actual return path. */
    g_z80.sp = (uint16_t)(g_z80.sp - 2);
    sms_write16(g_z80.sp, 0x0000);
    call_by_address(0x0038);             /* IM1 vector; RET unwinds back here */
}

/* Advance the VDP up to absolute cycle `cyc`, stepping scanlines and firing
 * frame_completed() at each frame boundary. Shared by sms_sync (recompiled
 * path) and the hybrid interpreter so both drive video timing identically. */
static void advance_vdp(uint64_t cyc){
    while (cyc >= g_next_line_cyc){
        vdp_step_line();
        g_next_line_cyc += SMS_CYC_PER_LINE;
        if (g_vdp.line == 0) frame_completed();
    }
}

void sms_sync(void){
    if (++g_sync_depth > g_sync_maxdepth) g_sync_maxdepth = g_sync_depth;
    advance_vdp(g_z80.cyc);
    if (vdp_irq_asserted() && g_z80.iff1)
        take_irq();
    g_sync_deadline = g_next_line_cyc;
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
    g_hz.int_pending=0; g_hz.nmi_pending=0; g_hz.iff_delay=0;
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
    for (;;){
        if (vdp_irq_asserted() && g_hz.iff1 && !g_hz.int_pending)
            z80_gen_int(&g_hz, 0xFF);    /* IM1: vector 0x0038 (data ignored) */
        z80_step(&g_hz);
        advance_vdp(base + g_hz.cyc);
        g_sync_deadline = g_next_line_cyc;
        if (!g_hz.halted && g_hz.sp > entry_sp) break;   /* routine RETed */
        if (++guard >= guard_max){
            fprintf(stderr, "[hybrid] guard tripped: addr=%04X pc=%04X sp=%04X "
                    "(entry_sp=%04X)\n", addr, g_hz.pc, g_hz.sp, entry_sp);
            break;
        }
    }
    state_from_hz();
    g_z80.cyc = base + g_hz.cyc;         /* reapply rebased absolute time */
}

/* ====================== dispatch miss ====================== */
void sms_dispatch_miss(uint16_t addr){
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
    vdp_reset(is_gg);
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
            z80_step(&g_hz);
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
    if (setjmp(g_quit_env) == 0){
        call_by_address(0x0000);         /* reset entry; runs the game */
        /* If we get here the reset routine RETurned (unusual) - just stop. */
    }
    g_running = false;
    fprintf(stderr, "[timing] frames=%llu irq_taken=%llu (reentrant=%llu) "
            "irq/frame=%.2f sync_maxdepth=%d\n",
            (unsigned long long)g_frame, (unsigned long long)g_irq_taken,
            (unsigned long long)g_irq_reentrant,
            g_frame ? (double)g_irq_taken / (double)g_frame : 0.0,
            g_sync_maxdepth);
}

void     glue_set_dump(uint64_t frame, const char *path){ g_dump_frame = frame; g_dump_path = path; }
void     glue_set_frame_callback(int (*cb)(const uint32_t *fb, int w, int h)){ g_frame_cb = cb; }
uint64_t glue_frame_count(void){ return g_frame; }
int      glue_dispatch_miss_count(void){ return g_miss_count; }
size_t   glue_rom_size(void){ return g_rom_size; }
