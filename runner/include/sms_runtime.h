/*
 * sms_runtime.h — shared runtime interface for the SMS/GG recompiler.
 *
 * Defines Z80State (the machine register file the generated C operates on) and
 * the bus / I/O accessor surface the runner implements. Both the recompiled
 * game code (the generated TUs) and the runner include this. The hybrid
 * interpreter path uses the vendored superzazu z80 with the same bus hooks.
 *
 * Memory map (Z80 16-bit address space):
 *   $0000-$BFFF  paged ROM (Sega: first 1KB fixed bank 0; frame regs $FFFC-$FFFF)
 *   $C000-$DFFF  8 KB system RAM
 *   $E000-$FFFF  mirror of system RAM (frame regs live in the top 4 bytes)
 *
 * I/O (port space, via IN/OUT):
 *   $3E memory control   $3F I/O control
 *   $7E V-counter (r)    $7F H-counter (r) / PSG (w)
 *   $BE VDP data         $BF VDP control / status
 *   $DC/$C0 port A       $DD/$C1 port B
 *   GG only: $00 start/region, $01-$05 link, $06 PSG stereo
 */
#ifndef SMS_RUNTIME_H
#define SMS_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Z80 register file ---------------------------------------------------
 * 8-bit registers are stored individually; 16-bit pairs are composed via the
 * helpers below. Flags are stored as one packed F byte (bit layout: S Z Y H X
 * P N C = 7..0) so PUSH AF / POP AF / EX AF,AF' move them verbatim. */
typedef struct {
    uint8_t  a, f, b, c, d, e, h, l;          /* main set */
    uint8_t  a_, f_, b_, c_, d_, e_, h_, l_;  /* shadow set (EXX / EX AF,AF') */
    uint16_t ix, iy, sp, pc;
    uint16_t wz;            /* internal MEMPTR (for exact undocumented flags) */
    uint8_t  i, r;          /* interrupt vector / refresh */
    bool     iff1, iff2;
    uint8_t  im;            /* interrupt mode 0/1/2 */
    bool     halted;
    uint8_t  ei_block;      /* EI just executed: block one maskable-IRQ accept slot
                             * (Z80 accepts no IRQ until after the instruction
                             * following EI). Set by EI, cleared after one insn. */

    uint64_t cyc;           /* T-state accumulator (drives line/frame timing) */
} Z80State;

/* Flag bit masks in F. */
enum {
    Z80_FLAG_C = 0x01,  /* carry */
    Z80_FLAG_N = 0x02,  /* add/subtract */
    Z80_FLAG_P = 0x04,  /* parity/overflow */
    Z80_FLAG_X = 0x08,  /* undocumented (bit 3 of result) */
    Z80_FLAG_H = 0x10,  /* half carry */
    Z80_FLAG_Y = 0x20,  /* undocumented (bit 5 of result) */
    Z80_FLAG_Z = 0x40,  /* zero */
    Z80_FLAG_S = 0x80,  /* sign */
};

/* 16-bit pair accessors. */
static inline uint16_t z80_bc(const Z80State *s){ return (uint16_t)(s->b<<8 | s->c); }
static inline uint16_t z80_de(const Z80State *s){ return (uint16_t)(s->d<<8 | s->e); }
static inline uint16_t z80_hl(const Z80State *s){ return (uint16_t)(s->h<<8 | s->l); }
static inline uint16_t z80_af(const Z80State *s){ return (uint16_t)(s->a<<8 | s->f); }
static inline void z80_set_bc(Z80State *s,uint16_t v){ s->b=(uint8_t)(v>>8); s->c=(uint8_t)v; }
static inline void z80_set_de(Z80State *s,uint16_t v){ s->d=(uint8_t)(v>>8); s->e=(uint8_t)v; }
static inline void z80_set_hl(Z80State *s,uint16_t v){ s->h=(uint8_t)(v>>8); s->l=(uint8_t)v; }
static inline void z80_set_af(Z80State *s,uint16_t v){ s->a=(uint8_t)(v>>8); s->f=(uint8_t)v; }

/* ---- Bus / I/O surface (implemented by the runner) ----------------------- */
uint8_t  sms_read8 (uint16_t addr);
void     sms_write8(uint16_t addr, uint8_t val);
uint8_t  sms_io_in (uint8_t port);
void     sms_io_out(uint8_t port, uint8_t val);

static inline uint16_t sms_read16(uint16_t a){
    return (uint16_t)(sms_read8(a) | (sms_read8((uint16_t)(a+1))<<8));
}
static inline void sms_write16(uint16_t a, uint16_t v){
    sms_write8(a,(uint8_t)v); sms_write8((uint16_t)(a+1),(uint8_t)(v>>8));
}

/* The recompiler runtime globals (defined in glue.c). */
extern Z80State g_z80;

/* Dispatch an arbitrary Z80 address: generated function if known, else hybrid
 * interpreter fallback (and logs a dispatch miss). Returns when the called
 * routine RETs. Used for computed CALL/JP targets. */
void call_by_address(uint16_t addr);

/* HALT: run the machine (advancing time / servicing interrupts) until an
 * interrupt is taken, then return so the generated code continues after the
 * HALT. Implemented by the runner. */
void sms_halt(void);

/* ---- timing / interrupt poll (mirrors the genesis/NES inline poll) -------- *
 * Generated code calls sms_tick() after every instruction. When the cycle
 * counter reaches the next scheduled VDP event (g_sync_deadline), sms_sync()
 * advances the VDP, raises line/frame interrupts, and takes a pending IRQ
 * (calling the IM1 handler) when IFF1 is set. The reset entry never returns;
 * frames are driven by this poll firing inside the running game. */
extern uint64_t g_sync_deadline;   /* g_z80.cyc at which the next event is due */
void sms_sync(void);
void sms_dispatch_miss(uint16_t addr);  /* runner: log an unresolved target */

/* Current ROM bank mapped into the 16 KB slot containing `addr` ($0000/$4000/
 * $8000), or -1 for the RAM region. Generated bank-aware dispatch uses this. */
int  sms_slot_bank(uint16_t addr);

extern int g_diff_freeze; extern uint64_t g_diff_icount; void sms_diff_abort(void);
extern uint64_t g_frame_ic;   /* per-frame instruction count (timing-drift probe) */
/* Pre-body tick: emitted BEFORE every non-block instruction's body. Sync-first -
 * settle the PREVIOUS instruction's end-of-cycle IRQ sample (VDP advanced through
 * that instruction's completion), THEN charge this instruction's cost. This makes
 * the recompiled path accept interrupts at the exact instruction boundary the
 * superzazu interpreter/oracle does - and that real Z80 hardware does: /INT is
 * sampled on an instruction's final T-state and accepted before the next one.
 * Charging before the sync ("pre-pay") advanced the VDP one instruction too far at
 * the sample point, so IRQs landed one instruction early and the timeline drifted
 * ~1 insn/frame from the oracle. The tick CALL still precedes the body (the goto/
 * return lives there); only the sync-vs-charge order inside the tick moved. */
static inline void sms_tick(uint8_t n){
    g_frame_ic++;
    if (g_z80.cyc >= g_sync_deadline) sms_sync();
    g_z80.cyc += n;
    g_z80.ei_block = 0;   /* the EI-delay covers exactly one instruction boundary */
    if (g_diff_freeze && ++g_diff_icount > 3000000u) sms_diff_abort();  /* bound diff test runs */
}
/* Post-body tick: emitted AFTER a repeating block-op iteration's effect (LDIR/OTIR/
 * ... - the one site where a tick follows its body). Charge-first: the iteration's
 * memory/register effect already happened, so charge its cost and sample the IRQ
 * now, matching superzazu which advances the VDP after each block-op iteration. */
static inline void sms_tick_post(uint8_t n){
    g_frame_ic++;
    g_z80.cyc += n;
    if (g_z80.cyc >= g_sync_deadline) sms_sync();
    g_z80.ei_block = 0;
    if (g_diff_freeze && ++g_diff_icount > 3000000u) sms_diff_abort();
}

/* ---- always-on function-entry ring (PRINCIPLES #17) ---------------------- *
 * Each recompiled function records its entry address here on entry. The ring
 * runs continuously; sms_dispatch_miss() (and any future probe) queries the
 * backward window to recover the call chain that led to a bad target - no
 * arm-then-record. Power-of-two size so the mask wraps cheaply. */
#define SMS_ENTER_RING_BITS 10
#define SMS_ENTER_RING_SIZE (1u << SMS_ENTER_RING_BITS)
extern uint16_t g_enter_ring[SMS_ENTER_RING_SIZE];
extern uint32_t g_enter_pos;

extern int g_diff_active;
void sms_diff_enter(uint16_t addr);     /* differential function harness (debug) */
static inline void sms_enter(uint16_t addr){
    g_enter_ring[g_enter_pos & (SMS_ENTER_RING_SIZE - 1)] = addr;
    g_enter_pos++;
    if (g_diff_active) sms_diff_enter(addr);
}

/* Per-instruction PC breadcrumb. Generated code calls SMS_PC(addr) before each
 * instruction; g_dbg_pc holds the address of the instruction currently executing.
 * It is maintained in ALL builds (release too) because the IM1 interrupt accept
 * must push the REAL interrupted PC - games read (and may act on) the stacked
 * return address, so a 0x0000 placeholder diverges them. The store is a single
 * constant-to-global per instruction (not telemetry). -DSMS_TRACE_PC layers the
 * heavier per-instruction differential trace on top. */
extern uint16_t g_dbg_pc;
#ifdef SMS_TRACE_PC
extern int g_diff_trace; void sms_diff_logpc(void);   /* differential instruction-level trace */
#define SMS_PC(a) (g_dbg_pc = (uint16_t)(a), (g_diff_trace ? sms_diff_logpc() : (void)0))
#else
#define SMS_PC(a) (g_dbg_pc = (uint16_t)(a))
#endif

#endif /* SMS_RUNTIME_H */
