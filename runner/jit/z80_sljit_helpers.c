/*
 * z80_sljit_helpers.c — flag-exact helpers the emitted shards CALL.
 *
 * Each wraps the SAME z80_ops.h semantic core the generated static C uses, so a
 * shard's result (value + flags on Z80State) is identical to the static path by
 * construction — and the static path is validated against superzazu. Word-sized
 * signatures match the sljit call ABI (SLJIT_ARGS*(W/V, P, W)).
 */
#ifdef SMS_HAVE_JIT

#include "jit_abi.h"
#include "z80_ops.h"

/* INC/DEC r: return the new value (emitter stores it); flags set on s. */
long z80h_inc8(Z80State *s, long v){ return (long)z80_inc8(s, (uint8_t)v); }
long z80h_dec8(Z80State *s, long v){ return (long)z80_dec8(s, (uint8_t)v); }

/* 8-bit ALU on the accumulator: read A from s, compute, write A (+flags) back.
 * CP writes flags only. Indexed by the Z80 ALU group (y field 0..7). */
void z80h_add(Z80State *s, long v){ s->a = z80_add8(s, s->a, (uint8_t)v, 0); }
void z80h_adc(Z80State *s, long v){ s->a = z80_add8(s, s->a, (uint8_t)v, z80f_c(s)); }
void z80h_sub(Z80State *s, long v){ s->a = z80_sub8(s, s->a, (uint8_t)v, 0); }
void z80h_sbc(Z80State *s, long v){ s->a = z80_sub8(s, s->a, (uint8_t)v, z80f_c(s)); }
void z80h_and(Z80State *s, long v){ s->a = z80_and8(s, s->a, (uint8_t)v); }
void z80h_xor(Z80State *s, long v){ s->a = z80_xor8(s, s->a, (uint8_t)v); }
void z80h_or (Z80State *s, long v){ s->a = z80_or8 (s, s->a, (uint8_t)v); }
void z80h_cp (Z80State *s, long v){ z80_cp8(s, s->a, (uint8_t)v); }

/* accumulator rotates (RLCA/RRCA/RLA/RRA) */
void z80h_rlca(Z80State *s){ z80_rlca(s); }
void z80h_rrca(Z80State *s){ z80_rrca(s); }
void z80h_rla (Z80State *s){ z80_rla(s); }
void z80h_rra (Z80State *s){ z80_rra(s); }

#endif /* SMS_HAVE_JIT */
